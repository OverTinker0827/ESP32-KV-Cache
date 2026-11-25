# Results log — hardware-aware KV cache on ESP32

**This file is append-only.** Each entry is dated and never edited afterwards; corrections and
improvements are added as new entries at the bottom. Raw data for every entry is in `results/`.

## Setup (common to all entries unless an entry says otherwise)

| Item | Value |
|---|---|
| Board | ESP32-D0WD-V3 rev 3.1 (classic ESP32, **not** S3): 2× Xtensa LX6 @ 240 MHz, 520 KB SRAM, 4 MB flash, **no PSRAM**, CP210x on COM5 |
| Baseline | [mateuszsury/microredis](https://github.com/mateuszsury/microredis) @ `8bde79f`, precompiled to `.mpy` (`mpy-cross 1.29.0 -O1`), on MicroPython v1.29.0 `ESP32_GENERIC` ([firmware](https://micropython.org/resources/firmware/ESP32_GENERIC-20260824-v1.29.0.bin)) + `bisect` from micropython-lib |
| C toolchain | ESP-IDF v5.5.6, GCC 14.2 (xtensa-esp-elf), `-O2`, FreeRTOS SMP @ 1000 Hz |
| Clock | 240 MHz for every variant (MicroPython defaults to 160 MHz; set explicitly) |
| Workload | 16 B keys `key:%012d` (sequential) or 16 B random hex, 32 B values |

Reproduce: `python tools/build_mpy.py` (microredis), `.\tools\build_variant.ps1 -Name <n> -Cfg <fragments> -Flash` (C),
`python tools/serial_wait.py COM5 DONE 60` (read the `RESULT` line).

---

## Entry 1 — 2026-11-25 — Baselines: microredis vs basic C vs optimized C (on-device engine)

**What was measured:** the storage engine alone, on the ESP32, with WiFi off: 500 keys SET, then GET,
then DEL (C: averaged over 20 repetitions), then insert distinct keys until the store refuses.
Network benchmarks are **not** in this entry (see Blockers).

### Variants

| Variant | Engine | Notes |
|---|---|---|
| microredis | Python dicts (5 per keyspace: data, types, expires, version, last_access) | upstream, 3 compile/runtime patches (below) |
| basic C | one `malloc` per entry, chained hash table that doubles, FNV-1a, FreeRTOS mutex | "first straightforward port" |
| optimized C | preallocated 4 KiB slab arena + 4 B/slot open-addressing index (11-bit tag, fastrange, backward-shift delete), FNV-1a + fmix32, CLOCK eviction, hot path in IRAM | `variants/opt.cfg` |

### Throughput and latency (sequential keys)

| Metric | microredis | basic C | optimized C | opt vs basic | opt vs microredis |
|---|---:|---:|---:|---:|---:|
| SET ops/s | 922 | 80,874 | **441,423** | 5.5× | **479×** |
| GET ops/s | 1,480 | 40,306 | **361,193** | 9.0× | **244×** |
| DEL ops/s | 2,356 | 99,880 | **395,867** | 4.0× | **168×** |
| SET µs/op | 1,085 | 12.4 | **2.27** | | |
| GET µs/op | 676 | 24.8 | **2.77** | | |
| DEL µs/op | 424 | 10.0 | **2.53** | | |

Random-hex keys give the same C numbers (±0.3 %): the C hashes have no key-pattern sensitivity.

### Memory

| Metric | microredis | basic C | optimized C |
|---|---:|---:|---:|
| Max keys (16 B key / 32 B value) | 1,223 | 3,664 | **3,942** |
| Memory holding the store | ~188 KB GC heap (also holds 107 KB of interpreter/code objects) | 265.6 KB of heap | 244.1 KB (216 KB arena + 22.9 KB index) |
| Bytes per key (store memory / max keys) | ~153 (upper bound) | 72 | **62** |
| Heap left for the rest of the system when full | ≈0 (MemoryError) | 8 KB (safety stop) | **40 KB reserve guaranteed** |
| Heap fragmentation on the request path | yes (MemoryError on 13 KB dict resize at < 2000 keys) | yes (`malloc` per SET) | **none** (no allocation per request) |

### Findings

1. **microredis does not run on MicroPython as published.** It needed: `const(ord('+'))` → literals
   (`core/constants.py`), removal of `const()` reassigned in `except` (`commands/__init__.py`),
   `del bytearray[:]` → rebind (`core/response.py`), and `bisect` from micropython-lib. None touch GET/SET.
   Patches are applied at build time by `tools/build_mpy.py`; upstream checkout is untouched.
2. **microredis spends 107 KB of the ~164 KB MicroPython heap on code**, leaving 39.5 KB for data
   before WiFi starts.
3. **Sequential keys are pathological for MicroPython dicts**: `key:%012d` hashes into 230 of 1024 low-bit
   buckets; a bare dict GET runs at 6,962/s on sequential vs 66,471/s on random keys (9.5×).
   The C engine's FNV-1a + murmur3 fmix32 is insensitive to key pattern.
4. **One optimized-engine operation costs ~2.5 µs (~600 cycles).** Inter-task handoff on FreeRTOS
   (queue + context switch) and lwIP socket calls cost more than that, so any multicore design must
   add almost no coordination per request to win — the hypothesis for the next experiments.
5. The optimized engine keeps 21 KB less memory for itself than the basic one (40 KB reserve for
   WiFi/lwIP) and still stores 7.6 % more keys, because entries are packed with 8 B headers into
   8-byte-granular slab classes and the index costs 4 B per slot.

### Blockers / not yet measured

- **Network benchmarks (throughput, p95/p99 latency, CPU per core) are blocked**: the board's 3.3 V
  supply collapses when the radio starts (brownout reset on USB *and* on a wall charger; with the
  brownout detector disabled the chip hits a watchdog reset instead). Needs a 100–470 µF capacitor on
  3V3–GND or a different board. Client (`tools/bench.py`) and servers are ready.
- **Energy per 1,000 requests** needs a current sensor (INA219) or USB power meter.
- **PSRAM tiering** cannot be tested on this board (no PSRAM).

Raw data: `results/microredis_engine_ondevice.json`, `results/engine_ondevice_summary.json`,
`results/serial_20261125_130955.log` (basic C), `results/serial_20261125_131413.log` (optimized C).

---

## Entry 2 — 2026-11-25 — Attribution of the speed-ups, tail latency, simulated WiFi load

**What changed in the method (benchmark v2):**
- Every operation is timed individually with the CPU cycle counter (C) or `ticks_us` (MicroPython):
  p50 / p99 / p99.9 / max per operation, not just throughput.
- The C benchmark runs in a dedicated task pinned to a chosen core (v1 ran inside the main task).
- Four scenarios per C variant: `quiet` (core 1), `quiet_core0`, `sim_other_core` (bench core 1,
  load core 0) and `sim_same_core` (bench and load on core 0).
- **Simulated WiFi load** (`kvcache/main/wifisim.c`), because the board cannot power the radio:
  a priority-22 task that, per millisecond, uses 40 % of its core (measured: 40 %), copies two
  1600 B heap buffers, reads 2 KiB of flash (cache-line misses) and takes 2,000 timer interrupts/s.
  This is a stand-in, **not yet calibrated against real WiFi**; its bursts are 0.8 ms long.
- Ablation switches: `CONFIG_KV_BASIC_FAST_ITOA` (basic GET without `snprintf`),
  `CONFIG_KV_FORCE_LOCK` (spinlock on every op), `CONFIG_KV_HOT_IRAM=n` (hot path left in flash).
- microredis measured on sequential **and** random keys.

### Throughput and latency (16 B keys, 32 B values; latency in µs)

| variant | scenario | SET/s | GET/s | DEL/s | SET p50/p99/p99.9/max µs | GET p50/p99/p99.9/max µs | rand GET/s |
|---|---|---:|---:|---:|---:|---:|---:|
| microredis | quiet, seq keys | 891 | 1,585 | 2,188 | 711 / 4,261 / – / – | 200 / 4,027 / – / – | |
| microredis | quiet, random keys | 1,940 | 6,474 | 4,276 | 397 / 1,880 / – / – | 106 / 553 / – / – | |
| basic | quiet | 80,792 | 76,995 | 99,698 | 12.21 / 16.32 / 95.72 / 103.61 | 12.82 / 17.05 / 18.38 / 116.22 | 77,019 |
| basic | quiet_core0 | 80,895 | 76,927 | 99,707 | 12.21 / 17.39 / 18.19 / 79.67 | 12.82 / 18.16 / 18.43 / 104.32 | 76,987 |
| basic | sim_other_core | 78,460 | 75,586 | 96,504 | 12.21 / 17.00 / 22.34 / 31.35 | 12.82 / 17.05 / 20.43 / 20.70 | 75,538 |
| basic | sim_same_core | 45,909 | 41,080 | 57,362 | 12.21 / 834.31 / 858.35 / 862.04 | 12.82 / 839.08 / 950.47 / 1172.73 | 41,293 |
| basic_fastitoa | quiet | 80,813 | 129,760 | 99,736 | 12.21 / 16.89 / 87.64 / 98.85 | 7.58 / 11.34 / 12.34 / 51.10 | 129,659 |
| basic_fastitoa | quiet_core0 | 80,872 | 129,617 | 99,715 | 12.21 / 17.74 / 18.36 / 75.69 | 7.58 / 12.30 / 13.16 / 51.10 | 129,334 |
| basic_fastitoa | sim_other_core | 78,414 | 125,981 | 96,610 | 12.21 / 18.00 / 22.45 / 38.58 | 7.58 / 11.61 / 15.64 / 16.07 | 125,767 |
| basic_fastitoa | sim_same_core | 46,070 | 73,825 | 57,684 | 12.21 / 835.58 / 879.26 / 895.01 | 7.58 / 13.69 / 865.23 / 880.88 | 73,728 |
| opt_noiram | quiet | 430,551 | 356,811 | 390,441 | 2.26 / 2.52 / 6.14 / 147.68 | 2.73 / 2.80 / 6.79 / 62.21 | 357,717 |
| opt_noiram | quiet_core0 | 427,003 | 356,328 | 390,243 | 2.27 / 2.47 / 7.53 / 143.70 | 2.73 / 2.80 / 8.09 / 57.39 | 357,500 |
| opt_noiram | sim_other_core | 432,021 | 356,544 | 390,976 | 2.26 / 2.45 / 6.12 / 24.36 | 2.73 / 2.83 / 8.17 / 30.59 | 357,091 |
| opt_noiram | sim_same_core | 232,780 | 208,711 | 218,054 | 2.27 / 5.94 / 824.81 / 828.64 | 2.73 / 6.47 / 833.17 / 833.18 | 208,550 |
| opt_forcelock | quiet | 317,309 | 274,989 | 293,470 | 3.09 / 4.12 / 7.21 / 25.24 | 3.56 / 4.26 / 7.60 / 7.60 | 275,042 |
| opt_forcelock | quiet_core0 | 316,826 | 274,627 | 293,083 | 3.09 / 4.24 / 8.44 / 25.24 | 3.56 / 4.28 / 8.90 / 8.90 | 274,725 |
| opt_forcelock | sim_other_core | 306,241 | 267,838 | 287,001 | 3.09 / 4.37 / 9.45 / 9.45 | 3.56 / 4.44 / 9.64 / 9.67 | 268,197 |
| opt_forcelock | sim_same_core | 184,236 | 160,125 | 164,222 | 3.09 / 7.01 / 833.25 / 836.94 | 3.56 / 7.56 / 834.64 / 837.82 | 160,064 |
| **opt** | **quiet** | **434,046** | **357,079** | **390,579** | 2.25 / 2.88 / 6.12 / 24.39 | 2.73 / 3.15 / 6.85 / 6.86 | 357,117 |
| opt | quiet_core0 | 433,538 | 356,341 | 390,137 | 2.25 / 3.04 / 7.43 / 24.39 | 2.73 / 3.17 / 8.08 / 8.09 | 356,735 |
| opt | sim_other_core | 416,996 | 349,650 | 386,279 | 2.25 / 2.96 / 6.05 / 6.17 | 2.73 / 3.28 / 8.46 / 8.47 | 350,127 |
| opt | sim_same_core | 250,928 | 204,015 | 227,464 | 2.25 / 5.83 / 824.32 / 832.32 | 2.73 / 6.39 / 828.91 / 840.58 | 207,511 |

(microredis has 1,000 samples per operation, so no p99.9/max; C variants have 2,000.)

### Capacity

| variant | max keys | bytes/key |
|---|---:|---:|
| microredis | 1,223 | 77 (`gc.mem_alloc` delta; excludes the 107 KB of code objects) |
| basic / basic_fastitoa | 3,659 | 68 |
| opt / opt_noiram / opt_forcelock | 3,869 | 60 |

### Why it got faster — in brief (now measured, not estimated)

**microredis → basic C (46–91×: SET 91×, GET 49×, DEL 46×, sequential keys)**
1. **Interpreter overhead:** an empty MicroPython loop iteration costs ~2 µs (~500 cycles); a C call costs a few cycles.
2. **Work per command:** microredis touches its dicts 6–7 times per SET and ~5 times per GET
   (5 dicts: data, types, expires, version, last_access; GET even inserts into `last_access`). C does one lookup.
3. **Hash clustering on sequential keys:** random keys make microredis **2.2× faster on SET, 4.1× on GET,
   2.0× on DEL** — real, but smaller than first estimated. Even on random keys the optimized C engine is
   **224× (SET) and 55× (GET) faster**, so interpreter + per-command work is the larger factor.
4. **GC pauses:** microredis GET p99 is ~4 ms against a p50 of 0.2 ms.

**basic C → optimized C (3.9–5.4×: SET 5.4×, GET 4.6×, DEL 3.9×)**
1. **`snprintf` in the GET reply:** ~5.2 µs per GET (basic GET 12.82 → 7.58 µs p50, +69 % GET/s when replaced).
2. **Locking:** a FreeRTOS mutex in basic; the optimized single-owner engine takes no lock. Forcing a
   spinlock costs **+0.84 µs per op (−27 % SET/s, −23 % GET/s)**: the bar any shared-table multicore design must clear.
3. **`malloc`/`free` per entry + pointer-chasing chains** vs a free-list pop and one flat 4 B/slot index with
   an 11-bit tag: the remaining ~6–7 µs per SET/DEL.
4. **IRAM placement:** no effect when quiet (+0.8 %); **+7.8 % SET/s when WiFi-like load shares the core**
   (flash cache being evicted); **−3.5 % when the load is on the other core** (hypothesis: IRAM instruction
   fetch competes for the SRAM bus with the other core's buffer traffic, while flash-cached code is fetched
   from each core's private cache — to verify).

### Other findings

1. **Pinning isolates the cache from WiFi-like load:** load on the other core costs the optimized engine
   only −4 % SET / −2 % GET, and no p99.9 change. Load on the same core costs −42 % and pushes p99.9 to
   ~830 µs (the simulator's burst length) for every variant. Argument for keeping the cache off core 0.
2. **A flash-cache line miss costs ~4 µs (~900 cycles)** (32 B over 40 MHz DIO flash): the first simulator
   setting (16 KiB of flash reads per ms) overran its whole time budget on misses alone.
3. **No core asymmetry:** core 0 and core 1 give the same results when quiet.
4. **p99 alone hides the worst cases:** with 2–3 µs operations, fewer than 1 % of operations hit a 0.8 ms
   preemption, so the optimized engine's p99 stays ~6 µs under same-core load while p99.9 is ~830 µs.

### Corrections / open items

- **Entry 1's basic C GET (40,306/s) is not reproduced by benchmark v2: 76,995/s on both cores.** Same engine
  code; v1 ran inside the main task, v2 in a pinned task. Cause not yet identified. Entry 2 numbers supersede
  Entry 1 for basic C GET. All other Entry 1 C figures reproduce within 2 %.
- Max keys for the optimized engine is 3,869 here vs 3,942 in Entry 1 (−1.9 %): the v2 firmware is larger
  (simulator, timer driver) and the heap is carved after the sample buffers are freed. Basic: 3,659 vs 3,664.
- The WiFi simulator must be re-calibrated against real WiFi once the board's supply is fixed.

Raw data: `results/entry2_<variant>.jsonl` (one RESULT line per scenario), tables via `python tools/summarize.py entry2`.

---

## Entry 3 — 2026-11-25 — Phase 1: multicore designs (on-device, WiFi off)

**Method.** One firmware (`variants/opt.cfg + nowifi.cfg + mc.cfg`, harness `kvcache/main/mc_bench.c`)
runs every design at boot with the lock discipline and shard count switched at runtime, so all designs
share the same binary, memory layout and keys. 1,024 random 16 B keys (preloaded), 32 B values,
workloads 90/10 and 50/50 GET/SET, uniform or Zipf(θ = 0.99). Each run lasts 0.4 s; every 16th
operation is timed. Baseline `1c_nolock` = one task on core 1, no lock. In the *direct* designs each
core gets its own request stream (as with two event loops each owning its own connections); the *queue*
designs have one receiver task on core 0 (standing in for the network stack) feeding workers.
Lock modes: `mux` = FreeRTOS `portMUX` (disables interrupts), `cas` = plain compare-and-swap spinlock
(S32C1I), `rw` = readers-writer spinlock, `seqlock` = lock-free optimistic GETs + CAS-locked writers,
`owned` = each core only receives keys of its own shard, no lock (shared-nothing upper bound).

Note: the baseline here (289k ops/s) is lower than Entry 2's 357k GET/s because this harness mixes
GET/SET over 1,024 random keys and adds its own loop overhead; compare designs within this entry.

### Direct designs: throughput (ops/s) and speed-up vs 1 core without lock

| design | g90_uniform | g50_uniform | g90_zipf | g50_zipf |
|---|---:|---:|---:|---:|
| 1c_nolock | 289,071 (1.00×) | 290,891 (1.00×) | 289,766 (1.00×) | 291,633 (1.00×) |
| 1c_mux | 227,906 (0.79×) | 229,033 (0.79×) | 228,273 (0.79×) | 229,506 (0.79×) |
| 1c_cas | 277,178 (0.96×) | 278,916 (0.96×) | 277,938 (0.96×) | 279,586 (0.96×) |
| 2c_shared_mux | 277,433 (0.96×) | 291,229 (1.00×) | 278,455 (0.96×) | 292,683 (1.00×) |
| 2c_shared_cas | 325,594 (1.13×) | 346,084 (1.19×) | 326,998 (1.13×) | 347,423 (1.19×) |
| 2c_sharded_cas | 418,119 (1.45×) | 427,871 (1.47×) | 420,347 (1.45×) | 430,085 (1.47×) |
| 2c_shared_rw | 471,825 (1.63×) | 394,080 (1.35×) | 473,697 (1.63×) | 395,328 (1.36×) |
| 2c_sharded_rw | 484,340 (1.68×) | 450,480 (1.55×) | 487,805 (1.68×) | 452,817 (1.55×) |
| 2c_shared_seqlock | 476,375 (1.65×) | 361,095 (1.24×) | 477,392 (1.65×) | 365,385 (1.25×) |
| 2c_sharded_seqlock | 489,167 (1.69×) | 426,505 (1.47×) | 491,395 (1.70×) | 427,742 (1.47×) |
| **2c_owned_nolock** | **515,587 (1.78×)** | **511,989 (1.76×)** | **517,074 (1.78×)** | **513,469 (1.76×)** |

### Direct designs: latency and lock contention

| design | workload | p50 / p99 / p99.9 µs | contended acquisitions | spin cycles per contended | time spinning | seqlock retries |
|---|---|---:|---:|---:|---:|---:|
| 1c_nolock | g90_uniform | 3.29 / 3.83 / 7.27 | – | – | – | – |
| 1c_mux | g90_uniform | 4.21 / 5.02 / 8.43 | 0.0 % | – | 0.0 % | – |
| 1c_cas | g90_uniform | 3.43 / 3.99 / 7.50 | 0.0 % | – | 0.0 % | – |
| 2c_shared_mux | g90_uniform | 7.06 / 8.47 / 17.81 | 98.5 % | 593 | 33.8 % | – |
| 2c_shared_cas | g90_uniform | 5.96 / 6.65 / 17.76 | 99.3 % | 451 | 30.4 % | – |
| 2c_shared_cas | g50_uniform | 5.51 / 7.39 / 17.40 | 99.3 % | 382 | 27.4 % | – |
| 2c_sharded_cas | g90_uniform | 4.08 / 6.38 / 15.99 | 34.7 % | 443 | 13.4 % | – |
| 2c_sharded_cas | g50_uniform | 4.04 / 6.20 / 16.16 | 36.8 % | 358 | 11.7 % | – |
| 2c_shared_rw | g90_uniform | 3.77 / 5.92 / 14.65 | 15.5 % | 383 | 5.8 % | – |
| 2c_shared_rw | g50_uniform | 5.12 / 6.12 / 17.07 | 66.6 % | 364 | 19.9 % | – |
| 2c_sharded_rw | g90_uniform | 3.81 / 5.90 / 14.13 | 8.2 % | 333 | 2.8 % | – |
| 2c_sharded_rw | g50_uniform | 3.90 / 6.07 / 15.93 | 26.9 % | 341 | 8.6 % | – |
| 2c_shared_seqlock | g90_uniform | 3.66 / 10.10 / 15.87 | 9.6 % | 383 | 3.6 % | 9,705 |
| 2c_shared_seqlock | g50_uniform | 3.98 / 21.75 / 34.85 | 50.2 % | 355 | 13.4 % | 35,998 |
| 2c_sharded_seqlock | g90_uniform | 3.70 / 7.87 / 16.03 | 5.0 % | 353 | 1.8 % | 5,110 |
| 2c_sharded_seqlock | g50_uniform | 4.00 / 12.49 / 16.65 | 23.8 % | 341 | 7.2 % | 14,860 |
| 2c_owned_nolock | g90_uniform | 3.66 / 4.07 / 12.87 | – | – | – | – |
| 2c_owned_nolock | g50_uniform | 3.69 / 4.12 / 13.90 | – | – | – | – |

(Zipf rows are within 2 % of the uniform rows and are in the raw data.)

### Queue designs: receiver task on core 0 hands requests to workers

| design | workload | handoff | ops/s | vs 1c_nolock | worker0 / worker1 ops/s | CPU core0 / core1 |
|---|---|---|---:|---:|---:|---:|
| q_1w | g90_uniform | FreeRTOS queue, 1/item | 102,164 | 0.35× | 102,164 / – | 100 % / 100 % |
| q_2w_shared | g90_uniform | FreeRTOS queue, 1/item | 206,898 | 0.72× | 68,639 / 138,259 | 100 % / 97 % |
| q_2w_routed | g90_uniform | FreeRTOS queue, 1/item | 190,601 | 0.66× | 91,821 / 98,779 | 100 % / 64 % |
| q_1w_batch16 | g90_uniform | FreeRTOS queue, 16/item | 250,718 | 0.87× | 250,718 / – | 31 % / 100 % |
| q_2w_routed_batch16 | g90_uniform | FreeRTOS queue, 16/item | 408,109 | 1.41× | 203,234 / 204,874 | 100 % / 83 % |
| ring_1w | g90_uniform | SPSC ring, 1/item | 250,039 | 0.86× | 250,039 / – | 100 % / 100 % |
| **ring_2w_routed** | g90_uniform | SPSC ring, 1/item | **442,547** | **1.53×** | 215,298 / 227,248 | 100 % / 100 % |
| ring_2w_routed_batch16 | g90_uniform | SPSC ring, 16/publish | 441,237 | 1.53× | 215,318 / 225,918 | 100 % / 100 % |
| q_2w_routed_batch16 | g90_zipf | FreeRTOS queue, 16/item | 411,357 | 1.42× | 203,318 / 208,038 | 100 % / 84 % |
| ring_2w_routed | g90_zipf | SPSC ring, 1/item | 429,085 | 1.48× | 216,308 / 212,776 | 100 % / 100 % |

Queue latencies (p50 184–745 µs, in the raw data) are measured at saturation: the receiver keeps the
queue full, so they equal queue depth × service time (Little's law), not the cost of one handoff.
They are not comparable to the direct designs' per-operation latencies.

### Statistics counters (2c_owned_nolock, g90_uniform)

| counter design | ops/s | lost updates in 0.4 s |
|---|---:|---:|
| per-core counters, combined at the end | 515,549 | 0 |
| one shared counter, atomic CAS increment | 505,769 (−1.9 %) | 0 |
| one shared counter, plain `++` | 513,662 | **316** |

### Findings

1. **Two cores top out at 1.78×, even with no lock at all** (`2c_owned_nolock`). Per-operation p50 rises
   3.29 → 3.66 µs (+11 %) when both cores work at once: the cores share the internal SRAM bus. This is
   the real ceiling for any dual-core design on this chip (parallel efficiency 89 %).
2. **A shared table behind FreeRTOS's `portMUX` gives no speed-up (0.96×).** 98.5 % of acquisitions are
   contended; the cores take turns. `portMUX` alone costs 21 % on one core (it disables interrupts and
   tracks the owner); a plain CAS spinlock costs 4 %. Lock choice matters more than core count.
3. **Sharding (teammate's idea) works:** 2 shards with a CAS lock each cut contention from 99 % to 35 %
   and reach 1.45×. Combined with a readers-writer lock or a seqlock it reaches 1.68–1.70×.
4. **Read-mostly locks win only when reads dominate.** At 90 % GET, RW lock (1.63×) and seqlock (1.65×)
   on a single shared table nearly match sharding; at 50 % GET they fall to 1.35× and 1.24×, and the
   seqlock's p99 doubles (21.8 µs) because readers retry (36k retries in 0.4 s). Sharded RW is the most
   robust lock-based design across mixes (1.55–1.68×).
5. **Receiver → queue → workers (teammate's idea): the handoff mechanism decides everything.**
   One FreeRTOS queue item per request adds ~6.5 µs to a 3.3 µs operation (9.8 µs per request in
   total) and makes it **3× slower than one core** (0.35×).
   Batching 16 requests per item: 0.87× (1 worker), 1.41× (2 routed workers). A lock-free SPSC ring:
   **1.53× without batching**, and batching adds nothing on top of a ring. With per-request FreeRTOS
   queues the receiver itself becomes the bottleneck (worker 1 idle 36 % in `q_2w_routed`).
6. **Per-core counters are only 1.9 % faster than one shared atomic counter.** ESP32's internal SRAM has
   no data cache, so there is no cache-line bouncing; the gain is avoiding the CAS, and it is small. The
   plain shared counter lost 316 increments in 0.4 s: per-core or atomic is required for correctness.
7. **Zipf(0.99) skew changed nothing here** (all within 2 %): locks are per shard, not per key, and with
   2 shards and randomly placed hot keys the two workers stayed balanced (93.8k vs 93.6k ops/s).
   Load imbalance needs a stronger skew or more shards: Phase 2.

### Limitations

- WiFi off; the direct designs assume requests already arrive per core. The network stack's serial cost
  (Amdahl) will reduce all speed-ups; measured in Phase 4.
- The CAS, RW and seqlock locks do not disable interrupts; under WiFi load a preempted lock holder makes
  the other core spin. To test with the WiFi-load simulator.
- Seqlock GETs do not set the CLOCK reference bit, so eviction is less LRU-like under that mode.
- **IRAM is nearly full.** The first full network build (optimized engine + lwIP/WiFi IRAM options)
  overflowed IRAM by 2,940 B; with `CONFIG_ESP_WIFI_EXTRA_IRAM_OPT` removed it links at 128,343 of
  131,072 B (97.9 %). Every IRAM byte now competes between our hot path and the network stack's:
  a Phase 4 measurement.

Raw data: `results/entry3_mc.jsonl`; tables: `python tools/summarize_mc.py entry3_mc.jsonl`.

---

## Entry 4 — 2026-11-25 — Phase 2: load imbalance and adaptive balancing; Phase 1 designs under WiFi-like load

**Method.** Harness Phase 2 (`variants/opt + nowifi + mc + mc2`). A receiver task on core 0 (where the
network stack lives) routes **one** request stream into two lock-free SPSC rings; worker 0 runs on core 0
next to the receiver, worker 1 on core 1. The table is split into **8 shards**; `owner[shard]` says which
worker executes it (initially even shards → worker 0, odd → worker 1). 1,024 random keys, 32 B values,
90 % GET unless noted, 0.4 s per run. The receiver is saturating, so throughput is limited by whichever
worker is busiest.

Workloads: `uniform`; `zipf099` / `zipf12` (Zipf θ = 0.99 / 1.2, hot keys placed randomly over shards,
**3 repetitions with a fresh placement each**; within a repetition every design runs on the same placement
and the same request stream); `hotshard_zipf12` (θ = 1.2 with all the hottest keys in shard 0, owned by the
receiver's core: 91 % of the load lands on worker 0); `hotshard_zipf12_g50` (same, 50 % GET).

Designs:

| design | idea | how it works |
|---|---|---|
| static_hash | baseline (Phase 1 best) | owner never changes, no lock |
| roundrobin_rw | work sharing | requests alternate between workers; RW lock per shard |
| spill_rw | **teammate: adaptive load balancing** | send to the owner unless its ring holds > 32 more requests than the other's; RW lock per shard |
| migrate | **teammate: adaptive load balancing** (ownership migration) | every 2,048 requests, if the busier ring's average backlog exceeds the other's by > 8, move the busy worker's least-used shard to the other worker. **Lock-free hand-over:** the shard's new requests wait in a side buffer until the old owner has executed everything routed to it before the decision, so requests for one key are never reordered |
| migrate_hyst | same, with hysteresis | imbalance > 32 for 2 epochs in a row, then 4 epochs of cool-down |
| hotrep_rw | **mine: hot-key replication** | top-8 keys per 2,048 requests are hot; their GETs go to the less loaded worker, which serves them from a replica (seqlock, written only by the key's owner on SET / first GET); RW lock per shard for the rare fallback |

### Throughput (mean ops/s) and speed-up vs static_hash on the same placement [min–max over repetitions]

| design | uniform | zipf099 (n=3) | zipf12 (n=3) | hotshard_zipf12 | hotshard_zipf12_g50 |
|---|---:|---:|---:|---:|---:|
| static_hash | 370,743 (1.00×) | 364,514 (1.00×) | 350,060 (1.00×) | 228,121 (1.00×) | 226,312 (1.00×) |
| roundrobin_rw | 362,692 (0.98×) | 363,222 (1.00× [0.93–1.06]) | 363,025 (1.05× [0.89–1.15]) | 358,544 (1.57×) | **338,192 (1.49×)** |
| spill_rw | 376,207 (1.01×) | 376,390 (1.04× [0.98–1.09]) | 382,411 (1.10× [1.00–1.17]) | **361,909 (1.59×)** | 335,064 (1.48×) |
| migrate | **390,513 (1.05×)** | **403,982 (1.11× [1.03–1.18])** | 408,042 (1.18× [1.00–1.28]) | 241,779 (1.06×) | 238,945 (1.06×) |
| migrate_hyst | 371,168 (1.00×) | 396,567 (1.09× [1.00–1.17]) | **408,104 (1.18× [1.05–1.24])** | 240,195 (1.05×) | 237,415 (1.05×) |
| hotrep_rw | 336,624 (0.91×) | 381,670 (1.05× [0.98–1.11]) | 406,468 (1.17× [1.08–1.24]) | 348,385 (1.53×) | 257,771 (1.14×) |

### Balancing activity (mean over repetitions)

| design | workload | w0 / w1 ops/s | spills | migrations | replica hits / fallbacks | lock contended |
|---|---|---:|---:|---:|---:|---:|
| static_hash | hotshard_zipf12 | 207,684 / 20,436 | – | – | – | – |
| spill_rw | hotshard_zipf12 | 170,678 / 191,231 | 64,482 | – | – | 7.5 % |
| migrate | hotshard_zipf12 | 205,902 / 35,876 | – | 3 (all other shards moved off w0) | – | – |
| hotrep_rw | hotshard_zipf12 | 154,193 / 194,192 | – | – | 65,188 / 2 | 0.0 % |
| roundrobin_rw | hotshard_zipf12_g50 | 169,094 / 169,097 | – | – | – | 30.1 % |
| migrate | uniform | 181,156 / 209,356 | – | **61** | – | – |
| migrate_hyst | uniform | 188,334 / 182,834 | – | **0** | – | – |
| migrate | zipf12 | 182,294 / 225,747 | – | 28 | – | – |
| migrate_hyst | zipf12 | 183,024 / 225,079 | – | **2** | – | – |
| hotrep_rw | zipf12 | 164,143 / 242,325 | – | – | 36,543 / 149 | 0.0 % |

(full table: `results/entry4_tables.md`)

### Phase 1 designs under WiFi-like load on core 0 (simulator at 40 % duty, Entry 2)

| design | workload | quiet ops/s | under load | change | vs 1 core under load | p99.9 core 0 µs quiet → load | p99.9 core 1 µs quiet → load | lock contended under load |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 1c_nolock (core 1) | g90 | 290,353 | 283,316 | −2 % | 1.00× | – | 7.5 → 9.0 | – |
| 2c_shared_cas | g90 | 325,820 | 250,290 | −23 % | **0.88×** | 17.9 → 836.1 | 17.7 → 17.8 | 74.7 % |
| 2c_sharded_cas | g90 | 420,296 | 286,442 | −32 % | 1.01× | 16.0 → 848.1 | 17.3 → **833.7** | 30.1 % |
| 2c_sharded_rw | g90 | 487,641 | 324,670 | −33 % | 1.15× | 14.4 → 849.9 | 13.2 → 17.3 | 6.5 % |
| 2c_sharded_seqlock | g90 | 497,852 | 395,450 | −21 % | 1.40× | 13.8 → 836.9 | 12.5 → 16.1 | 3.6 % |
| **2c_owned_nolock** | g90 | 517,301 | **414,010** | −20 % | **1.46×** | 14.5 → 830.5 | 12.7 → 16.0 | – |
| 2c_shared_cas | g50 | 350,148 | 260,386 | −26 % | 0.92× | 17.1 → 839.9 | 17.2 → 17.2 | 77.4 % |
| 2c_sharded_rw | g50 | 455,595 | 306,662 | −33 % | 1.08× | 15.3 → 840.7 | 14.2 → 16.8 | 23.3 % |
| 2c_sharded_seqlock | g50 | 432,715 | 333,829 | −23 % | 1.18× | 20.1 → 831.8 | 20.0 → 22.0 | 18.1 % |
| 2c_owned_nolock | g50 | 516,854 | 413,870 | −20 % | 1.46× | 14.1 → 829.5 | 12.7 → 12.7 | – |
| ring_2w_routed | g90 | 429,215 | 240,041 | −44 % | 0.85× | queue-bound | queue-bound | – |

### Findings

1. **Imbalance is real only when hot keys concentrate.** With Zipf skew and random placement, the 8-shard
   static map stays near 52/48 on average, but individual placements vary (ratios range 0.89–1.28×), so a
   single run is not enough; this entry uses 3 placements.
2. **For a hot shard, sharing work beats moving ownership.** When 91 % of the load sits in one shard,
   spill-over (teammate) gives **1.59×** and round-robin 1.57×, hot-key replication 1.53×. Migration gives
   only 1.06×: it correctly moves every other shard away (final owners `01111111`), but cannot split the hot
   shard itself. **Ownership granularity limits migration.**
3. **For spread-out skew, migration wins and costs nothing when idle.** On Zipf 1.2 with random placement,
   migration gives 1.18× (best repetition 1.28×), versus 1.10× for spill-over. Migration takes no locks,
   whereas spill-over and round-robin pay RW-lock cost on every request (round-robin is 0.98× on uniform).
4. **Plain migration thrashes; hysteresis fixes it.** v1 made 28–61 migrations in 0.4 s (each one stalls a
   shard while its old owner drains); with hysteresis: 0–2, the same 1.18× on Zipf 1.2, and no drift on
   uniform traffic.
5. **Hot-key replication (mine) helps reads, not writes.** 1.53× on the hot shard at 90 % GET, but 1.14×
   at 50 % GET, because SETs must go to the owner and refresh the replica. It costs 9 % on uniform traffic
   (RW locks + routing). A first version was receiver-bound (0.66× on uniform) because hot-key detection
   rescanned all counters 8× per epoch; v2 uses a one-pass top-8.
6. **No single policy wins everywhere.** Spill-over is the safest (mean never below 1.01× here, worst
   single placement 0.98×, best on hot shards); migration with hysteresis is best for spread-out skew and free otherwise. The two are
   complementary: migrate coarse imbalance, spill or replicate what one shard cannot hold.
7. **Under WiFi-like load on core 0, the lock-free shared-nothing design is the only one that keeps a
   solid dual-core gain: 1.46× over one core** (owned, 414k vs 283k ops/s). It loses exactly core 0's lost
   capacity (−20 %). Lock-based designs lose more (−23 to −33 %) because core 1 spins while a preempted
   core-0 lock holder waits for the simulator's burst; a shared table with a CAS lock becomes **slower than
   one core (0.88×)**. Core 1's p99.9 mostly stays at 16–22 µs; one run (sharded CAS) shows the
   lock-holder stall reaching core 1 at p99.9 (834 µs). Core 0's own p99.9 is ~830 µs in every design:
   anything sharing the WiFi core inherits its bursts.
8. **The receiver → ring design loses 44 % under load** because its receiver lives on the loaded core:
   the serial stage (Amdahl) is exactly where the network stack runs.

### Corrections

- In the chat summary of the first Phase 2 run I stated that the lock-free design kept p99.9 at 16.6 µs under
  WiFi-like load while lock-based designs went to ~835 µs. With per-core latency (added in this entry) that
  is wrong: the ~835 µs tails are core 0's own preemption and appear in all designs, including the
  lock-free one. The lock effect shows mainly as lost throughput.
- First Phase 2 runs (single placement, v1 hot-key detector, no hysteresis variant) are kept as
  `results/entry4_p2_v1.jsonl` and `entry4_p2_v2.jsonl`; this entry uses `results/entry4_p2.jsonl`.

### Limitations

- WiFi is simulated (Entry 2); the receiver produces requests from memory instead of parsing packets.
- Replicas trade consistency: a GET served from a replica may miss a SET still queued at the owner.
- 2 workers only; with more shards per worker the migration granularity problem shrinks.

Raw data: `results/entry4_p2.jsonl`; tables: `python tools/summarize_p2.py` (saved as `results/entry4_tables.md`).

---

## Entry 5 — 2026-11-25 — Phase 1–2 results integrated into the real server; end-to-end test over loopback

**Why this entry.** Phases 1–2 were measured in a separate harness; the real server still used designs the
harness had ruled out (portMUX in its two-core modes, shared non-atomic statistics counters). This entry
moves the decisions into the server and tests the server itself, end to end.

**What was integrated**

| change | evidence |
|---|---|
| One lock implementation for server and harness: shard count and lock discipline are runtime settings (server takes them from Kconfig) | prevents the two drifting apart again |
| Two-core modes: `shared` = 1 table + seqlock, `sharded` = 8 shards + seqlock (CAS-locked writers); portMUX removed | Entry 3: portMUX 0.96×, sharded seqlock 1.69× (engine-level) |
| Per-loop statistics blocks summed in `INFO`; basic server updates under a critical section | Entry 3: shared plain counter lost 316 updates in 0.4 s |
| lwIP core locking in `netopt.cfg` | this entry: +5 % quiet, +33 % under WiFi-like load |

**Method.** Without WiFi, the board tests itself: lwIP's loopback interface carries real TCP connections
from client tasks on core 0 (where the network stack runs) to the server on `127.0.0.1:6379`, so every
request goes through sockets, TCP, the RESP parser, the event loop(s) and the engine. Each build runs
(1) a 34-case functional suite with exact expected replies (PING, SET/GET, NX/XX, DEL, EXISTS, TTL/PTTL,
expiry, SETEX, pipelining, inline commands, 3 KB and 10 KB values, FLUSHALL, errors), (2) a raw TCP echo
baseline (no parser, no engine), (3) 1 s runs of GET/SET at 1 or 4 connections × 1 or 16 pipelined
requests, quiet and with the WiFi-load simulator on core 0. Loopback runs TCP twice on one chip and the
clients share core 0 with lwIP, so absolute numbers are a pessimistic stand-in for WiFi; they compare
server designs under identical conditions. Test builds use a 120 KB heap reserve for the on-board clients
and a 32-packet loopback queue (default 8).

### Functional suite

| build | result |
|---|---|
| basic C (basic engine + task per client) | 33/33 (no inline commands, by design) |
| optimized, all five configurations | **34/34** |

### Raw TCP echo over loopback (no parser, no engine): RTT p50 / p99 µs

| echo task on | default lwIP | lwIP core locking |
|---|---:|---:|
| core 1 (socket calls cross to lwIP's core 0) | 682 / 1,061 | 627 / 814 |
| core 0 (same core as lwIP) | **545** / 762 | 551 / 763 |
| (stock lwIP config of the basic build, core 1) | 1,334 / 1,719 | – |

### Real server over loopback: ops/s (batch p50 / p99 µs)

Quiet:

| design | GET 1×1 | GET 4×1 | GET 1×16 | GET 4×16 | SET 1×1 | SET 4×16 |
|---|---:|---:|---:|---:|---:|---:|
| basic C | 509 (1,959 / 2,308) | 935 | 16 (1.38 s) | 48 ⚠ | 564 | 64 |
| opt, 1 loop core 1 | 1,348 (735 / 859) | 1,135 | 15,999 (897 / 1,303) | 18,528 | 1,318 | 17,976 |
| opt, 1 loop core 0 | 912 (1,095 / 1,103) | 758 | 13,294 | 11,152 | 911 | 11,021 |
| opt, 2 loops, shared seqlock | 925 | 871 | 16,272 | 13,072 | 924 | 12,912 |
| opt, 2 loops, 8 shards seqlock | 923 | 874 | 17,584 | 12,944 | 923 | 12,944 |
| **opt, 1 loop core 1 + core locking** | 1,279 | **1,206** | 16,272 (1,006 / 1,013) | **19,392** | 1,324 | **19,356** |

With the WiFi-load simulator on core 0 (40 % duty):

| design | GET 1×1 | GET 4×1 | GET 1×16 | GET 4×16 | SET 1×1 | SET 4×16 |
|---|---:|---:|---:|---:|---:|---:|
| basic C | 278 | 420 | 208 | 64 | 314 | 1,087 |
| opt, 1 loop core 1 | 883 | 726 | 11,389 | 10,715 | 883 | 11,087 |
| opt, 1 loop core 0 | 494 | 407 | 7,184 | 6,032 | 493 | 5,952 |
| opt, 2 loops, shared seqlock | 499 | 570 | 11,563 | 8,496 | 499 | 8,559 |
| opt, 2 loops, 8 shards seqlock | 498 | 553 | 11,676 | 8,560 | 497 | 8,400 |
| **opt, 1 loop core 1 + core locking** | **990** | **821** | **12,640** | **14,222** | **985** | **13,984** |

Server-side time per command (from the server's own counters) is 3.7–4.4 µs for every optimized design
when quiet. ⚠ = a client connection failed in that run (basic C: one 3 s receive timeout, see findings).
Full tables: `results/entry5_tables.md`.

### Findings

1. **The network stack, not the cache, dominates end to end.** A bare TCP echo over loopback costs
   545–682 µs per round trip; the full server costs 735 µs, i.e. **the server adds ~50 µs (≈ 7 %)**:
   ~93 % of a request is lwIP's socket/TCP path (~150 µs per `recv()`, ~107 µs per `send()` measured
   inside the server). This is the measurement that justifies attacking the socket layer next.
2. **Engine-level dual-core gains do not carry over end to end.** The two-loop designs (1.69× in the
   Entry 3 harness) are **30 % slower** than one loop on core 1 here (12.9–13.1k vs 18.5k ops/s at 4×16),
   and 20 % slower under WiFi-like load: their second loop runs on core 0, where lwIP (and here the test
   clients) are already the bottleneck. Amdahl's law with the serial stage on core 0. **Decision: the
   server's default stays one loop on core 1**; the seqlock-sharded two-loop mode is kept for when core 0
   has spare capacity (an adaptive switch is a follow-up).
3. **Same-core socket calls are 20 % cheaper** (echo RTT 545 vs 682 µs), and **lwIP core locking**
   recovers part of the cross-core cost: +5 % at 4×16 quiet and **+33 % under WiFi-like load**
   (14.2k vs 10.7k ops/s), +12 % at 1×1 under load. Adopted.
4. **Optimized vs basic C, end to end:** 2.6× at one request in flight (1,348 vs 509 ops/s) and
   280–1,000× with 16 pipelined requests. The basic server sends one TCP segment per reply; 16 pipelined
   replies per client overflow the loopback queue and trigger ~1 s TCP retransmissions (and one 3 s client
   timeout). The size of that gap is loopback-specific; the per-reply segment cost is not.
5. **A one-loop server placed on core 0 is the worst optimized choice** (912 vs 1,348 ops/s at 1×1; 494 vs
   883 under load), confirming from the real server path what Entry 2 showed for the engine.
6. **IRAM is now 99.4 % full** in the full WiFi build (793 B free) after adding core locking: every further
   IRAM placement now has to be traded against the network stack's.

### Bugs found by the end-to-end test (fixed)

- **Replies larger than the 4 KB transmit buffer dropped the connection** (optimized server): it grew the
  buffer straight to 71 KB, more than the heap reserve. Now grows by doubling.
- **Same bug in the basic server** (single 70 KB `malloc`, client left without a reply). Now doubles.
- Test harness: an unchecked `malloc` in the client (crash), the next run starting before the server had
  closed the previous run's connections, client buffers exhausting the reserve, lwIP's 8-packet loopback
  queue; basic server timing used per-core cycle counters from unpinned tasks (now `esp_timer`).

### Corrections

- **Build-system issue:** ESP-IDF applies `SDKCONFIG_DEFAULTS` only when a build directory's `sdkconfig` is
  first created; later edits to a fragment are silently ignored. The first rerun of this entry therefore ran
  with a stale 80 KB reserve and an 8-packet loopback queue (connection resets in every 4-connection run).
  `tools/build_variant.ps1` now always regenerates `sdkconfig`. Checked every build directory: Entries 1–4
  were not affected (their fragments existed before the first build; the one later change, removing
  `ESP_WIFI_EXTRA_IRAM_OPT`, was applied by deleting that `sdkconfig` by hand).
- In chat I recommended "two-core mode = 8 shards + seqlock" as the server's way to use both cores. That
  holds for the engine, but end to end the second loop costs more than it gains (finding 2); the default
  is one loop on core 1.

### Open

- **Values above ~4 KB are stored outside the arena in the heap reserve** and get `-OOM` when the reserve
  is fragmented (the 10 KB test fails with the deployment-size reserve). To fix in Phase 3 (chained arena
  chunks).
- Everything here is loopback; real-WiFi numbers still need the board's power fixed.

Raw data: `results/entry5_lb_*.jsonl`; tables: `python tools/summarize_lb.py entry5` (saved as
`results/entry5_tables.md`).

---

## Entry 6 — 2026-11-25 — Phase 3: memory (engine v2), integrated into the server

**What changed (engine v2, `kvcache/main/kv_opt.c`).** The Entry 5 engine is frozen as `kv_opt1.c` and
the exact Entry 2 engine (git `e82bda2`) as `kv_opt0.c`, so all three run through the same benchmarks.

| change | why |
|---|---|
| arena pages allocated one by one (4 KiB each) | lets the store shrink/grow at runtime (adaptive reserve) |
| **chained values inside the arena** (4 KiB chunks) | values > ~4 KB used the heap reserve and failed with `-OOM` (Entry 5) |
| **4-byte entry header**; the 4-byte expiry is stored only for keys with a TTL | v1 spent 8 B of header per entry |
| **"page-fit" size classes** above 256 B (4088/n: 272, 292 … 584, 680, 816, 1020, 1360, 2044) | v1's 640/768 classes fit 6 × 532 B entries per page instead of 7 |
| 4-byte-granular classes up to 64 B; O(1) class lookup table | less rounding waste; no per-SET scan |
| **prefix compression**: `a:b:` prefixes (≤ 24 B, up to 63) stored once, 1-byte id per entry | IoT key names repeat long prefixes |
| reclaim fully empty pages of other classes before evicting | empty pages stayed stuck in their old class (slab calcification) |
| adaptive reserve controller `kv_mem_adjust(low, high)` (option, off by default) | research question: cache vs network memory |
| **streamed SET and GET** for values larger than the 4 KB connection buffers | no buffer growth from the heap reserve |
| clock read only for entries with a TTL; GET/SET bodies and fast paths inlined; compile-time lock mode in single-loop server builds | speed regression found in this entry (below) |

**Correctness.** A new on-board engine test suite (`engine_test.c`, 72 checks: small/large/empty values,
in-place and class-changing overwrites, TTL/EXPIRE/KEEPTTL/NX/XX, 4,089 B – 64 KB chained values byte-exact,
prefix learning, and two 6,000-operation stress runs with eviction, page stealing and chains mixed, verifying
every surviving value against its expected content) passes **72/72** on the final engine. It found two engine
bugs, both fixed: TTL deadlines up to 1 ms long (`| 1` to avoid a zero deadline; also present in v1) and
empty pages stuck in their old slab class (64 KB values failed with free memory available).

### Capacity: max keys before the store refuses (noeviction), store bytes per key in brackets

16 B keys `key:%012d`:

| engine | 8 B | 16 B | 32 B | 64 B | 128 B | 256 B | 512 B | 1024 B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| basic C | 5,145 (54) | 4,406 (56) | 3,416 (72) | 2,367 (104) | 1,431 (173) | 798 (309) | 425 (581) | 225 (1100) |
| opt0 (Entry 2) | 5,131 (48) | 5,131 (48) | 3,942 (62) | 2,268 (108) | 1,350 (181) | 648 (376) | 324 (753) | 162 (1506) |
| opt1 (Entry 5) | 5,075 (47) | 5,075 (47) | 3,869 (62) | 2,226 (108) | 1,325 (181) | 636 (377) | 318 (754) | 159 (1507) |
| **opt2 (v2)** | **5,447** (44) | **5,447** (44) | **4,056** (58) | **2,392** (99) | 1,300 (182) | 728 (326) | 364 (652) | 156 (1520) |

24 B IoT keys `sensor:temp:roomNN:NNNNN` (20 distinct prefixes):

| engine | 8 B | 16 B | 32 B | 64 B | 128 B | 256 B | 512 B | 1024 B |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| basic C | 4,406 (56) | 3,856 (64) | 3,076 (80) | 2,199 (112) | 1,371 (180) | 798 (309) | 425 (581) | 225 (1100) |
| opt1 (Entry 5) | 5,075 (47) | 4,505 (53) | 3,339 (72) | 2,226 (108) | 1,325 (181) | 636 (377) | 318 (754) | 159 (1507) |
| opt2 | 5,447 (44) | 4,784 (50) | 3,536 (67) | 2,184 (109) | 1,300 (182) | 728 (326) | 364 (652) | 156 (1520) |
| **opt2 + prefix** | **5,447** (44) | **5,447** (44) | **4,784** (50) | **2,652** (89) | **1,456** (163) | **780** (304) | 364 (652) | 156 (1520) |

Mixed value sizes (16–512 B, IoT keys; insert until 200 rejections in a row):

| engine | keys stored | payload / store bytes |
|---|---:|---:|
| basic C | 766 | 0.90 |
| opt1 (Entry 5) | 844 | 0.75 |
| opt2 | 1,189 (+41 %) | 0.82 |
| **opt2 + prefix** | **1,645 (+95 %)** | **0.94** |

### Speed (Entry 2 engine benchmark, same firmware, sequential 16 B keys, 32 B values)

| engine | SET/s | GET/s | DEL/s | GET p50 / p99 µs |
|---|---:|---:|---:|---:|
| opt0 (Entry 2 code) | 434,272 | 357,283 | 389,468 | 2.73 / 3.11 |
| opt1 (Entry 5 code) | 380,474 | 324,991 | 366,743 | 3.01 / 3.33 |
| opt2, first version | 304,460 | 297,380 | 331,081 | 3.29 / 3.88 |
| **opt2, final** (prefix off) | **404,350** | **338,329** | 359,544 | 2.89 / 3.33 |
| opt2, final, prefix on | 245,585 | 260,321 | 266,261 | 3.77 / – |

### End to end (real server over loopback; v2 engine + prefix + streaming + lwIP core locking)

| | GET 1×1 | SET 1×1 | GET 1×16 | GET 4×16 | SET 4×16 | GET 4×16 under WiFi-like load |
|---|---:|---:|---:|---:|---:|---:|
| Entry 5 server (opt1 + core locking) | 1,279 | 1,324 | 16,272 | 19,392 | 19,356 | 14,222 |
| **Entry 6 server** | **1,365** | **1,374** | **18,303** | **19,837** | **19,644** | **14,560** |
| v2 without prefix (same run day) | 1,247 | 1,158 | 16,016 | 18,733 | 19,456 | 14,096 |
| v2 with prefix (same run day) | 1,186 | 1,208 | 16,077 | 19,134 | 19,072 | 14,463 |

Functional suite at the **real 40 KB deployment reserve**: Entry 5 engine 30/34 (10 KB SET `-OOM`),
**Entry 6 server 34/34** (10 KB SET and GET streamed through the 4 KB buffers).

### Adaptive reserve (v2) under a simulated packet-buffer load (baseline ~10 KB, bursts to ~64 KB in ~5 ms every 1.5 s)

| reserve | buffer allocations failed | cache hit rate | avg arena pages |
|---|---:|---:|---:|
| fixed 40 KB (current default) | 1,215 of 1,296 | 0.671 | 52.0 |
| **fixed 80 KB** | **0 of 176** | 0.555 | 43.0 |
| fixed 100 KB | 0 of 176 | 0.492 | 38.0 |
| adaptive, low 20 / high 48 KB | 26 of 202 | 0.590 | 45.6 |
| adaptive, low 40 / high 64 KB | 21 of 197 | 0.535 | 41.4 |
| adaptive, low 56 / high 80 KB | 19 of 195 | 0.485 | 37.4 |

### Findings

1. **Engine v2 stores more in the same memory** (vs the Entry 5 engine, 16 B keys): +7.3 % (8 B values),
   +4.8 % (32 B), +7.5 % (64 B), +14.5 % (256 B and 512 B, page-fit classes), but −1.9 % at 128 B and 1 KB;
   **+41 % for mixed sizes, +95 % with prefix compression**.
2. **Prefix compression** adds +14 % / +35 % / +21 % / +12 % / +7 % keys for IoT-style names with 16 / 32 /
   64 / 128 / 256 B values, and nothing for short prefixes (`key:`) or ≥ 512 B values. It costs 23–39 % in the engine benchmark but **nothing measurable end
   to end** (the engine is ~7 % of a request, Entry 5). Enabled by default.
3. **Where malloc still wins:** basic C stores more 512 B–1 KB values (425 vs 364; 225 vs 156). A 4 KiB page
   holds only three ~1 KB entries; exact-size `malloc` has no page rounding. Fixing this needs larger pages or
   chaining medium values (future work).
4. **Small values hit the index limit**, not the arena: 5,447 keys, because the index is sized for an average
   44 B entry (`AVG_ENTRY_EST`). A bigger index costs memory when values are large; a trade-off to tune.
5. **Streaming makes large values independent of the heap reserve**: values up to 64 KB are written into and read
   out of arena chunks through the normal 4 KB connection buffers (pages pinned while a value streams in).
6. **Adaptive reserve: negative result for sudden bursts.** No adaptive setting reaches zero failed buffer
   allocations: the burst (64 KB in ~5 ms) outruns a reactive controller (2 ms tick, up to 16 pages per tick).
   A fixed reserve sized for the burst (80 KB) has none. Adaptive only wins hit rate by accepting 10–13 % failed
   allocations. **For bursty demand the headroom must be fixed; adaptation can only help with slowly changing
   demand** (not tested). Kept as an option (`CONFIG_KV_ADAPTIVE_RESERVE`, low ≥ largest burst), off by default.
   The synthetic load says the 40 KB default reserve may be too small for real WiFi bursts: to calibrate once
   the radio works.
7. **IRAM** cannot hold the index (Phase 3 plan item dropped): the deployment build has 793 B of IRAM left (Entry 5).

### Corrections

- **The Entry 5 integration cost engine speed and I did not measure it then.** The exact Entry 2 engine, built
  into today's firmware, runs at 434k SET/s; the Entry 5 engine at 380k (−12 %). In this entry I first blamed the
  runtime lock switch; making it compile-time did not recover the speed, which disproved that. The cause was
  code structure (hot functions no longer inlined, an extra function call and a clock read per operation). The
  final v2 engine recovers most of it (404k SET/s, −7 % vs Entry 2, +6 % vs Entry 5) while storing more.
- End to end the loss was never visible (the engine is ~7 % of a request), which is exactly why it went unnoticed.

Raw data: `results/entry6_p3_*.jsonl`, `results/entry6_lb_*.jsonl`; tables: `python tools/summarize_p3.py entry6`
(saved as `results/entry6_tables.md`).

## Entry 7 — 2026-11-25 — Final comparison: shared vs sharded vs owner vs queue dispatch, with and without network, against microredis (all re-measured)

Everything in this entry was measured again today on the same board (ESP32-D0WD-V3, 240 MHz, no PSRAM, radio
off). Nothing is copied from earlier entries. Each design was rebuilt, flashed and run on its own boot.

**What was compared**

| design | what it does |
|---|---|
| single (core) | one task/loop does everything on core 1 (the deployed default) |
| shared (seqlock / rw) | two loops (one per core), one table, readers optimistic (seqlock) or reader-writer lock |
| sharded8 (seqlock / rw) | two loops, table split into 8 shards, each with its own lock |
| owner | 8 shards; each core owns 4 and never locks; a key owned by the other core is forwarded to it (lock-free ring + wake-up) |
| dispatch (queue) | one receiver hands every request to worker(s) through a queue: 1 worker, 2 owner workers, or a shared worker pool |
| basic C | Entry 1's straightforward C (malloc per entry, task per client) |
| microredis | the upstream MicroPython server this project started from |

**Method.** No network: the engine harness from Phases 1–3; 1024 random 16 B keys, 32 B values, 90/10 and
50/50 GET/SET, **5 runs × 3 s** per design, then one extra **checked** run per workload. The checker makes every
value self-describing (key, version, checksum). One writer per key publishes versions before writing. Every GET
is verified: not torn, not a version nobody wrote, not older than one already seen, not missing. At the end
every key's final value is checked. Network: the real server over lwIP TCP loopback (127.0.0.1); client tasks run
on core 0 of the same chip. Functional suite (34 checks), then a 4-client concurrent **consistency test** (the
same checks over TCP, 3 s), then **5 runs × 2 s** of six workloads. microredis ran the same engine workload on
MicroPython v1.29.0 and its real asyncio server over loopback, also 5 × 2 s.

### No network (engine only)

### No network, g90_uniform (5 runs x 3 s each)

| design | ops/s mean | std dev | min - max | vs single core | p50 / p99 / p99.9 µs | CPU core0 / core1 |
|---|---:|---:|---:|---:|---:|---:|
| single_core | 299,774 | 57 (0.0 %) | 299,694 - 299,846 | 1.00× | 3.18 / 3.60 / 7.21 | 0 % / 100 % |
| shared_seqlock | 447,320 | 1,447 (0.3 %) | 445,512 - 449,211 | 1.49× | 3.91 / 11.02 / 15.68 | 100 % / 100 % |
| shared_rw | 440,022 | 1,626 (0.4 %) | 438,854 - 442,876 | 1.47× | 4.05 / 6.85 / 14.74 | 100 % / 100 % |
| sharded8_seqlock | 477,230 | 342 (0.1 %) | 476,881 - 477,658 | 1.59× | 3.94 / 8.19 / 13.79 | 100 % / 100 % |
| sharded8_rw | 471,926 | 192 (0.0 %) | 471,722 - 472,209 | 1.57× | 4.04 / 6.15 / 13.73 | 100 % / 100 % |
| owner | 543,553 | 383 (0.1 %) | 542,987 - 544,012 | 1.81× | 3.50 / 4.01 / 13.29 | 100 % / 100 % |
| dispatch_1worker | 275,761 | 84 (0.0 %) | 275,713 - 275,908 | 0.92× | – | 100 % / 100 % |
| dispatch_2owners | 488,448 | 4,512 (0.9 %) | 481,590 - 493,558 | 1.63× | – | 100 % / 100 % |
| dispatch_pool | 377,183 | 1,240 (0.3 %) | 376,293 - 379,055 | 1.26× | – | 100 % / 97 % |
| microredis | 6,910 | 33 (0.5 %) | 6,894 - 6,969 | 0.02× | – | one core (MicroPython) |

### No network, g50_uniform (5 runs x 3 s each)

| design | ops/s mean | std dev | min - max | vs single core | p50 / p99 / p99.9 µs | CPU core0 / core1 |
|---|---:|---:|---:|---:|---:|---:|
| single_core | 296,909 | 74 (0.0 %) | 296,798 - 296,986 | 1.00× | 3.26 / 3.63 / 7.12 | 0 % / 100 % |
| shared_seqlock | 332,185 | 1,029 (0.3 %) | 330,850 - 333,375 | 1.12× | 4.32 / 25.15 / 42.21 | 100 % / 100 % |
| shared_rw | 345,044 | 1,025 (0.3 %) | 343,930 - 346,325 | 1.16× | 6.28 / 7.17 / 17.14 | 100 % / 100 % |
| sharded8_seqlock | 447,679 | 674 (0.2 %) | 446,993 - 448,609 | 1.51× | 4.20 / 9.10 / 14.57 | 100 % / 100 % |
| sharded8_rw | 462,213 | 94 (0.0 %) | 462,062 - 462,297 | 1.56× | 4.04 / 6.54 / 14.18 | 100 % / 100 % |
| owner | 536,184 | 160 (0.0 %) | 536,014 - 536,416 | 1.81× | 3.57 / 4.01 / 13.36 | 100 % / 100 % |
| dispatch_1worker | 273,083 | 14 (0.0 %) | 273,068 - 273,093 | 0.92× | – | 100 % / 100 % |
| dispatch_2owners | 487,117 | 4,474 (0.9 %) | 481,870 - 491,592 | 1.64× | – | 100 % / 100 % |
| dispatch_pool | 301,763 | 1,238 (0.4 %) | 300,597 - 303,438 | 1.02× | – | 100 % / 96 % |
| microredis | 4,232 | 60 (1.4 %) | 4,151 - 4,276 | 0.01× | – | one core (MicroPython) |

### No network, correctness (one checked 3 s run per design and workload)

| design | workload | GETs checked | SETs | torn | unpublished version | went backwards | misses | final state wrong | verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| single_core | g90_uniform | 421,740 | 49,731 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_seqlock | g90_uniform | 806,912 | 41,756 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_rw | g90_uniform | 791,729 | 41,392 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_seqlock | g90_uniform | 816,362 | 43,790 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_rw | g90_uniform | 808,309 | 38,663 | 0 | 0 | 0 | 0 | 0 | PASS |
| owner | g90_uniform | 822,489 | 88,887 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_1worker | g90_uniform | 409,477 | 44,930 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_2owners | g90_uniform | 766,044 | 88,917 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_pool | g90_uniform | 684,443 | 71,605 | 0 | 0 | 0 | 0 | 0 | PASS |
| single_core | g50_uniform | 236,875 | 237,077 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_seqlock | g50_uniform | 597,233 | 194,715 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared_rw | g50_uniform | 613,067 | 200,274 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_seqlock | g50_uniform | 633,362 | 208,020 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded8_rw | g50_uniform | 619,495 | 216,159 | 0 | 0 | 0 | 0 | 0 | PASS |
| owner | g50_uniform | 452,876 | 454,161 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_1worker | g50_uniform | 222,098 | 228,725 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_2owners | g50_uniform | 435,862 | 424,911 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch_pool | g50_uniform | 353,395 | 344,573 | 0 | 0 | 0 | 0 | 0 | PASS |
| microredis (512 keys) | g90_uniform | 4,522 | 541 | – | – | – | 0 wrong | 0 | PASS |
| microredis (512 keys) | g50_uniform | 2,632 | 2,576 | – | – | – | 0 wrong | 0 | PASS |

(Dispatch designs time the whole request, so they have no per-operation latency percentiles. The microredis
checked run uses 512 keys, see bug 3 below.)

### Network (real server over loopback TCP)

### Network (real server over loopback TCP), 5 runs x 2 s each: ops/s mean (std dev %) [min - max]

| design | GET 1×1 | GET 4×1 | MIX90 4×1 | GET 4×16 | SET 4×16 | MIX90 4×16 |
|---|---:|---:|---:|---:|---:|---:|
| single | 1,218 (3.2 %) [1,166 - 1,264] | 1,153 (1.0 %) [1,134 - 1,162] | 1,136 (1.2 %) [1,116 - 1,153] | 19,544 (1.1 %) [19,318 - 19,716] | 18,926 (0.7 %) [18,768 - 19,104] | 19,440 (0.9 %) [19,239 - 19,608] |
| shared | 1,144 (16.5 %) [936 - 1,301] | 873 (0.5 %) [867 - 878] | 877 (0.8 %) [870 - 888] | 12,718 (0.3 %) [12,672 - 12,767] | 12,701 (0.4 %) [12,640 - 12,768] | 12,723 (0.4 %) [12,664 - 12,792] |
| sharded | 1,144 (16.8 %) [932 - 1,301] | 876 (0.5 %) [870 - 881] | 879 (0.9 %) [873 - 892] | 12,728 (0.6 %) [12,600 - 12,800] | 12,678 (0.1 %) [12,664 - 12,688] | 12,758 (0.6 %) [12,680 - 12,856] |
| owner | 766 (8.2 %) [701 - 836] | 668 (0.7 %) [663 - 673] | 668 (0.7 %) [663 - 675] | 5,137 (6.3 %) [4,704 - 5,471] | 5,234 (5.2 %) [4,912 - 5,496] | 5,150 (4.8 %) [4,864 - 5,416] |
| dispatch | 927 (0.9 %) [918 - 936] | 692 (4.1 %) [667 - 736] | 699 (2.8 %) [668 - 715] | 10,354 (2.2 %) [10,040 - 10,584] | 10,081 (1.0 %) [9,992 - 10,240] | 10,158 (1.5 %) [10,000 - 10,336] |
| basic | 572 (0.6 %) [569 - 576] | 950 (1.5 %) [937 - 974] | 941 (1.2 %) [925 - 953] | 82 (28.1 %) [48 - 112] | 141 (82.2 %) [48 - 328] | 104 (87.0 %) [32 - 240] |
| microredis | 26 (1.7 %) [25 - 26] | 0 (0.0 %) [0 - 0] ⚠ | 0 (0.0 %) [0 - 0] ⚠ | - | - | - |

### Network, latency (batch round trip, mean over runs): p50 / p99 µs

| design | GET 1×1 | GET 4×1 | MIX90 4×1 | GET 4×16 | SET 4×16 | MIX90 4×16 |
|---|---:|---:|---:|---:|---:|---:|
| single | 805 / 981 | 3,707 / 5,406 | 3,773 / 5,529 | 3,280 / 4,964 | 3,261 / 6,044 | 3,265 / 5,494 |
| shared | 869 / 1,490 | 3,889 / 13,453 | 3,851 / 13,532 | 4,504 / 15,487 | 4,129 / 17,103 | 4,553 / 16,498 |
| sharded | 869 / 1,592 | 3,866 / 13,494 | 3,863 / 13,827 | 4,442 / 16,448 | 4,177 / 16,301 | 4,568 / 16,690 |
| owner | 1,469 / 1,944 | 5,227 / 17,446 | 5,161 / 17,681 | 11,264 / 35,922 | 10,652 / 37,758 | 10,743 / 37,861 |
| dispatch | 1,077 / 1,084 | 5,173 / 9,187 | 5,277 / 9,142 | 5,665 / 13,446 | 5,778 / 12,912 | 5,781 / 13,200 |
| basic | 1,742 / 2,004 | 3,962 / 7,955 | 4,045 / 7,638 | 1,036,085 / 3,100,487 | 1,176,917 / 3,114,543 | 1,900,053 / 3,315,203 |
| microredis | – | – | – | – | – | – |

### Network, correctness

| design | functional suite | consistency: GETs checked | SETs | torn | unpublished | backwards | final wrong | io errors | verdict |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| single | see log | 18,103 | 2,729 | 0 | 0 | 0 | 0 | 0 | PASS |
| shared | see log | 14,127 | 2,105 | 0 | 0 | 0 | 0 | 0 | PASS |
| sharded | see log | 14,130 | 2,102 | 0 | 0 | 0 | 0 | 0 | PASS |
| owner | see log | 6,280 | 952 | 0 | 0 | 0 | 0 | 0 | PASS |
| dispatch | see log | 10,397 | 1,587 | 0 | 0 | 0 | 0 | 0 | PASS |
| basic | see log | 139 | 29 | 0 | 0 | 0 | 0 | 0 | PASS |
| microredis | 10/11 | not run | | | | | | | |

Functional suite: single 34/34, shared 34/34, **sharded 32/34, owner 32/34** (both fail `set_10k`/`get_10k`, see
finding 5), dispatch 34/34, basic C 33/33, microredis 10/11 (`TTL` answered 9 right after `SET ... EX 10`;
Redis rounds to 10). All six C designs passed the TCP consistency test with 0 errors. microredis could not run it,
because it cannot hold 4 connections (below).

### Findings

1. **Without network, owner-based sharding is the best design: 1.81× one core on both mixes** (543,553 and
   536,184 ops/s) with the tightest tail (p99 4.0 µs). No locks at all; forwarding is rare because each core's
   stream is mostly its own keys. That is the same 1.78–1.81× SRAM-bus ceiling seen in Phase 1. Nothing went past it.
2. **Locks cost most under writes.** Shared seqlock falls from 1.49× (90 % GET) to 1.12× (50 % GET, p99 25 µs:
   readers retry while writers hold the line). A reader-writer lock is steadier under writes (1.16×). Splitting
   into 8 shards removes most of the contention (1.51–1.59×).
3. **A queue in front of the work is a loss unless the receiver also works.** One receiver + one worker is
   *slower* than one core (0.92×): the FreeRTOS queue handoff (~6.5 µs, Phase 2) costs more than the ~3 µs
   operation. Two owner workers reach 1.63×, a shared worker pool 1.02–1.26×.
4. **With network, the single loop wins every workload.** 1,218 ops/s with one request in flight, ~19,500
   pipelined. Two loops (shared or sharded) lose 24 % at 4×1 and 35 % pipelined. Owner loses 42 % / 74 %
   (forwarding plus a wake-up per foreign key, and replies wait for the other core). Dispatch loses 40 % / 47 %.
   About 93 % of a request is lwIP/socket work (Entry 5), done by lwIP's single thread on core 0. A second loop on
   core 0 competes with that thread instead of adding capacity. One loop on core 1 leaves core 0 to lwIP: in effect
   **the network stack is the "second worker"**. GET 1×1 on the two-loop designs varies run to run (936–1,301),
   depending on which core the connection lands on.
5. **8-shard designs cannot store a 10 KB value in the loopback build.** Each shard gets its own slice of the
   page pool. The loopback build has 18 pages (72 KiB, because 120 KB of heap is reserved for the on-chip
   clients), i.e. ~2 pages (8 KiB) per shard, so a 10 KB value does not fit one shard. Owner mode also disables
   value streaming (streaming touches the engine directly, not through the router). This is a real cost of
   per-shard memory: the largest value becomes about 1/8 of the arena.
6. **Against microredis** (same chip, same workloads, measured today):

| | microredis | basic C | final C (single) | final C best (owner) |
|---|---:|---:|---:|---:|
| engine, 90/10 GET/SET (ops/s) | 6,910 | – | 299,774 (**43×**) | 543,553 (**79×**) |
| engine, 50/50 (ops/s) | 4,232 | – | 296,909 (**70×**) | 536,184 (**127×**) |
| server, GET, 1 client, 1 in flight (ops/s) | 26 | 572 | 1,218 (**47×**) | 766 |
| server, GET, 4 clients (ops/s) | fails: out of memory | 950 | 1,153 | 668 |
| server, GET, 4 clients × 16 pipelined (ops/s) | not supported (one command per read) | 82 | 19,544 (**~750× microredis's 1-client rate**) | 5,137 |
| correctness | engine PASS (512 keys); server functional 10/11 | 33/33, consistency PASS | 34/34, all checks PASS | 32/34 (10 KB), all checks PASS |

   microredis's server costs ~37 ms per request over loopback. A bare MicroPython asyncio echo server on the same
   loopback takes **~17 ms** (control run), so about half of that is MicroPython's event loop, not microredis.
   A real external client over WiFi would change both numbers. Each microredis connection allocates a 4 KB
   buffer, and with ~26–31 KB of heap left the fragmented heap refused it in every 4-client run (38
   MemoryErrors). (Basic C at 4×16 collapses for a different reason: one segment per reply overflows lwIP's
   loopback queue, Entry 5.)

**Best design, by environment:** no network → **owner** (1.81×). With this chip running its own network stack →
**single loop on core 1** (the default). With an external network processor (planned: ESP8266 over SPI), lwIP
leaves this chip, the situation moves towards the no-network case, and owner/sharded are worth testing again.

### Bugs found (fixed)

1. **In the benchmark harness, not the server.** The first two network attempts had one client time out in the
   first repetition of the 4-client runs (`errno=11`). Instrumenting it showed 5-byte replies (`$-1`, key
   missing) where 39 bytes were expected. The new consistency test left its 512 keys behind, so the preload's SETs got
   `-OOM`, and the preload never checked replies. Now the consistency test deletes its keys and the preload aborts
   loudly on any reply other than `+OK`. After the fix: 0 errors in all 180 C runs. Ruled out first: server buffer
   allocation, accept failures, loopback queue size, lwIP drops (lwIP counters showed 0). Earlier attempts are kept
   in `results/entry7_first_attempt/` and `results/entry7_second_attempt/`.
2. **microredis's server never ran on MicroPython.** `writer.get_extra_info('peername', default)` takes no default
   argument in MicroPython, so every accepted connection raised `TypeError`. This is now a 4th build-time patch in
   `tools/build_mpy.py`. It went unnoticed before because WiFi never came up.
3. **microredis cannot hold 1024 keys with distinct 32 B values** (MemoryError while preloading). Its timed runs
   still use 1024 keys because every key shares one value object; the checked run uses 512 keys.

### Corrections

- **Entry 5 says the loopback queue was 32 packets.** lwIP accepts at most 16 (`CONFIG_LWIP_LOOPBACK_MAX_PBUFS`),
  the 32 was silently ignored, and Entries 5–6 ran with the default 8. Now set to 16. (An unlimited queue made no
  difference to this entry's numbers.)

Raw data: `results/entry7_nonet.jsonl`, `results/entry7_microredis_engine.jsonl`, `results/entry7_lbf_*.jsonl`,
`results/entry7_microredis_loopback.log`; tables: `python tools/summarize_final.py nonet|net` (saved as
`results/entry7_nonet_tables.md`, `results/entry7_net_tables.md`). New server modes: `owner`, `dispatch`
(`kvcache/variants/owner.cfg`, `dispatch.cfg`, `kvcache/main/route.c`).
