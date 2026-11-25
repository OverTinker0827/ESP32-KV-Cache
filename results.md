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
