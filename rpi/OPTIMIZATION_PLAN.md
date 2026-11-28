# Raspberry Pi 5: optimization plan for the KV cache

The board is a **Raspberry Pi 5** (BCM2712), not a Pi 4. `/proc/device-tree/model` reads "Raspberry Pi 5 Model B Rev 1.0".

## 1. The machine, measured (`results/hw_probe.txt`)

| What | Value | What it means for a KV store |
|---|---|---|
| Cores | 4× Cortex-A76 r4p1, 1.5–2.4 GHz, out-of-order, no SMT | 4 real cores. The ESP32 had 2 in-order cores at 240 MHz. |
| L1d per core | 64 KiB, 4-way, 64 B lines, **1.7 ns** (4 cycles) | The hot code and the request batch fit here. |
| L2 per core | 512 KiB private, 8-way, **4.8 ns** | Each core's own shard index can live here. |
| L3 shared | 2 MiB, 16-way, **13–22 ns** | The hot set of all shards combined. |
| DRAM | 8 GB LPDDR4X, **~115 ns** dependent load | One miss costs as much as about 70 L1 hits. Above ~2 MiB the store is limited by memory latency. |
| DRAM bandwidth | memcpy: **10.2 GB/s on 1 core, 8.4 GB/s on 4** | One core already uses up the memory system. More cores do not help a workload limited by memory bandwidth. |
| Core-to-core line transfer | **~145 ns** round trip (the same for every pair) | Every cross-core handoff, shared lock or false-shared line costs about this much. The ESP32 had no cache, so this cost did not exist there. |
| Atomics | LSE: uncontended CAS **5.8 ns**; one shared counter falls from 184 M/s (1 thread) to **35 M/s total** (2–4 threads) | Shared atomics stop scaling as soon as there are 2 cores. |
| Hash | FNV-1a + fmix (our ESP hash) **15.0 ns** per 16 B key; hardware CRC32C **2.3 ns** | Hardware CRC32 is 6.6× faster. |
| ISA extras | NEON/ASIMD, dot product, AES, PMULL, SHA1/2, CRC32, LSE atomics, LRCPC | NEON can compare 8–16 tags at once. CRC32 for hashing. LSE for any atomics that remain. |
| Pages | Kernel uses **16 KiB pages** (rpi-2712), no THP sysfs | TLB reach is already 4× that of 4 KiB pages. Larger mappings are still worth testing. |
| NUMA | `numa=fake=8`, interleave policy (Raspberry Pi's SDRAM bank trick) | Every "node" maps to the same 4 CPUs, so there is no real NUMA. Memory is interleaved across banks. |
| Network | Only **WiFi** (wlan0) is up; eth0 has no link | Over the network, WiFi limits everything. Most work is measured on loopback, plus the in-process engine benchmark, as on the ESP32. |
| Default OS | `ondemand` governor (the clock was seen at 1.5–1.7 GHz), PREEMPT kernel, 6.18 | Fixed clocks are needed for repeatable numbers. |

## 2. Baseline to beat (Entry P1)

Loopback test: redis-benchmark with 2 threads on cpu0–1, server pinned to cpu3, 100k keys, 32 B values. Stock OS settings. Median of 3 runs.

| Server | GET c=1 | GET c=50 | GET c=50, P=16 | SET c=50, P=16 | Server CPU at c=50 |
|---|---:|---:|---:|---:|---:|
| Redis 8.0.2 | 41.6 k | 153.8 k | 799.5 k | 726.6 k | ~95 % (limited by the server) |
| Valkey 8.1.1 | 41.6 k | 142.8 k | 726.9 k | 592.0 k | ~95 % |
| Valkey 8.1.1, io-threads 2 (cpu2–3) | 41.6 k | 166.6 k | 888.7 k | 666.4 k | ~190 % (2 cores; the client now limits it) |
| microredis on CPython 3.13 | 9.9 k | 20.0 k | fails (drops pipelined connections) | — | ~93 % |

## 3. Where the time goes in a request

With small values and many clients, the server spends most of each request in the kernel (syscalls, TCP on loopback, epoll), not in the hash table. Redis already does a lookup in well under 100 ns. So the work divides into two separate problems:

- **A. Network and request path** (most of the end-to-end time): syscalls per request, wakeups, copies, scheduling, IRQs.
- **B. Engine** (what the ESP32 work was about): hash, index probe, memory layout, eviction. This matters most for pipelined and batched work, for large key sets that spill to DRAM, and for the in-process benchmark.

## 4. Candidate optimizations

Every item is measured on its own, before and after, as on the ESP32. ⭐ marks the ones expected to matter most.

### 4.1 Parallel-architecture level (the main focus)

| # | Idea | Why it should help on the A76 | How we measure it |
|---|---|---|---|
| P1 ⭐ | **Shared-nothing, one shard per core** (the ESP "owner" design, now with 4 owners). Each worker thread is pinned and owns its shard's index and slab memory. No locks and no shared cache lines on the data path. | A line transfer costs 145 ns and contended atomics give 35 M/s total. Any shared write limits scaling. | Engine Mops/s at 1–4 threads; perf c2c / HITM counts |
| P2 ⭐ | **Connection-to-core with SO_REUSEPORT + one epoll per thread**. The kernel spreads connections across the threads. | Removes the single accept/event loop (Redis) and the io-thread handoff (Valkey). | rps at c=50 against Redis/Valkey using the same number of server cores |
| P3 ⭐ | **Cross-core forwarding when a key's owner is another core**: batched SPSC rings (the ESP ring, but with 64 B-aligned head and tail and batch publish), versus **shared access with optimistic per-bucket version locks** (seqlock reads, CAS writes) | Forwarding pays about 145 ns per handoff, but batching 16–32 requests per handoff spreads that cost. Optimistic reads never write to a shared line. | Both variants, with GET share 50/90/99 % |
| P4 | **Cache-line-aware layout**: the index bucket is exactly 64 B (8 tags plus 8 refs, or 7 plus a version word); per-thread stats padded to 128 B; no false sharing | The ESP32 had no cache, so layout did not matter. Here a false-shared counter costs 145 ns per write. | perf c2c, before and after |
| P5 ⭐ | **Batched lookup with software prefetch** (MICA/CPHash style): parse all requests in a pipelined read, hash them all, prefetch every bucket, then probe all, then prefetch the entries | DRAM costs 115 ns, but the A76 can keep many misses in flight. Overlapping misses turns a chain of dependent misses into parallel ones. | Engine benchmark on 100 M-key or 1 GB sets; P=16 rps |
| P6 | **NEON tag match**: compare the 16-bit tags of the whole bucket in one `vceqq_u16` | One instruction instead of 8 compares and branches. | ns per lookup |
| P7 | **Hardware CRC32C hash** in place of FNV-1a + fmix | 2.3 ns versus 15 ns per key (measured) | Engine benchmark |
| P8 | **Hot-key handling under skew** (the Phase 2 results): replicate read-hot keys to every core, spill writes; redo the ESP mc_bench experiments with 4 cores | With 4 cores, skew is worse: one hot shard is 25 % of the capacity, not 50 %. | Zipf 0.99 load |
| P9 | **Core budget experiments**: how many cores for the server versus the client and IRQs; 2+2 versus 3+1 versus 4 (shared) | There are only 4 cores, and loopback clients also use CPU. | rps per server core |
| P10 | **Lock-free memory reclamation** for shared mode (epoch-based) | Needed if P3 uses shared access. | Correctness and stress tests |

### 4.2 Memory and microarchitecture level

| # | Idea | Why |
|---|---|---|
| M1 ⭐ | **Big arena, mmap'd once, no malloc on the request path**; slab classes (as on the ESP32), but with 64-bit refs and room for GBs | Avoids glibc malloc and jemalloc overhead and their memory use. |
| M2 | **Large pages for the index and arena**: compare 16 K pages, `MAP_HUGETLB` (2 MiB / 32 MiB contiguous ranges, if the kernel offers them), and `madvise` | Sets of hundreds of MB overflow the TLB. |
| M3 | **Key inline next to the value, both in one line when small**; at most 1 DRAM miss for the index and 1 for the entry | A lookup costs 2 misses, about 230 ns. Small entries need to stay within one line. |
| M4 | **Keep the per-core index in its own L2** (size the shards so the hot index part is ≤ 512 KiB) | 4.8 ns versus 115 ns. |
| M5 | **Branch-light parser**, RESP parsing with NEON `\r\n` search | Parsing is a large share of the per-request cost. |
| M6 | **Compiler**: `-O3 -mcpu=cortex-a76`, LTO, PGO (`-fprofile-use`), `-fno-plt` | Free 5–20 %. |
| M7 | **Memory compaction carried over from the ESP32**: prefix compression and compact 4-byte headers. These matter for capacity in bytes per key, not speed. | Bytes per key compared with Redis (Redis has ~50+ B overhead per key). |

### 4.3 OS level

| # | Idea | Why |
|---|---|---|
| O1 ⭐ | **`performance` governor / fixed 2.4 GHz** | Stock `ondemand` sat at 1.5–1.7 GHz. |
| O2 ⭐ | **Pin threads**; server on its cores, client on others; **isolate cores** (`isolcpus`/`nohz_full`, or cgroup cpusets) to keep housekeeping off the server cores | Scheduler jitter shows up in p99. |
| O3 | **IRQ affinity**: WiFi/eth IRQs and softirqs off the server cores, or onto the matching core (RFS/XPS) | Network IRQ work competes with the server. |
| O4 | **Busy-polling**: `SO_BUSY_POLL` / `net.core.busy_poll`, and spin then sleep in the event loop | Avoids sleep and wake costs at high rates. |
| O5 ⭐ | **Fewer syscalls**: read and write in batches per epoll wake, `writev`, edge-triggered epoll, **io_uring** (multishot recv, registered buffers) | The kernel path is the largest cost per request. |
| O6 | **TCP/socket knobs**: `TCP_NODELAY`, buffer sizes, `somaxconn`, `tcp_autocorking` | Latency and tail. |
| O7 | **Memory OS knobs**: `vm.swappiness=0`, `mlockall`, NUMA policy (fake nodes), disable `kswapd` interference | Avoids page faults and swaps on the request path. |
| O8 | **PREEMPT kernel** versus `preempt=none` on the cmdline; real-time priority (`SCHED_FIFO`) for workers | Fewer involuntary context switches. |
| O9 | **Thermal**: watch `vcgencmd get_throttled` during each run | The Pi 5 throttles at about 85 °C. All results stay valid only without throttling. |

### 4.4 Other hardware on the board

| # | Idea | Notes |
|---|---|---|
| H1 | **Gigabit Ethernet** (needs a cable) for honest network benchmarks; WiFi caps throughput far below what the engine can do | Ask whether a cable is available. |
| H2 | **NVMe/PCIe or the SD card** for a persistence tier (AOF/snapshot) | Only if persistence becomes a goal. |
| H3 | **VideoCore GPU**: not useful (no general-purpose compute API for this) | Skip. |
| H4 | **ARM PMU** (`perf stat`: cycles, L1/L2/LLC misses, branch misses) to explain every result | Used throughout. |

## 5. Order of work

1. **Entry P1**: baselines plus the hardware probe. Done.
2. **Entry P2**: port the engine to Linux (`kvpi`); in-process engine benchmark plus a single-thread epoll server. This is directly comparable with Redis on one core.
3. **Entry P3**: OS tuning (O1, O2, O3, O6) applied to every server, to separate tuning from design.
4. **Entry P4**: the engine on the A76: CRC32 (P7), 64 B buckets and NEON tags (P4/P6), arena (M1), batched prefetch (P5), compiler (M6).
5. **Entry P5**: parallel design: shared-nothing on 4 cores with SO_REUSEPORT (P1/P2), forwarding versus optimistic shared access (P3), core budget (P9).
6. **Entry P6**: syscall path: batching, io_uring, busy-poll (O4/O5).
7. **Entry P7**: skew and hot keys on 4 cores (P8).
8. **Entry P8**: final comparison with Redis, Valkey (io-threads), and Dragonfly/KeyDB if they can be installed.

## 6. Design to beat the baselines (written after Entries P1, P1b, P2)

### Targets (stock OS, loopback, 32 B values, 100k keys)

| Core budget | Best unpipelined GET (c=50) | Best pipelined GET (c=50 P=16) |
|---|---|---|
| 1 server core | Redis 153.8 k | Redis 799.5 k |
| 2 server cores | Dragonfly (2 threads) 181.8 k | Valkey (io-threads 2) 888.7 k |

### Fairness rules
- **Same core budget.** The server gets exactly 1 or 2 cores (`taskset`), and the client always gets cpu0–1.
- **Same OS settings for everyone.** Any OS-level change (governor, conntrack, IRQs) is applied to the baselines too, and they are re-measured. Only design changes count as "ours".
- **Report CPU per request as well as throughput.** The client has only 2 cores, so it can become the limit. CPU-µs per request (server CPU ÷ rps) stays fair even then.
- **Repeat runs and check for throttling.** Median of 3 runs, `throttled=0x0`.

### What the v0 profile says (GET c=50 P=1, 1 core)
- **74 % of server CPU is in the kernel**: the loopback TCP stack, syscall entry, epoll, and skb alloc/free. `kv_get` is 4.4 %.
- **Netfilter/conntrack is about 5 %**, loaded by Tailscale's iptables rules (`nf_conntrack`, `nf_hook_slow`, `nft_*`).
- **Each request costs 1 read and 1 write**, plus a share of `epoll_wait`.
- **So the engine only matters when pipelined** (P=16, where v0 is already +54 % over Redis). Unpipelined, the levers are cores and syscalls.

### The design (each step built and measured on its own)

**D1. Multi-core server, connections spread by SO_REUSEPORT (architecture).**
- One worker thread per server core, pinned, each with its own listening socket (`SO_REUSEPORT`) and its own epoll.
- The kernel spreads connections across the workers, and a connection stays on one core for its whole life. There is no accept thread, no handoff, and no io-thread queue.
- Dragonfly and Valkey both pay a cross-thread hop on many requests. This design never does.

**D2. Shared data with optimistic reads, versus owner forwarding (parallel architecture; the key experiment).**
- On the ESP32 the owner design won, because the cores had no caches and a lock round trip cost about as much as a handoff. On the A76 the costs are different:
  - A cross-core line transfer is about 145 ns.
  - A forwarded request has to wake the owner (an eventfd write) and then wait for it, which costs microseconds.
  - Each request already spends about 6 µs in the kernel.
- Main design: one table split into 256 stripes, each with its own engine instance.
  - **GET**: a seqlock read. Read the stripe's sequence number, look up the key, copy the value into the reply buffer, then check the sequence number again; retry if it changed. No shared line is written. Slab memory is never unmapped, and an old index is kept alive after it grows, so a racing reader can only see stale bytes, never fault.
  - **SET**: a per-stripe CAS spinlock plus a sequence bump. With 256 stripes and 2–4 workers, two cores rarely want the same stripe.
- Comparison variant: owner forwarding (the ESP32 design), with batched SPSC rings and eventfd wake. This measures what the handoff costs on this chip.

**D3. Fewer kernel crossings (OS level, per request).**
- io_uring (liburing 2.9): multishot recv, plus every reply submitted in a single `io_uring_enter` per loop iteration instead of one `write` per connection.
- Busy-poll before sleeping, when load is high.
- Expected gain: the syscall part of the 74 % kernel share.

**D4. OS settings (applied to every server, not just ours).**
- Performance governor.
- Conntrack bypass for loopback (`raw` table `NOTRACK` on `lo`). It doesn't persist across a reboot and is removed afterwards.
- IRQ and softirq placement away from the server cores.
- Keep housekeeping work off the server cores.

**D5. Engine for the pipelined case (architecture and memory).**
- CRC32C hash (6.6× cheaper).
- 64 B buckets with NEON tag compare.
- Batch-prefetch every key in a pipelined read before probing (MICA style).
- Precomputed reply headers, and a faster RESP parser.
- `-O3 -mcpu=cortex-a76`, plus LTO/PGO.

**D6. Skew (Phase 2 work, on 4 cores).** Zipf-distributed keys with memtier, and hot-stripe handling.

## 7. Strongest baselines, and how each one uses multiple cores

The comparison is only meaningful against the strongest Redis-compatible servers, each tuned to its best thread count. Each one uses multiple cores in a different way:

| Server | How it uses cores | Data sharing | Status |
|---|---|---|---|
| Redis 8.x (newest) | One main thread runs every command. Optional `io-threads` do socket read/write and parsing. | None (one thread) | Debian 8.0.2 measured; newest to build from source; test io-threads 1–3 |
| Valkey 9.x (newest) | Main thread plus async io-threads. Batches dictionary lookups with memory prefetch (since 8.0). | None (one command thread) | Debian 8.1.1 measured; newest to build from source; io-threads 1–3 |
| Dragonfly | Shared-nothing: one shard per thread, fibers, io_uring. A command for a key on another thread hops there (VLL transactions). | Partitioned, with message passing | v2.1 measured; test 1–3 threads |
| Garnet (Microsoft) | Multi-threaded network layer over Tsavorite (descended from FASTER [5]). Latch-free shared hash index with epoch protection. | Shared, latch-free | To install (needs .NET); 1–3 threads |
| KeyDB | Multi-threaded Redis fork; all threads share one dataset behind a global spinlock | Shared, one lock | To build; 1–3 threads |
| kvpi v1 (ours) | SO_REUSEPORT worker per core, no handoff; 64 seqlock stripes | Shared, optimistic reads | Measured; 1–3 cores |

**Core budget rule while the client is on the same Pi:** the server gets N = 1, 2 or 3 cores, and the client gets the rest. A setting that makes the client the limit is reported with CPU-µs per request. The full 4-core comparison needs an external client over Ethernet (later).

## 8. Parallel-architecture optimizations: brainstorm

What the measured hardware means (P1): 4 equal A76 cores, private L1/L2, a shared 2 MiB L3, **145 ns** per cache-line transfer between cores, contended atomics capped at **35 M/s total**, and about **6 µs of kernel time** in each unpipelined request. Every idea below is judged against those numbers.

### 8.1 Threading model: who touches which data (the main question)
1. **Shared data, optimistic reads (v1, done).** Any core serves any key. GETs write no shared line. A SET writes one stripe word, so one line transfer (about 145 ns) when another core reads that stripe next. That is about 2 % of a 6 µs request.
2. **Shared-nothing with forwarding (Dragonfly; the ESP32 owner design).** A key on another core costs a ring push, a wake (eventfd, about 1–3 µs when the owner sleeps) and a reply. With N cores, (N−1)/N of keys are remote. Prediction: worse than (1) unpipelined, unless requests are batched per hop. To be built as v1-owner to measure it.
3. **Single writer per stripe, readers on any core.** No CAS at all on SET, but a write on another core must be forwarded. This combines the lock-free writes of (2) with the free reads of (1). Best for GET-heavy work; costs extra hops on SET-heavy work.
4. **Steer the connection to the data.** With an SO_REUSEPORT eBPF program (`SO_ATTACH_REUSEPORT_EBPF`), or by migrating a connection once its key pattern is known (sticky clients, multi-tenant prefixes), a connection lands on the core that owns its keys. That makes (2) free of hops for well-partitioned clients.

### 8.2 Synchronisation cost
5. **Stripe count versus false conflicts.** 64 stripes now. Measure 16 to 4096, and per-bucket version words inside the index (no separate lock line).
6. **No locked read-modify-write on the read path** (done). Only SET uses an LSE CAS, at 5.8 ns uncontended.
7. **Flat combining for write-hot stripes.** When one stripe is contended, one core applies a batch of SETs for every waiting core. This fixes write skew to a single key (Phase 2's hot key) without spinning.
8. **Memory reclamation by quiescent states (QSBR).** Each worker announces "quiescent" once per event-loop pass. Old indexes and freed large chunks are released after every worker has passed. This replaces today's "never unmap" rule.
9. **No false sharing:** per-thread stats are 128 B aligned (done), connections stay with their thread, and counters on a hot shared line are never updated per request.

### 8.3 Parallelism inside a core (memory-level and SIMD)
10. **Memory-level parallelism (MLP) for pipelined batches.** Hash all keys in a read, prefetch every index slot, then every entry. The A76 keeps many misses in flight, so N dependent 115 ns misses become about one. Valkey 8 does this, and it is why its pipelined throughput is high. Add an MLP probe (k independent pointer chains) to measure how many misses really overlap.
11. **NEON:** compare a 16-tag bucket with one vector instruction (64 B bucket), find `\r\n` in 16 B steps for RESP, and assemble replies with vector copies.
12. **CRC32C hash:** 2.3 ns versus 15 ns (measured). It also frees issue slots for overlapping misses.

### 8.4 Parallelism between user code and the kernel
13. **io_uring with SQPOLL:** a kernel thread polls the submission queue, so the worker makes no syscalls. It costs a core, which is the trade-off the core budget measures. Compare with plain io_uring (one `io_uring_enter` per loop) and epoll.
14. **Batched replies per loop:** collect the writes of every ready connection into one submit.
15. **Busy-poll then sleep:** spin for a few µs before `epoll_wait` or `io_uring_wait`. This avoids the wake cost of about 2 µs at high rates.
16. **Core specialisation:** compare 1 network core + N engine cores with every core doing both (Redis io-threads versus v1). Here the network share of a request is 74 %, which may favour "all cores do everything".

### 8.5 Placing work for a real NIC (Ethernet, later)
17. **RSS/RPS/RFS/XPS:** spread receive softirqs across cores, and run each connection's softirq on the core that owns it. `SO_INCOMING_CPU` plus the reuseport eBPF steering from item 4 makes the receive path stay on one core.
18. **IRQ affinity** matched to the workers. The core budget accounts for the softirq cost.

### 8.6 Skew across 4 cores (Phase 2, again)
19. Uniform, then Zipf 0.99 load with memtier.
    - **With shared reads (v1), read skew needs no balancing**, because every core can serve a hot key.
    - **Write skew** to a few keys is handled by flat combining (7) or by splitting hot keys per core.
    - Redo Phase 2's spill-over and replication results with 4 cores in mind.

### 8.7 OS scheduling for parallel workers
20. `isolcpus`/`nohz_full` or cpusets for the server cores, `SCHED_FIFO` workers, and housekeeping and kthreads kept elsewhere. This mostly improves p99, not throughput.
