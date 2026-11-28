# Raspberry Pi 5 results log

Append-only, like the ESP32 `results.md`: every improvement gets a new dated entry, and old entries are never rewritten.
Board: Raspberry Pi 5 Model B Rev 1.0 (BCM2712, 4× Cortex-A76, 8 GB), Debian 13, kernel 6.18.60-rpt-rpi-2712 (16 KiB pages), gcc 14.2.
Access: `deadname@100.82.121.48` over Tailscale. Plan and rationale: [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md).

---

## Entry P1 — 2026-11-27 — Baselines and hardware probe

### Setup
- Stock OS: `ondemand` governor (clock seen at 1.5–1.7 GHz), no isolation, no IRQ tuning.
- Loopback test. The server is pinned with `taskset` to cpu3 (cpu2–3 for Valkey io-threads 2). redis-benchmark runs with `--threads 2` on cpu0–1.
- Workload: `-r 100000` keys, `-d 32` B values, SET and GET run separately. Median of 3 runs. 4 M requests for P=16, 500 k for P=1 (125 k at c=1).
- Server CPU is measured from `/proc/<pid>/stat`: 100 % means one full core.
- Script: `bench/baseline.sh`. Raw data: `results/baseline_*.csv`.

### Results (requests/s)

| Server | SET c=1 | GET c=1 | SET c=50 | GET c=50 | SET c=50 P=16 | GET c=50 P=16 | srv CPU c=50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Redis 8.0.2 (jemalloc) | 41,611 | 41,611 | 142,816 | 153,799 | 726,612 | 799,520 | 93–98 % |
| Valkey 8.1.1 | 38,414 | 41,611 | 133,298 | 142,816 | 591,979 | 726,876 | 93–99 % |
| Valkey 8.1.1 `--io-threads 2` | 38,402 | 41,611 | 153,799 | 166,611 | 666,445 | 888,691 | 185–193 % |
| microredis (CPython 3.13.5) | 9,960 | 9,921 | 19,960 | 19,960 | n/a | n/a | 92–94 % |

Latency at c=50 P=1: Redis p50 0.29 ms / p99 0.54 ms. Valkey io-threads 2: p99 0.34–0.36 ms.

Notes:
- **c=1 is limited by round trips**: about 24 µs per request. The server sits at 55 % CPU, and Redis, Valkey and io-threads all score the same. That cost is loopback TCP plus two wakeups.
- **At c=50 Redis and Valkey are limited by the server** (one core at about 95 %).
- **Valkey with 2 io-threads uses 2 cores**, but gains only 8–22 %. At that point the 2-thread client is probably the limit.
- **microredis fails on pipelining.** It closes connections on P=16. It also has `MAX_CLIENTS = 8`, which was raised to 128 for this test.
- **Thermal:** 44 → 59 °C, `throttled=0x0` for every run.

### Hardware probe (`bench/hw_probe.c`, performance governor, `results/hw_probe.txt`)

| Measurement | Result |
|---|---|
| Load latency: L1 (≤ 48 KiB) | 1.67 ns |
| Load latency: L2 (128–384 KiB) | 4.8 ns |
| Load latency: L3 (0.75–2 MiB) | 12.8–22.3 ns |
| Load latency: DRAM (≥ 32 MiB) | 107–119 ns |
| Core-to-core cache-line round trip | 143–149 ns (the same for all pairs) |
| `fetch_add` on one shared line | 184.5 M/s with 1 thread; 35.7 / 35.7 / 35.3 M/s total with 2 / 3 / 4 threads |
| Uncontended CAS | 5.84 ns |
| Hash on a 16 B key | FNV-1a + fmix 15.0 ns; hardware CRC32C 2.27 ns |
| memcpy bandwidth | 10.24 GB/s (1 thread) → 9.81 → 9.03 → 8.40 GB/s (4 threads) |

### Takeaways
1. A request is mostly kernel time, not hash-table time. On one core, Redis tops out at about 150 k rps unpipelined and 800 k pipelined.
2. Sharing writable lines between cores is very expensive on this chip: 145 ns per transfer, and atomics stop scaling at 2 threads. The ESP32 "owner" design (no shared data) suits it well.
3. DRAM is 70× slower than L1, and one core already uses all the memory bandwidth. Large key sets need prefetch and batching (memory-level parallelism), not more threads.
4. Hardware CRC32 hashing is 6.6× cheaper than the FNV hash used on the ESP32.

---

## Entry P1b — 2026-11-27 — Dragonfly baseline, and memtier installed

- Downloaded the official Dragonfly arm64 release (`dragonfly-aarch64.tar.gz`, v2.1-1bfc6b18, built 2026-11-04). memtier_benchmark was built from RedisLabs/memtier_benchmark at commit 9173bcd and is available for later entries.
- Same harness and settings as P1 (`bench/baseline.sh`). Dragonfly runs with `--proactor_threads=N --dbfilename=`.

| Server | SET c=1 | GET c=1 | SET c=50 | GET c=50 | SET c=50 P=16 | GET c=50 P=16 | srv CPU c=50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Dragonfly v2.1, 1 thread (cpu3) | 29,377 | 33,289 | 111,086 | 117,592 | 380,337 | 443,754 | 96–99 % |
| Dragonfly v2.1, 2 threads (cpu2–3) | 27,741 | 27,741 | 166,611 | 181,752 | 639,693 | 726,876 | 191–197 % |

- **On one core, Dragonfly trails Redis**: −24 % GET at c=50 and −45 % GET at P=16. Its shared-nothing threading costs extra on every request.
- **With 2 cores it has the best unpipelined GET of all the baselines** (181.8 k vs Valkey io-threads 166.6 k), but still trails Valkey io2 when pipelined.
- **At c=1 it is slower than Redis** (27.7–33.3 k vs 41.6 k), so it has a longer request path. p99 at c=50 P=16 is about 3 ms on 1 thread.
- Thermal: 59.3 °C, `throttled=0x0`.

---

## Entry P2 — 2026-11-28 — Engine port (kvpi v0) and where a request's time goes

### Correction to Entry P1b
The engine benchmark below set the governor to `performance` and did not set it back. So **the Dragonfly numbers in P1b were measured at a fixed 2.4 GHz, not under stock `ondemand`.**
- Rerun under `ondemand`: c=50 results are unchanged (GET 117.6 k with 1 thread, 181.8 k with 2). Under load, ondemand runs at full speed anyway.
- Only the c=1 rows drop, by 10–15 % (GET c=1: 27.7 k / 26.3 k).
- Entry P3 reruns every server in one job under the same governor.

### kvpi engine v0 (`kvpi/engine.c`, snapshot `kvpi/engine_v0.c`)
A direct Linux port of the ESP32 `kv_opt` design:
- **Storage**: one mmap'd arena split into 1 MiB slab pages, slab classes, and per-class free lists, with no malloc on the request path.
- **Index**: open addressing with linear probing. Slots are 8 B, holding a 16-bit tag and a 48-bit ref (the ESP32 used 4 B: 11-bit tag, 21-bit ref). Deletes use backward shift.
- **Other**: in-place overwrite, and the ESP32 hash (FNV-1a + fmix).
- **Test**: `test_engine` runs 3 M random SET/GET/DEL operations against a reference model, with index growth, class changes and deletes. It passes.

