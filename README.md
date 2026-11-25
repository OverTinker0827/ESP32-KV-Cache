# ESP32 KV Cache

A Redis-compatible, in-memory key-value cache written in C for the **ESP32 (Xtensa)**, built with
ESP-IDF and optimized for the chip's limited SRAM, dual cores and WiFi stack.

The project starts from [microredis](https://github.com/mateuszsury/microredis), a Redis-style server
written in MicroPython for microcontrollers. It benchmarks microredis first, then builds a basic C
port and a progressively hardware-optimized C version, measuring each step on the same board.

## Results so far

On-device storage-engine benchmark, ESP32 @ 240 MHz, 16 B keys, 32 B values, sequential keys
(latest entry; full tables, method and raw data in [results.md](results.md)):

| | microredis | basic C | optimized C |
|---|---:|---:|---:|
| SET ops/s | 891 | 80,792 | **434,046** (487× microredis) |
| GET ops/s | 1,585 | 76,995 | **357,079** (225×) |
| DEL ops/s | 2,188 | 99,698 | **390,579** (179×) |
| GET p50 / p99 latency | 200 µs / 4,027 µs | 12.8 µs / 17.1 µs | **2.7 µs / 3.2 µs** |
| Max keys | 1,223 | 3,659 | **3,869** |
| Bytes per key | 77 | 68 | **60** |

With simulated WiFi load on the other core the optimized engine loses only 4 %; on the same core it
loses 42 %, which is why the cache is pinned away from the WiFi core.

Multicore (both cores, 90 % GET, vs one core without a lock):

| design | speed-up |
|---|---:|
| shared table, FreeRTOS `portMUX` | 0.96× |
| shared table, readers-writer lock | 1.63× |
| 2 shards, seqlock | 1.69× |
| receiver → lock-free ring → 2 shard-owning workers | 1.53× |
| 2 shards each owned by one core, no lock (ceiling: shared SRAM bus) | 1.78× |

Network benchmarks (throughput, p95/p99 latency, CPU per core) are pending a power fix on the
test board; see [Hardware notes](#hardware-notes).

## Supported commands

`GET` `SET` (`EX` `PX` `NX` `XX` `KEEPTTL`) `SETEX` `DEL` `EXISTS` `TTL` `PTTL` `EXPIRE` `PEXPIRE`
`PING` `ECHO` `DBSIZE` `FLUSHALL` `FLUSHDB` `INFO` `CONFIG SET maxmemory-policy` `QUIT`, plus the
`COMMAND`/`CLIENT` replies that `redis-cli` and `redis-py` expect. Speaks RESP2 (and inline commands
for telnet/netcat).

## Design

Every optimization is a Kconfig switch, so "basic" is all switches off, "optimized" is all on, and
each one can be turned off on its own to measure what it contributes.

| Area | Basic C | Optimized C |
|---|---|---|
| Memory | one `malloc` per entry | store carved once into 4 KiB pages after WiFi is up; slab classes with 8-byte granularity; no allocation per request, so no heap fragmentation |
| Index | chained hash table, doubles and rehashes | open addressing, 4 bytes per slot (21-bit entry reference + 11-bit hash tag), sized exactly with one multiply (`fastrange`), backward-shift delete (no tombstones) |
| Hash | FNV-1a | FNV-1a + murmur3 `fmix32` (no clustering on sequential keys like `sensor:0001`) |
| Eviction / TTL | none / lazy | CLOCK per slab class with page stealing; lazy expiry plus an incremental sweep when idle |
| Server | one FreeRTOS task per client, copies every argument, one `send()` per reply | `select()` event loop, zero-copy RESP parser, all replies of a packet sent with one `send()` |
| Placement | default | hot path (hash, lookup, parser, dispatch) in IRAM; lwIP/WiFi IRAM options; lwIP pinned to core 0 |
| Multicore | unpinned tasks | single loop on core 1 or core 0, two loops sharing one locked table, or two loops with the table split into two shards |

## Repository layout

```
kvcache/            ESP-IDF project (the C cache)
  main/kv_basic.c     basic engine
  main/kv_opt.c       optimized engine
  main/server_*.c     basic and event-loop servers
  main/resp.c         zero-copy RESP parser
  main/cmd.c          command layer (shared by all variants)
  main/engine_bench.c on-device engine benchmark (prints a RESULT line)
  variants/*.cfg      sdkconfig fragments that define each variant
micropython/        microredis boot script, on-device benchmark, diagnostics
tools/              build, flash, serial and network-benchmark scripts
results/            raw results (JSON, serial logs)
results.md          append-only results log
```

## Getting started

Prerequisites: Python 3.10+, git, an ESP32 on a serial port (COM5 below).

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install esptool mpremote pyserial redis "mpy-cross==1.29.0.post2"
copy wifi_secrets.example.json wifi_secrets.json   # then fill in a 2.4 GHz SSID and password
```

### microredis baseline

```powershell
git clone https://github.com/mateuszsury/microredis ref/microredis
.\.venv\Scripts\esptool.exe --port COM5 erase-flash
.\.venv\Scripts\esptool.exe --port COM5 write-flash 0x1000 firmware\ESP32_GENERIC-v1.29.0.bin
.\.venv\Scripts\python.exe tools\build_mpy.py      # compiles to .mpy and applies compatibility patches
cd build\microredis_mpy; ..\..\.venv\Scripts\mpremote.exe connect COM5 cp -r microredis :; cd ..\..
.\.venv\Scripts\mpremote.exe connect COM5 mip install bisect
.\.venv\Scripts\mpremote.exe connect COM5 run micropython\mr_engine_bench.py
```

MicroPython firmware: [ESP32_GENERIC v1.29.0](https://micropython.org/resources/firmware/ESP32_GENERIC-20260824-v1.29.0.bin)
(save it as `firmware\ESP32_GENERIC-v1.29.0.bin`).

### C cache

Install [ESP-IDF v5.5.6](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32/get-started/),
then build and flash a variant. Each variant gets its own build directory.

```powershell
.\tools\build_variant.ps1 -Name opt -Cfg opt,netopt -Flash        # optimized, with WiFi
.\tools\build_variant.ps1 -Name basic -Cfg basic -Flash           # basic C port
.\tools\build_variant.ps1 -Name opt_engine -Cfg opt,nowifi -Flash # engine benchmark only, WiFi off
.\.venv\Scripts\python.exe tools\serial_wait.py COM5 "READY|DONE" 60
```

### Network benchmark

```powershell
.\.venv\Scripts\python.exe tools\bench.py --host <board-ip> --label opt --pipeline 1,16 --fill
```

Measures requests/s and avg/p50/p95/p99 latency for PING/SET/GET at 1, 4 and 8 connections, and for
the C cache also CPU utilization per core (from `INFO`) and server-side execution time.

## Hardware notes

- The test board is a classic **ESP32-D0WD-V3** (no PSRAM). The code targets ESP32 and should port
  to ESP32-S3; PSRAM tiering needs a board with PSRAM.
- The test board's 3.3 V supply collapses when the radio starts (brownout reset). A 100–470 µF
  capacitor across 3V3 and GND, a better cable or a better board fixes this. Until then only
  on-device benchmarks with WiFi off are possible.
- `wifi_secrets.json` is git-ignored; the build reads it to embed the credentials.

## Roadmap

- Network benchmarks of all versions once the board's power is fixed
- Multicore study: sharding, read-write locks, queue-based workers, batching, per-core counters,
  adaptive load balancing
- Server running inside the lwIP task (no socket-layer handoffs)
- Memory study across value sizes; WiFi-buffer vs capacity trade-off
- Adaptive memory-aware caching (SRAM/PSRAM tiering on a PSRAM board)
- Energy per 1,000 requests (needs an INA219 or USB power meter)
