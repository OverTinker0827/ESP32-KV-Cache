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