**In-process benchmark** (`bench_engine`, 1 thread on cpu3, performance governor, 16 B keys, 32 B values, uniform random; key-generation cost of 24–25 ns subtracted):

| Keys | Where they live | GET ns | GET Mops/s | SET-overwrite ns | Bytes/key (slabs + index) |
|---:|---|---:|---:|---:|---:|
| 10 k | L2 | 42.8 | 23.4 | 47.5 | 118 (mostly empty slab page) |
| 30 k | L3 | 56.6 | 17.7 | 61.5 | 87 |
| 1 M | DRAM | 240.2 | 4.16 | 261.7 | 73 |
| 10 M | DRAM | 266.8 | 3.75 | 284.7 | 70 |

At DRAM sizes a GET costs about 2 dependent misses: the index slot, then the entry.

### kvpi-server v0 (`kvpi/server.c`): single thread, epoll, the same design as Redis
P1 harness, stock `ondemand`, server on cpu3 (median of 3; P=1 rows carry the 0.25 s rounding error explained in P3):

| | SET c=1 | GET c=1 | SET c=50 | GET c=50 | SET c=50 P=16 | GET c=50 P=16 |
|---|---:|---:|---:|---:|---:|---:|
| kvpi v0 | 45,389 | 45,389 | 166,667 | 166,611 | 1,332,889 | 1,230,391 |
| Redis 8.0.2 (P1) | 41,611 | 41,611 | 142,816 | 153,799 | 726,612 | 799,520 |

### Profile of v0 (GET c=50 P=1, `bench/profile.sh`, perf at 4999 Hz with call graphs)
- **73.96 % of server CPU is in the kernel.** The biggest kernel entries are socket wakeup (`_raw_spin_unlock_irqrestore` via `sock_def_readable`), syscall entry (`el0_svc`), `tcp_ack`, skb free/defer, and `__tcp_transmit_skb`.
- **About 5 % is netfilter/conntrack** (`nf_conntrack_tcp_packet`, `nf_hook_slow`, `__nf_conntrack_confirm`, `nf_conntrack_get_tuple_skb`). This is loaded by Tailscale's iptables tables (filter, nat, mangle), so loopback packets are tracked too.
- **`kv_get` is 4.4 %.** Unpipelined, the engine barely matters. The levers are cores and kernel crossings, as in design D1–D4 in OPTIMIZATION_PLAN.md §6.

### memtier check
memtier_benchmark with 2 threads × 25 connections on cpu0–1 is a heavier client than redis-benchmark: 185 k GET versus about 200 k against the same server. It needs `-s 127.0.0.1`, because `localhost` resolves to `::1`. redis-benchmark stays the main load generator.

---

## Entry P3 — 2026-11-28 — Every server rerun with wall-clock timing; kvpi v1 (multi-core, seqlock stripes)

### Why a rerun
- **redis-benchmark rounds its rps to 0.25 s steps.** Its rps figure is n / elapsed, with elapsed rounded to 0.25 s. For example, 500 k requests give 142,816 / 153,799 / 166,611 / 199,920 (3.50 / 3.25 / 3.00 / 2.50 s). So the P=1 numbers in P1, P1b and P2 carry up to ±8 % error.
- **Harness v2 (`bench/baseline.sh`)** times each redis-benchmark process itself in ns and reports `rps_wall`. It also reports **CPU-µs per request** (server CPU ÷ rps), which stays fair when the client is the limit.
- **Runs:** P=1 uses 1 M requests at c=50 and 125 k at c=1. P=16 uses 4 M requests at c=50 and 500 k at c=1. Median of 3 runs.
- **Setup:** all eight servers in one job (`bench/run_all.sh`), stock `ondemand` governor, server on cpu3 (cpu2–3 for the 2-core rows), client on cpu0–1. Temperature 49.9–61.5 °C, `throttled=0x0`. Raw data in `results/baseline_*.csv` and `results/run_all_p3.log`. The rounded P1 CSVs are in `results/old_quantised/` on the Pi.

### kvpi v1 (`kvpi/server_mt.c`), design D1 + D2
- **Threads:** one pinned worker per server core, each with its own `SO_REUSEPORT` listener and epoll. A connection never leaves its core.
- **Data:** 64 stripes. Each stripe is its own engine instance plus one 64 B-aligned sequence word, which serves as both the writer lock (CAS even → odd) and the seqlock for readers.
- **GET:** an optimistic read copies the value straight into the reply buffer. If the sequence changed, the copy is rolled back and retried. Readers write no shared line.
- **Engine changes needed for this (not optimizations):**
  - The index pointer and its mask are published together (the mask is kept in the index header).
  - Old indexes stay mapped after a grow while sharing is on.
  - The per-lookup probe counter is removed, since every reader would be writing a shared line.
  - `kv_get_rd` checks every length against the arena before using it.
- **Stress test** (`kvpi/stress_mt.py`: 8 processes, 64 hot keys, 4 stripes, values 0–9 KB with a CRC in each value, pipelines of 1/4/16, 20 s, 2 server cores): **2,827,655 operations, 0 bad.** 2,753 read retries happened, so the retry path was exercised, and 168,639 writer spins.

### Results (requests/s, wall-clock; CPU-µs per request in brackets)

**1 server core**

| Server | GET c=1 | SET c=50 | GET c=50 | SET c=50 P=16 | GET c=50 P=16 |
|---|---:|---:|---:|---:|---:|
| Redis 8.0.2 | 33,219 | 147,872 [6.64] | 153,626 [6.41] | 725,706 [1.34] | 798,275 [1.20] |
| Valkey 8.1.1 | 35,581 | 137,751 [7.01] | 147,946 [6.51] | 591,090 [1.68] | 725,593 [1.33] |
| Dragonfly v2.1, 1 thread | 31,147 | 110,962 [8.78] | 121,044 [8.23] | 379,925 [2.61] | 443,037 [2.22] |
| **kvpi v0** (1 thread) | **45,247** | **173,613 [5.68]** | **166,422 [5.89]** | **1,329,297 [0.72]** | **1,227,321 [0.80]** |
| kvpi v1, 1 worker | 45,252 | 166,418 [5.86] | 159,771 [6.09] | 1,329,348 [0.74] | 1,139,861 [0.84] |

**2 server cores (cpu2–3)**

| Server | GET c=1 | SET c=50 | GET c=50 | SET c=50 P=16 | GET c=50 P=16 |
|---|---:|---:|---:|---:|---:|
| Valkey 8.1.1, io-threads 2 | 35,578 | 159,732 [12.30] | 166,415 [11.65] | 665,582 [2.98] | 887,078 [2.21] |
| Dragonfly v2.1, 2 threads | 26,236 | 173,587 [11.00] | 190,120 [10.44] | 638,746 [3.01] | 760,273 [2.62] |
| **kvpi v1, 2 workers** | **45,251** | **199,600 [7.26]** | **199,604 [7.46]** | **2,273,633 [0.82]** | **2,274,169 [0.84]** |

