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