Notes:
- **kvpi v1 on 2 cores, unpipelined, is limited by the client.** It runs at 146–149 % server CPU and tops out at 199.6 k, which is the most the 2-core redis-benchmark can drive. Per request it uses 7.3 µs of CPU, against 10.4 µs for Dragonfly and 11.7 µs for Valkey io-threads. The real 2-core headroom can only be measured with an external client (Ethernet, later).
- **Pipelined on 2 cores:** 2.27 M, which is **2.56× Valkey io-threads** and **2.99× Dragonfly** on the same cores, and 1.71× kvpi on 1 core. Scaling is nearly linear because there is no handoff and no shared write on reads.
- **The cost of v1's concurrency on 1 core** (v1 1 worker vs v0): GET c=50 −4.0 %, GET P=16 −7.1 % (seqlock and bounds checks); SET is unchanged.
- **Multi-threaded designs cost more CPU per request than single-threaded ones.** Valkey io-threads 11.65 µs and Dragonfly 10.44 µs, versus Redis 6.41 µs. Their cross-thread handoffs are expensive on this chip (145 ns per line transfer, plus wakeups), which is what the hardware probe predicted.
- **c=1 is purely round-trip latency.** kvpi gives 45.2 k (22 µs per request) against 33.2 k for Redis (30 µs), with p50 0.023 ms vs 0.031 ms.


---

## Entry P4 — 2026-11-28 — Strongest baselines at 1 / 2 / 3 server cores, new load generator (kvload), kvpi v2 (io_uring)

### Baselines added
- **Redis 8.12.0** and **Valkey 9.2.1** (newest), built from their official tags with jemalloc 5.3 (LG_PAGE=14 detected).
- **KeyDB 6.3.4** (Snapchat/KeyDB). Local 1-line patch: `linuxMadvFreeForkBugCheck()` returns early when the page size is not 4 KiB.
  - Without it, KeyDB asserts at startup, because the check is hard-coded to 4 KiB pages and this kernel uses 16 KiB pages.
  - The kernel bug that check detects affects old ARM64 kernels only.
- **Garnet 2.2.14** (official `linux-arm64-based` release) on the .NET 10 runtime, installed with Microsoft's `dotnet-install.sh` into `~/.dotnet`. Default thread pool, restricted by `taskset`.
- **Dragonfly v2.1** (unchanged; it is the latest release).

### Load generator: `bench/kvload.c` replaces redis-benchmark
- **redis-benchmark's rounding is in its exit time, not just its printed rps.** It exits only on its 250 ms timer, so wall-clock timing (harness v2) was also rounded. Pipelined runs lasted 1.759 / 3.009 / 3.509 / 4.511 / 5.262 s, i.e. k × 0.25 s + 9 ms. That is why different servers all landed on 1,329,xxx.
  - So **the P=16 numbers in P3 carry up to about 10 % rounding error**, and P1–P3 P=1 numbers up to about 3 %.
  - The rounded first P4 run is kept in `results/p4_redisbench/`.
- **kvload design:** timed by duration (1 s warm-up, then a 4 s measured window), pinned threads with one epoll each, pre-formatted requests, and a minimal reply scanner. Latency is per batch (send → last reply). It reports client CPU.
- **Client ceiling** (against kvpi v2 on 2 cores):

  | Client cores | redis-benchmark | kvload |
  |---|---:|---:|
  | 1 core, P=1 | ~105 k | 184 k |
  | 2 cores, P=1 | ~200 k | 358 k |
  | 1 core, P=16 | ~1.3 M | 2.49 M |

- **Harness:** `bench/kvbench.sh` + `bench/run_matrix2.sh`. 50 connections in total, 100 k keys preloaded, 16 B keys, 32 B values, GET-only and SET-only, P=1 and P=16. Median of 3 runs; the spread is shown in `results/run_matrix2_p4.log`.
- **CPU split:**

  | Server cores | Server CPUs | Client CPUs (kvload threads) |
  |---|---|---|
  | 1 | cpu3 | cpu0,1 (2) |
  | 2 | cpu2–3 | cpu0,1 (2) |
  | 3 | cpu1–3 | cpu0 (1) |

  Stock `ondemand` governor. Temperature 51–64.8 °C, `throttled=0x0`.
- **Thread counts:** Redis/Valkey `--io-threads N`, Dragonfly `--proactor_threads=N`, KeyDB `--server-threads N`, kvpi one worker per core, Garnet's default pool limited to the given cores.

### kvpi v2 (`kvpi/server_uring.c`)
- v1 with io_uring in place of epoll + read/write: each loop pass is one `io_uring_submit_and_wait`, with `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`, multishot accept, one recv per connection, and double-buffered sends.
- The store and command code moved to `kvpi/store.h`, shared with v1.
- **Stress test:** 983,514 operations, 0 bad, 2,153 read retries.

### Results (ops/s; server CPU-µs per request in brackets; ⚑ = limited by the client, server below 95 % of its cores)

**GET, 50 connections, P=1**

| Server | 1 core | 2 cores (2+2) | 3 cores (3+1) |
|---|---:|---:|---:|
| Redis 8.12 | 149,463 [6.69] | 161,862 [7.26] | 186,967 [12.05] ⚑ |
| Valkey 9.2.1 | 149,465 [6.67] | 163,162 [12.26] | 187,252 [15.49] ⚑ |
| Dragonfly 2.1 | 124,453 [8.01] | 209,012 [9.55] | 188,859 [14.81] ⚑ |
| KeyDB 6.3.4 | 37,254 [26.8] | 125,871 [14.6] | 52,860 [46.3] |
| Garnet 2.2.14 | 41,488 [24.1] | 83,571 [23.9] | 85,080 [34.3] |
| kvpi v1 (epoll) | 162,097 [6.17] | 332,253 [6.02] | 183,432 [7.49] ⚑ |
| **kvpi v2 (io_uring)** | **169,630 [5.89]** | **356,495 [5.60]** | **185,324 [7.17]** ⚑ |

(Redis at 2 cores ran only 118 % busy: one main thread plus a lightly used io-thread, not limited by the client.)

**SET, 50 connections, P=1**

| Server | 1 core | 2 cores | 3 cores |
|---|---:|---:|---:|
| Redis 8.12 | 144,274 | 155,144 | 179,387 ⚑ |
| Valkey 9.2.1 | 142,726 | 160,650 | 180,806 ⚑ |
| Dragonfly 2.1 | 113,466 | 197,084 | 182,174 ⚑ |
| KeyDB 6.3.4 | 29,934 | 121,922 | 46,465 |
| Garnet 2.2.14 | 45,503 | 84,160 | 141,128 |
| **kvpi v2** | **173,495** | **356,289** | **175,836** ⚑ |

**GET, 50 connections, P=16**

| Server | 1 core | 2 cores | 3 cores |
|---|---:|---:|---:|
| Redis 8.12 | 928,291 [1.08] | 1,445,223 [1.29] | 1,655,260 [1.28] |
| Valkey 9.2.1 | 901,310 [1.11] | 1,447,818 [1.38] | 1,613,818 [1.85] |
| Dragonfly 2.1 | 449,593 [2.22] | 789,564 [2.53] | 1,054,725 [2.84] |
| KeyDB 6.3.4 | 372,294 | 671,612 | 729,149 |
| Garnet 2.2.14 | 522,904 [1.92] | 903,709 [2.21] | 1,488,249 [1.99] |
| kvpi v1 | 1,183,333 [0.84] | 2,354,562 [0.85] | 2,529,422 [0.89] ⚑ |
| **kvpi v2** | **1,228,268 [0.81]** | **2,432,156 [0.82]** | **2,525,529 [0.90]** ⚑ |

**SET, 50 connections, P=16**

| Server | 1 core | 2 cores | 3 cores |
|---|---:|---:|---:|
| Redis 8.12 | 782,259 | 1,092,195 | 1,109,289 |
| Valkey 9.2.1 | 720,293 | 1,107,460 | 1,171,707 |
| Dragonfly 2.1 | 386,593 | 688,302 | 924,940 |
| KeyDB 6.3.4 | 324,055 | 568,292 | 624,775 |
| Garnet 2.2.14 | 512,485 | 906,682 | 1,460,921 |
| **kvpi v2** | **1,394,453** | **2,686,411** | **2,149,438** ⚑ |

### Observations
1. **Best baseline per setting:**

   | Setting | Best baseline |
   |---|---|
   | 1 core, P=1 | Redis / Valkey, about 149 k |
   | 2 cores, P=1 | Dragonfly, 209 k GET |
   | 3 cores, P=1 | all about 187 k, limited by the 1-core client |
   | P=16 | Redis 8.12: 1.45 M at 2 cores, 1.66 M at 3 cores |
   | SET at 3 cores | Garnet, 1.46 M |

2. **kvpi v2 against the best baseline:**

   | Setting | kvpi v2 | Best baseline | kvpi v2 vs best |
   |---|---:|---:|---:|
   | 1 core, GET P=1 | 169.6 k | 149.5 k | +13 % |
   | 1 core, GET P=16 | 1.23 M | 928 k | +32 % |
   | 1 core, SET P=16 | 1.39 M | 782 k | +78 % |
   | 2 cores, GET P=1 | 356.5 k | Dragonfly 209 k | +71 % |
   | 2 cores, GET P=16 | 2.43 M | Valkey 1.45 M | +68 % |
   | 2 cores, SET P=16 | 2.69 M | Valkey 1.11 M | ×2.43 |

3. **2+2 is the only setting where every server could be measured at saturation.** kvpi v2 runs both cores at 200 % and scales 2.10× from 1 to 2 cores at P=1. The next best scaling is Dragonfly at 1.68×; Redis manages 1.08× and Valkey 1.09×. Reasons:
   - kvpi's reads never write a shared line, and nothing is handed between threads.
   - Redis and Valkey still run every command on one main thread.
4. **3+1 is limited by the client for kvpi in every test** (server at 128–227 %). That is why kvpi's 3-core numbers are *below* its 2-core numbers: the client has one core instead of two.
   - For the baselines, the 1-core client is the limit unpipelined (about 187 k for everyone).
   - Pipelined, the baselines run 2.0–3.0 cores busy and still trail kvpi: Redis 1.66 M at 212 %, Garnet 1.49 M at 295 %, against kvpi's 2.53 M at 227 %.
   - Server cost per request at 3+1: kvpi 7.2 µs, Redis 12.1, Dragonfly 14.8, Valkey 15.5, Garnet 34.3.
5. **io_uring (v2 vs v1):** +4.6 % at 1 core P=1, +7.3 % at 2 cores P=1, p99 at 2 cores P=1 189 µs vs 322 µs, and +3–4 % pipelined.
   - CPU per request drops 5.89 vs 6.17 µs.
   - Smaller than hoped, because the remaining kernel cost is the TCP work itself, not syscall entry.
6. **KeyDB and Garnet are far behind on this board.**
   - KeyDB: 27–53 µs per request, p99 5–14 ms, unstable (spread up to 234 %). Its global lock and spin-waiting threads hurt on 4 small cores.
   - Garnet: about 24 µs per request unpipelined, with .NET thread-pool scheduling overhead. It scales well pipelined (1.49 M at 3 cores) but is still below Redis.
7. **Scaling of the multi-threaded baselines costs CPU:**
   - Valkey 9 io-threads: 6.67 → 12.26 → 15.49 µs per request as cores are added.
   - Dragonfly: 8.0 → 9.6 → 14.8 µs.
   - kvpi stays at 5.6–7.2 µs.
8. **The fair 4-core number needs an external client** (Ethernet). On one box, 2+2 is the most informative split.


---

## Entry P5 — 2026-11-28 — kvpi v3: A76 compile, CRC32C hash, fast RESP path, batched prefetch

Each step is built on top of the one before (`make v3` in `kvpi/Makefile`). The earlier rungs stay buildable.

| Step | Change |
|---|---|
| v3a | `-O3 -mcpu=cortex-a76` (was `-O2 -march=armv8.2-a+crc`) |
| v3b | + `KV_HASH_CRC`: two hardware CRC32C lanes (`__crc32cd`), 8 B at a time, plus a 64-bit fmix finaliser, replacing FNV-1a + fmix32 |
| v3c | + `KV_FAST` (`proto.h`): RESP array parser with digit loops (no `strtol`/`memchr`) that skips bulk payloads by length; command lookup by an 8-byte lower-cased word; reply integers and bulk headers formatted without `snprintf`/`sprintf` |
| v3d | + `KV_BATCH` (`store.h` `exec_batch`): parse up to 64 complete commands from a read, hash each key, prefetch its stripe word and home index slot, then prefetch every entry behind those slots, then execute in order. Any other command flushes the batch so far first, which keeps reply order. |

**Correctness:**
- `test_engine` passes with CRC32C.
- `stress_mt.py`, 8 processes × 12 s on 2 cores: v3c 2,245,146 operations, 0 bad; v3d 2,255,933 operations, 0 bad.
- A mixed pipelined batch (SET, GET, PING, DEL, lower-case `get`, EXISTS, inline PING) gives identical, correct replies from v3a and v3d.

### Engine benchmark (in-process, cpu3, `performance` governor as in P2; keygen cost subtracted)

| Keys | v0 GET ns | v3a GET ns | v3b GET ns (CRC) | v3b vs v0 |
|---:|---:|---:|---:|---:|
| 10 k (L2) | 43.7 | 46.3 | **34.8** | −20 % |
| 30 k (L3) | 60.8 | 56.2 | **44.1** | −27 % |
| 1 M (DRAM) | 240.2 | 242.1 | **225.6** | −6 % |
| 10 M (DRAM) | 266.4 | 268.6 | **257.3** | −3 % |

- `-O3 -mcpu` (v3a) alone does nothing for the engine. Key generation got faster (25 → 18 ns), which is not engine time.
- CRC saves about 9 ns per key, close to the 12.7 ns hash difference measured in P1. At DRAM sizes the two misses (about 230 ns) dominate.

### Server (kvload harness from P4, stock `ondemand`, median of 3; ops/s with server CPU-µs per request in brackets)

| Variant | 1c GET P=1 | 1c GET P=16 | 1c SET P=16 | 2+2 GET P=1 | 2+2 GET P=16 | 2+2 SET P=16 | 3+1 GET P=16 |
|---|---:|---:|---:|---:|---:|---:|---:|
| v2 (io_uring) | 170,581 [5.86] | 1,229,154 [0.81] | 1,391,420 [0.72] | 357,069 [5.59] | 2,423,162 [0.82] | 2,671,525 [0.75] | 2,520,256 ⚑ [0.90] |
| v3a O3/A76 | 170,514 | 1,230,527 | 1,406,865 | 357,007 | 2,421,058 | 2,684,084 | 2,528,922 ⚑ [0.90] |
| v3b + CRC32C | 171,230 | 1,263,682 [0.79] | 1,448,504 | 358,956 | 2,486,950 [0.80] | 2,756,013 | 2,506,404 ⚑ [0.88] |
| v3c + fast RESP | 175,458 [5.69] | 1,528,614 [0.65] | 1,585,644 | 362,743 [5.50] | 2,959,705 [0.68] | 3,012,367 | 2,505,301 ⚑ [0.75] |
| **v3d + batch prefetch** | **174,426 [5.73]** | **1,761,018 [0.57]** | **1,973,803 [0.51]** | **362,511 [5.51]** | **3,387,761 [0.59]** | **3,589,567 [0.56]** | 2,499,156 ⚑ [0.68] |

⚑ = the 1-core client is the limit (kvload at 100 % of cpu0; server at 144–228 % of 3 cores).

### What each step bought (pipelined, where the engine and user code matter)

| Step | 1c GET P=16 | 2+2 GET P=16 | 2+2 SET P=16 |
|---|---:|---:|---:|
| v3a | +0.1 % | −0.1 % | +0.5 % |
| v3b | +2.7 % | +2.7 % | +2.7 % |
| v3c | **+21.0 %** | **+19.0 %** | +9.3 % |
| v3d | **+15.2 %** | **+14.5 %** | **+19.2 %** |
| v2 → v3d | **+43.3 %** | **+39.8 %** | **+34.4 %** |

- **Unpipelined (P=1), v2 → v3d gains only +2.3 % (1c) and +1.5 % (2+2).** About 74 % of each request is kernel TCP (P2 profile), and v3 changes only user code.
- **The fast RESP path (v3c) was worth more than the hash.** With 16 requests per read, `strtol` + `memchr` per argument and the `strncasecmp` command chain cost more than hashing.
- **Batched prefetch (v3d) is real memory-level parallelism.**
  - The 100 k-key set takes about 8 MB of entries plus 1.6 MB of index, larger than the 2 MB L3, so most lookups miss to DRAM.
  - Prefetching 16 index slots and then 16 entries overlaps those misses.
  - SET gains the most (+19–24 %), because a SET touches the slot, the entry and the stripe word.
- **3+1 is still limited by the client** (about 2.5 M with one kvload core). Server CPU per request falls 0.90 → 0.68 µs at 3 cores, which is the same efficiency gain, just not visible as throughput.
- **At 2+2 with P=16, v3d now uses 161–184 % of the client's 2 cores, so the client is close to becoming the limit there too.**

### Against the best baselines (Entry P4, same harness)

| Setting | Best baseline | kvpi v3d | Ratio |
|---|---|---:|---:|
| 1c GET P=1 | Redis 8.12 / Valkey 9.2.1: 149,465 | 174,426 | 1.17× |
| 1c GET P=16 | Redis 8.12: 928,291 | 1,761,018 | **1.90×** |
| 1c SET P=16 | Redis 8.12: 782,259 | 1,973,803 | **2.52×** |
| 2+2 GET P=1 | Dragonfly: 209,012 | 362,511 | **1.73×** |
| 2+2 GET P=16 | Valkey 9.2.1: 1,447,818 | 3,387,761 | **2.34×** |
| 2+2 SET P=16 | Valkey 9.2.1: 1,107,460 | 3,589,567 | **3.24×** |
| 3+1 GET P=16 | Redis 8.12: 1,655,260 | 2,499,156 ⚑ | ≥ 1.51× (client-limited) |

Thermal: 50.5–66.4 °C, `throttled=0x0`. Raw data: `results/kv_p5_*.csv`, `results/run_p5.log`.


---

## Entry P6 — 2026-11-28 — Network path: OS settings, multishot receive, busy-poll, SQPOLL

Harness: kvload (P4), stock `ondemand`, median of 3. Server code: v3d (`frozen_v4/`, built as v4 with runtime flags).

### N1: OS settings (`bench/ab_notrack.sh`, `bench/ab_sysctl.sh`)

| Setting (cumulative) | 1c GET P=1 | 2+2 GET P=1 | 1c GET P=16 | 2+2 GET P=16 | server µs/req (2+2 P=1) |
|---|---:|---:|---:|---:|---:|
| stock | 173,829 | 362,888 | 1,766,358 | 3,391,251 | 5.50 |
| + loopback conntrack bypass (`iptables -t raw … -i/-o lo -j NOTRACK`) | 188,351 (+8.4 %) | 390,540 (+7.6 %) | 1,838,545 (+4.1 %) | 3,509,867 (+3.5 %) | 5.11 |
| + `net.ipv4.tcp_timestamps=0` | 188,930 (±0) | 390,705 (±0) | 1,840,444 | 3,538,502 | 5.11 |
| + `net.core.skb_defer_max=0` | 195,528 (+3.8 %) | 403,465 (+3.3 %) | 1,902,103 (+3.4 %) | 3,638,314 (+3.7 %) | 4.95 |

- **Kept:** conntrack bypass on `lo` and `skb_defer_max=0`. **Rejected:** TCP timestamps off (no effect).
  - `skb_defer_max=0` frees packet buffers on the current CPU instead of deferring them to the allocating CPU.
- **Mistake, recorded:** the first sysctl script called `sysctl` without `/usr/sbin`, so it could not read the old values and its restore failed.
  - As a result, `skb_defer_max=0` was measured with `tcp_timestamps=0` still set. That makes no difference, since timestamps alone showed ±0.
  - Both settings were then restored by hand to the kernel defaults (1 and 64) and checked. No config file sets them.
- **Profile with the bypass on** (`bench/profile2.sh`, 1c GET P=1): kernel 83.5 %, kvpi 7.3 %.
  - **Netfilter is still 6.9 %.** Tailscale's filter/nat/mangle chains are still walked for every loopback packet. They can't be removed, because Tailscale is how this Pi is reached.
  - **Biggest single item: waking the peer** (`_raw_spin_unlock_irqrestore` via `sock_def_readable`), 8.5 %.
- **All OS changes are temporary** (no persistence) and are undone at the end of each script. Every later comparison applies them to *all* servers.

### N2–N4: io_uring options (`bench/run_n2.sh`; OS settings from N1 on for every run, restored after)

| Variant | 1c GET P=1 | 2+2 GET P=1 | 3+1 GET P=1 | 1c GET P=16 | 2+2 GET P=16 | 2+2 SET P=16 |
|---|---:|---:|---:|---:|---:|---:|
| base (v4 = v3d) | 195,424 [5.11] | 401,717 [4.97] | 203,149 ⚑ [6.24] | 1,872,938 | 3,640,749 | 3,839,254 |
| **multishot recv + provided buffers (`-M 1`)** | **219,800 [4.55]** | 413,717 ⚑ [4.80] | 203,805 ⚑ | **1,968,763** | **3,768,684** | 3,803,173 |
| busy-poll 20 µs | 194,987 | 401,898 | 204,182 ⚑ [8.96] | 1,873,305 | 3,645,521 | 3,910,508 |
| busy-poll 50 µs | 194,782 † | 401,433 | 205,990 ⚑ [11.63] | 1,870,090 † | 3,676,400 | 3,834,274 |
| multishot + busy-poll 50 µs | 220,112 | 414,371 ⚑ | 207,177 ⚑ [11.71] | 1,982,511 | 3,794,067 | 3,596,704 |
| SQPOLL: 1 worker (cpu3) + kernel SQ thread (cpu2) | — | 209,245 (2 cores) | — | — | 3,018,461 (2 cores) | 3,169,322 |

Brackets: server CPU-µs per request. ⚑ = client-limited.

- **Multishot recv: kept, now the v5 default.**
  - 1c P=1 +12.5 %, P=16 +5 %, 2+2 P=16 +3.5 %.
  - It removes the poll re-arm (`io_arm_apoll`) and one SQE per request.
  - At 2+2 P=1 the client (200 %) is the limit, but server cost per request still falls 4.97 → 4.80 µs.
- **Busy-poll: rejected.** No gain at saturation (1c, 2+2). At 3+1, where the server has idle time, it burns 1.4–1.9× the CPU per request for +1 % throughput.
- **SQPOLL: rejected.** One worker plus its kernel polling thread on 2 cores does half the unpipelined throughput of 2 normal workers. On a 4-core board a core is better spent on a worker.
- † **Contaminated run:** `busy50_1c` overlapped a v5 compile on the Pi (02:45:16–02:45:22). Its numbers match `busy20_1c` within 0.3 % and the option was rejected anyway, so it was not rerun.

---

## Entry P7 — 2026-11-28 — kvpi v5: the features needed to be compared fairly

v0–v4 sources are frozen in `kvpi/frozen_v4/`; `make v3` / `make v4` build from there, so earlier numbers stay reproducible. v5 = v4 + the following.

### Engine (`kvpi/engine.c`)
- **Expiry:** an optional 8 B absolute deadline (ms) after the entry header (as on the ESP32).
  - Optimistic readers and writers treat expired entries as absent; writers delete them on access.
  - A sweep in each worker removes them:
    - while busy: one stripe × 4096 index slots per ms, adaptive;
    - after an idle wake (every 10 ms): a whole stripe, in 8192-slot lock holds.
- **Index slot** is now `[ttl:1 | tag:11 | lru:12 | ref:40]` (v4: `[tag:16 | ref:48]`).
  - The `ttl` bit lets the sweep skip keys without a deadline without touching their entries.
- **Memory limit with approximate LRU** (Redis-style `allkeys-lru`; `noeviction` is the default, as in Redis).
  - `lru` is the last access in seconds (mod 4096).
  - Optimistic readers refresh it with a CAS on the index slot, and only when the second has changed. A hot key costs at most one shared-line write per second, and a CAS cannot corrupt a slot that a writer changed meanwhile.
  - At the limit, `kv_put` samples index slots for entries of the slab class it needs, and evicts the oldest of 5 (an expired one first).
- **Slab pages are now 64 KiB** for classes up to 64 KiB (larger classes: one chunk per page). Before this fix they were 1 MiB:
  - a 32 MiB limit gave 64 MiB of slabs;
  - the 100-key stress test held 116 MB of slabs (now 7 MB).
- **Object values** for lists, hashes, sets and sorted sets: the entry value is an 8 B pointer, freed through a callback on overwrite, delete, expiry, eviction and flush.

### Objects (`kvpi/objects.c`)
| Type | Structure |
|---|---|
| list | power-of-two ring buffer |
| hash / set | open-addressing dict with stored hashes and backward-shift delete |
| sorted set | skiplist with spans, ordered by (score, member), plus a member → node dict |

Commands on objects take the stripe writer lock. Optimistic readers never follow object pointers.

### Commands (`kvpi/store.h`), 51 kinds checked against Redis
| Group | Commands |
|---|---|
| Strings & keys | GET, SET [NX\|XX] [EX\|PX\|EXAT\|PXAT\|KEEPTTL] [GET], SETNX, SETEX, PSETEX, GETSET, GETDEL, MGET, MSET, INCR, DECR, INCRBY, DECRBY, APPEND, STRLEN, DEL, UNLINK, EXISTS, TYPE |
| Expiry | EXPIRE, PEXPIRE, EXPIREAT, PEXPIREAT, TTL, PTTL, PERSIST |
| Lists | LPUSH, RPUSH, LPOP/RPOP [count], LLEN, LINDEX, LRANGE |
| Hashes | HSET, HMSET, HGET, HMGET, HDEL, HLEN, HEXISTS, HGETALL |
| Sets | SADD, SREM, SISMEMBER, SCARD, SMEMBERS, SPOP |
| Sorted sets | ZADD, ZINCRBY, ZSCORE, ZREM, ZCARD, ZRANGE [WITHSCORES], ZPOPMIN [count] |
| Admin | PING, ECHO, DBSIZE, FLUSHALL/DB, INFO, SELECT, QUIT |

Server options: `-x <maxmemory MiB> -e lru|noeviction`.

### Correctness
- **`test_engine`** (the P2 model test) passes.
- **`test_features`:**
  - lazy and swept expiry; NX/XX/KEEPTTL; adding and removing a deadline;
  - LRU under an 8 MiB limit: 300 k inserts, 230 k evictions, 0 failed writes, **4,996 / 5,000 hot keys kept vs 56 / 5,000 old cold keys**;
  - noeviction refuses writes at the limit.
- **`difftest.py`: the same random stream of 51 command kinds sent to Redis 8.12.0 and kvpi, every reply compared.**
  - Seeds 1–10 (30 k operations each) and 11–41 (20 k each): **0 mismatches outside DBSIZE.**
  - DBSIZE may count keys that expired in the last few tens of ms and have not been swept yet (≤ 5 keys in the test). Redis's DBSIZE also counts expired-but-unreclaimed keys, just over a shorter window. The test allows ±8 there.
  - Bugs this test found and fixed:
    1. `FAIL()` used `WLIT(c, cond ? A : B)`. `sizeof` of a conditional is `sizeof(char *)`, so the WRONGTYPE reply was cut to 7 bytes and the client hung.
    2. MGET must return nil, not WRONGTYPE, for non-string keys.
    3. The engine clock used `CLOCK_MONOTONIC_COARSE`, which ticks every 4 ms here (HZ=250), so `PX 1` keys lived too long. It now uses `CLOCK_MONOTONIC`, read once per loop pass.
    4. Integer parsing accepted `"07"` and `"+5"`. It now uses Redis's `string2ll` rules.
  - The test itself was made deterministic: TTLs are either far away, or 1 ms followed by a 10 ms pause. Otherwise a deadline landing between the two servers' clocks makes them diverge without any bug.
- **`stress_mt.py`** (8 processes, 64 hot keys, 2 workers): 1.78 M operations, 0 bad values.
- **Real-time expiry through the server:** 2000 keys with EX 1 plus 2000 without. After 2.5 s: DBSIZE 2000, `expired_keys:2000`, the expired key reads nil, the persistent one reads its value.
- **Eviction through the server** (`-x 32 -e lru`, 200 k SETs of 1 KiB at P=16): slabs exactly 32 MiB, 169 k evictions, process RSS 56 MB.


---

## Entry P8 — 2026-11-28 — All servers re-measured under the same OS settings; what v5's features cost

- **Setup:** P4 harness (kvload, 50 connections, 100 k keys, 32 B values, median of 3), stock `ondemand`.
- **OS settings:** the N1 settings (loopback conntrack bypass, `skb_defer_max=0`) were applied to **every** server and restored at the end (`bench/run_matrix3.sh`).
- **Builds:** kvpi v5 = features + multishot; kvpi v4 `-M 1` = the same network path without features.
- **Thermal:** 48–65 °C, `throttled=0x0`. Raw data: `results/kv_p8_*.csv`, `results/run_matrix3_p8.log`.

### GET (ops/s, [server CPU-µs per request]; ⚑ = limited by the client)

| Server | 1c P=1 | 2+2 P=1 | 3+1 P=1 | 1c P=16 | 2+2 P=16 | 3+1 P=16 |
|---|---:|---:|---:|---:|---:|---:|
| Redis 8.12 | 164,147 [6.08] | 177,617 [6.69] | 203,190 ⚑ [11.34] | 954,068 | 1,543,584 | 1,677,345 |
| Valkey 9.2.1 | 160,057 [6.23] | 178,897 [11.19] | 203,937 ⚑ [14.08] | 903,334 | 1,488,905 | 1,621,133 |
| Dragonfly 2.1 | 134,213 [7.45] | 227,555 [8.78] | 205,099 ⚑ [14.11] | 464,893 | 815,879 | 1,054,973 |
| KeyDB 6.3.4 | 49,773 | 164,846 | 57,145 | 366,766 | 666,913 | 745,701 |
| Garnet 2.2.14 | 44,159 | 83,494 | 104,053 | 566,975 | 841,395 | 1,082,345 |
| kvpi v4 (no features) | 220,089 [4.53] | 415,774 [4.79] | 202,475 ⚑ [6.23] | 1,974,955 | 3,734,918 | 2,684,407 ⚑ |
| **kvpi v5 (features)** | **215,548 [4.64]** | **414,995 [4.81]** | 204,630 ⚑ [6.19] | **1,893,723** | **3,553,940** | **2,640,315** ⚑ |

### SET

| Server | 1c P=1 | 2+2 P=1 | 1c P=16 | 2+2 P=16 | 3+1 P=16 |
|---|---:|---:|---:|---:|---:|
| Redis 8.12 | 155,194 | 168,402 | 797,051 | 1,134,699 | 1,114,818 |
| Valkey 9.2.1 | 150,382 | 176,291 | 707,832 | 1,106,875 | 1,172,811 |
| Dragonfly 2.1 | 122,473 | 211,299 | 392,994 | 702,026 | 922,616 |
| Garnet 2.2.14 | 49,533 | 82,166 | 568,363 | 876,470 | 1,039,193 |
| kvpi v4 | 220,496 | 402,592 | 2,240,873 | 3,758,821 | 2,259,045 ⚑ |
| **kvpi v5** | **215,386** | **402,322** | **2,108,117** | **3,471,042** | **2,243,390** ⚑ |

### Findings
1. **Cost of the v5 features** (expiry checks, the LRU stamp, the TTL bit, object dispatch):

   | Test | v5 vs v4 |
   |---|---:|
   | 1c P=1 | −2 % |
   | 1c P=16 GET / SET | −4.1 % / −5.9 % |
   | 2+2 P=16 GET / SET | −4.8 % / −7.7 % |
   | 2+2 P=1 | ±0 (server and client both saturated) |

2. **The OS settings helped the baselines too**, which is why they are applied to everyone: Redis 1c GET P=1 149.5 k → 164.1 k (+10 %), Dragonfly 2+2 209 k → 228 k.
3. **v5 against the best baseline in each setting:**

   | Setting | v5 vs best baseline |
   |---|---|
   | 1c GET P=1 | 1.31× Redis |
   | 2+2 GET P=1 | **1.82× Dragonfly** |
   | 1c GET P=16 | 1.98× Redis |
   | 2+2 GET P=16 | **2.30× Redis** |
   | 2+2 SET P=16 | **3.06× Redis** |
   | 3+1 P=16 | ≥ 1.57× Redis (client-limited) |

   At 3+1 P=1 every server reaches about 204 k, the limit of the 1-core client.
4. **CPU per request** (2+2 P=1): kvpi 4.8 µs, Redis 6.7 µs (one main thread at 119 %), Dragonfly 8.8 µs, Valkey 11.2 µs, KeyDB 11.4 µs, Garnet 24.0 µs.


---

## Entry P9 — 2026-11-28 — Workloads beyond GET-only: mixed, hot keys, large values, TTL, 1000 connections, 10 M keys

**Setup** (`bench/run_workloads.sh`):
- 2+2 split: server on cpu2–3; kvload with 2 threads on cpu0,1, 25 connections per thread (500 for `conn1000`).
- N1 OS settings applied to every server; `ulimit -n 65536`.
- Each server at its 2-core setting: Redis / Valkey `--io-threads 2`, Dragonfly `--proactor_threads=2`, KeyDB `--server-threads 2`, Garnet on 2 cores, kvpi v5 with 2 workers.
- A fresh server per workload, keys preloaded, 3 runs × 4 s, median.
- kvload gained `-z θ` (Zipf, YCSB generator, rank 0 hottest) and `-e s` (every SET carries `EX s`).
- RSS was read after loading. Thermal: max 61 °C, `throttled=0x0`. Raw data: `results/wl_*.csv`, `results/run_workloads_p9.log`.

### Throughput (ops/s), with p99 latency in µs

| Workload | kvpi v5 | Redis 8.12 | Valkey 9.2.1 | Dragonfly 2.1 | Garnet 2.2.14 | KeyDB 6.3.4 |
|---|---:|---:|---:|---:|---:|---:|
| mix 90/10, P=1 | **412,829** (207) | 175,900 (429) | 179,580 (298) | 208,200 (429) | 84,834 | 151,604 (8,026) |
| mix 90/10, P=16 | **3,495,275** (294) | 1,413,087 | 1,426,751 | 776,824 | 871,575 | 636,427 |
| mix 50/50, P=1 | **405,282** | 170,409 | 177,817 | 212,381 | 86,234 | 147,347 |
| mix 50/50, P=16 | **3,421,253** | 1,211,136 | 1,250,192 | 711,013 | 941,793 | 609,443 |
| Zipf 0.99 90/10, P=1 | **408,014** | 175,812 | 180,082 | 209,292 | 94,926 | 171,297 |
| Zipf 0.99 90/10, P=16 | **3,962,794** | 1,430,773 | 1,460,175 | 815,680 | 938,934 | 763,249 |
| Zipf 0.99 50/50, P=1 | **403,895** | 171,991 | 178,329 | 204,247 | 97,901 | 170,869 |
| Zipf 0.99 50/50, P=16 | **3,837,851** | 1,234,582 | 1,287,585 | 747,709 | 853,247 | 707,482 |
| 1 KiB values 90/10, P=1 | **359,668** | 164,754 | 166,121 | 172,226 | 91,340 | 147,050 |
| **16 KiB values 90/10, P=1** | 55,218 [36.2 µs/req] | 50,336 [28.4] | **56,322** [35.5] | 42,618 | 38,168 | 41,881 |
| 50/50 with `SET … EX 100`, P=1 | **396,571** | 166,660 | 175,507 | 193,903 | 86,696 | 159,917 |
| 1000 connections 90/10, P=1 | **232,891** ⚑ (p99 4,432) | 96,856 (20,178) | 102,361 (9,950) | 94,549 (31,155) | 64,066 | 140,742 |
| 10 M keys GET, P=1 | **412,181** | 172,940 | 180,429 | 218,461 | 83,627 | 147,234 |
| 10 M keys GET, P=16 | **3,071,764** | 1,557,108 | 1,419,854 | 759,433 | 960,840 | 530,865 |

⚑ = kvload at 200 % of its 2 cores (the client is the limit); kvpi server at 175 %.

### Memory (process RSS after loading)

| Data set | kvpi v5 | Redis | Valkey | Dragonfly | Garnet | KeyDB |
|---|---:|---:|---:|---:|---:|---:|
| 10 M keys × (16 B key + 32 B value) | 808 MB | 1,046 MB | **796 MB** | 858 MB | 870 MB | 1,319 MB |
| 20 k × 16 KiB values (328 MB of data) | 450 MB | 415 MB | 413 MB | **309 MB** | 537 MB | 421 MB |

### Where kvpi stands
1. **Ahead in 13 of 14 workloads**, by 1.65× to 2.98× over the best baseline in each. The margin is largest when pipelined: 2.5–3.0× with mixed and Zipf traffic.
2. **Hot keys (Zipf 0.99) do not hurt kvpi.** Pipelined it is faster than uniform (3.96 M vs 3.50 M), because hot entries stay in cache. Even 50/50 write-heavy Zipf, where the hot keys' stripe locks see the most traffic, does 3.84 M.
3. **TTL on every SET costs 2 %** (396.6 k vs 405.3 k for plain 50/50).
4. **1000 connections:** kvpi is client-limited at 233 k, with p99 4.4 ms. That matches Little's law for 1000 outstanding requests: 1000 ÷ 233 k = 4.3 ms. Redis and Valkey keep one command thread and fall to 97–102 k; their p99 is 10–20 ms.
5. **Weak spot 1 — 16 KiB values:** kvpi ties Valkey (−2 %) and uses 36.2 µs of server CPU per request, against Redis's 28.4 µs. Suspected cause: the multishot provided buffers are 4 KiB, so a 16 KiB SET arrives as 4+ completions, each copied into a read buffer that has to grow.
6. **Weak spot 2 — memory:**
   - At 10 M keys kvpi uses 1.5 % more than Valkey. Part of it is old index tables kept mapped after each growth (a bounded leak of about the final index size, roughly 128 MB here), kept so optimistic readers can never fault.
   - With 16 KiB values kvpi uses 46 % more than Dragonfly: slab class rounding (classes step by 1/8) plus the unused tail of 64 KiB pages for chunks around 16 KiB.


---

## Entry P10 — 2026-11-28 — kvpi v6: fixing the two weak spots (large values, memory)

### Diagnosis (`bench/profile_big.sh`: v5, 1 worker, 16 KiB values 90/10, perf)
- Kernel 62.6 %, libc 23.4 %, kvpi 11.2 %.
- **Top symbol: `__arch_copy_from_user`, 20.7 %**: the kernel copying our replies into socket buffers.
- **libc `memcpy` ≈ 23 %**, about 8 µs per request, far more than one 16 KiB copy should cost.
- **So the run is limited by memory bandwidth.** About 55 k req/s × 16 KiB × ~6 passes over the data (arena → reply buffer → kernel → client, on both sides) is roughly 5 GB/s on a board that measured 8–10 GB/s total (P1). That is why every server landed at 42–56 k.
- The lever is removing a pass over the data, not saving CPU.

### Changes (v6 = v5 +)
1. **QSBR (quiescent-state-based reclamation)** in `store.h`:
   - Each worker announces a quiescent state once per event-loop pass, and goes offline while it sleeps in io_uring.
   - Retired memory is stamped with a global epoch and freed by its retirer once every online worker has announced a newer epoch.
   - Old index tables are now retired through this, through a new engine hook `kv_set_retire`, instead of being kept mapped forever.
2. **Large values (≥ 8 KiB) in out-of-line, exactly-sized, reference-counted blobs** (`kv_blob_t`). The entry holds one reference.
   - A reader that validated its seqlock takes a reference with `inc-not-zero`. If the count is already 0, the value was just replaced, and the reader re-reads the key.
   - The thread that drops the count to 0 retires the blob through QSBR, which keeps the memory valid for readers still in their pass.
   - Blobs count against `maxmemory` (eviction samples any slab class for them).
3. **Zero-copy reply for blobs:** the reply stream records `(offset, blob)` segments (`wblob` in `proto.h`) and is sent with `sendmsg` iovecs straight from the blob. Short sends advance through the vector. References are released when the kernel has every byte.
4. **Per-class slab pages:** above 4 KiB, each page is a whole number of chunks, so no tail is wasted.
5. **Multishot provided buffers:** 512 × 16 KiB per worker (v5: 1024 × 4 KiB), so a 16 KiB SET arrives in fewer completions.

### Correctness
- `test_engine` (values up to 65 KB, so blobs are covered) and `test_features` pass.
- `difftest.py` against Redis 8.12 now includes 20 000-byte values, which covers blobs through GET/MGET, APPEND, STRLEN, GETSET, `SET … GET`, EXPIRE (entry relayout with a blob pointer) and INCR errors on blobs. **Seeds 51–54: 0 mismatches.**
- Stress test: 1.81 M operations, 0 bad values.
- Expiry (2000 / 2000 reclaimed) and eviction (exactly 32 MiB of slabs) as before.

### Results (2+2, P9 harness, same session for v5 and v6)

| Workload | v5 | **v6** | Change | Best baseline (P9) |
|---|---:|---:|---:|---|
| 16 KiB values 90/10, P=1 | 55,194 [36.10 µs/req] | **74,600 [26.79]** | **+35.2 %** | Valkey 56,322 → **v6 1.32×** |
| 1 KiB values, P=1 | 360,298 | 361,828 | +0.4 % | Dragonfly 172,226 |
| mix 90/10, P=1 | 413,000 | 412,136 | −0.2 % | Dragonfly 208,200 |
| mix 90/10, P=16 | 3,537,679 | 3,545,079 | +0.2 % | Valkey 1,426,751 |
| Zipf 50/50, P=16 | 3,820,957 | 3,797,418 | −0.6 % | Valkey 1,287,585 |
| 1000 connections, P=1 | 234,108 ⚑ | 234,556 ⚑ | +0.2 % | KeyDB 140,742 |
| 10 M keys, P=1 | 412,960 | 414,337 | +0.3 % | Dragonfly 218,461 |
| 10 M keys, P=16 | 3,053,378 | 3,050,997 | −0.1 % | Redis 1,557,108 |

Server CPU per request on 16 KiB values is now **below Redis** (26.8 vs 28.4 µs).

### Memory (RSS after loading)

| Data set | v5 | **v6** | Valkey | Dragonfly | Redis |
|---|---:|---:|---:|---:|---:|
| 10 M × (16 B key + 32 B value) | 804 MB | **699 MB** | 796 MB | 858 MB | 1,046 MB |
| 20 k × 16 KiB values | 454 MB | **349 MB** | 413 MB | **309 MB** | 415 MB |

- **10 M keys: v6 now uses the least memory of all six servers** (−12 % vs Valkey). That is the freed old index tables (QSBR).
- **16 KiB values:** second smallest after Dragonfly. Blobs are exactly sized (only malloc's own small overhead). The remaining gap to Dragonfly is mostly the larger receive buffer pool plus per-connection reply buffers.
- The larger receive buffer pool costs 4 MiB more per worker: the 1 KiB data set uses 145 MB vs 137 MB.

**Note:** v5 was measured from its binary (`build/v5`). Its sources were changed in place to make v6 and were not frozen separately; the v5 → v6 differences are exactly the list above.
