# How the ESP32 KV Cache works — explained in plain language

This document explains, without assuming you have read the code, what this project built, how a request
travels through it, every technique it uses, why each one matters, which ones exist *because* of how the
ESP32 is built, and how much each one improved over the starting point. Every number here comes from a
measurement on the board; the full tables are in [`results.md`](../results.md) (Entries 1–6).

---

## 1. The one-paragraph version

We took **microredis**, a Redis-style cache written in MicroPython, and rebuilt it in C for the ESP32. The
first C version ("basic C") was a straightforward port: about **50–90 times faster** than microredis just by
not being Python. The optimized version then rethought every layer around the ESP32's actual hardware —
tiny memory, two cores that share one memory bus, a WiFi stack that lives on one of those cores, and
program code that sits in slow flash behind a small cache. The engine now does a lookup in **about
3 microseconds**, stores **3× more keys** than microredis in the same chip, handles values up to 64 KB
without needing spare memory, and has been tested with 72 correctness checks on the board. Along the way we
learned that on this chip **the network stack, not the cache, is where almost all the time goes**, which
changed which optimizations are worth doing.

### Where things stand (latest numbers)

| | microredis | basic C | optimized C (final) |
|---|---:|---:|---:|
| SET per second (engine only, on the chip) | 891 | 80,792 | **404,350** |
| GET per second (engine only) | 1,585 | 76,995 | **338,329** |
| Typical GET time (engine only) | 200 µs | 12.8 µs | **2.9 µs** |
| Keys that fit (16 B keys, 32 B values) | 1,223 | 3,416 | **4,056** |
| Keys that fit (IoT key names, mixed value sizes) | – | 766 | **1,645** |
| Requests/s end to end, one at a time (over loopback TCP) | not measured¹ | 509 | **1,365** |
| Requests/s end to end, 4 clients × 16 pipelined | not measured¹ | 48 | **19,837** |
| Largest value with the normal 40 KB memory reserve | – | not measured | **64 KB** (previous optimized engine: failed at 10 KB) |

¹ microredis only runs over WiFi, and this board's power supply cannot run the radio yet (see section 10).

---

## 2. The chip, in plain terms

To understand the design you need five facts about the ESP32 on this board (an ESP32-D0WD-V3):

1. **Two processor cores, 240 MHz each.** Think of them as two workers. One of them (core 0) is also
   where Espressif runs the WiFi driver and the TCP/IP software (called **lwIP**). Anything else we put on
   core 0 competes with the network.
2. **Very little working memory.** About 520 KB of fast internal RAM in total, of which roughly
   **280 KB** is free before WiFi starts (WiFi and lwIP then take a good part of it), and there is no extra
   external RAM (PSRAM) on this board. A laptop has tens of thousands of times more.
3. **Program code lives in flash memory, which is slow**, and is read through a small **32 KB cache per
   core**. If the code you need is in the cache, it runs at full speed; if not, the chip has to fetch it
   from flash, which we measured at **about 4 µs per miss — roughly 900 instructions' worth of time**.
   There is a small region of fast RAM for code (**IRAM**) where you can put code that must never wait.
4. **The internal RAM has no data cache.** On a PC, each core has its own cache of RAM and the hardware
   spends a lot of effort keeping those caches consistent. On the ESP32 both cores read and write the same
   RAM directly over a **shared bus**. This changes which multi-core tricks pay off (section 7).
5. **The network path is software running on a small CPU.** Every packet is processed by lwIP in software,
   and every `send()`/`recv()` our code makes is a hand-off to lwIP's task. That is expensive here.

Everything in this project is a response to one of these five facts.

---

## 3. Where we started: microredis

microredis is a feature-rich Redis clone in MicroPython. Before we could even benchmark it, we found:

- **It did not run on MicroPython as published.** Three small code issues stopped it from compiling, and it
  needed a library (`bisect`) that MicroPython does not ship. We patched these at build time without
  touching how it works.
- **Its own code eats most of the memory.** Loading it used 107 KB of the ~164 KB MicroPython heap, leaving
  about 40 KB for actual data. It ran out of memory below 2,000 keys — partly because Python dictionaries
  need one large contiguous block to grow, and the heap was too fragmented to provide it.
- **It does a lot of work per command.** It keeps five separate dictionaries per key (data, type, expiry,
  version, last access). One SET touches them 6–7 times; one GET touches them 5 times and even *writes* to
  one of them.
- **Sequential key names are slow in MicroPython.** Keys like `key:000000000123` hash into very few buckets
  of MicroPython's dictionary, so lookups walk long chains: GET was 4× slower with sequential keys than with
  random ones.
- **Garbage-collection pauses**: its 99th-percentile GET took ~4 ms against a typical 0.2 ms.

The end result: ~900 SETs and ~1,600 GETs per second on the chip.

---

## 4. The life of a request

Here is what happens when a client asks the cache `GET sensor:temp:room07:00123`, in the final design:

```
 client (laptop / another device)
   |
   |  WiFi radio
   v
+----------------------------------- core 0 ------------------------------------+
|  WiFi driver  ->  lwIP (TCP/IP in software)                                    |
|  (also: the rest of the system; in our tests, a simulated WiFi load)           |
+--------------------------------------------------------------------------------+
   |  data handed to our socket (lwIP "core locking": no extra task switch)
   v
+----------------------------------- core 1 ------------------------------------+
|  EVENT LOOP (one task, owns all connections)                                   |
|   1. select(): which connections have data?                                    |
|   2. recv() into that connection's 4 KB receive buffer                         |
|   3. PARSER: find each complete command in the buffer, without copying it      |
|   4. COMMAND LAYER: recognise GET/SET/DEL/... and call the engine              |
|   5. ENGINE: hash key -> look in index -> find entry in arena -> copy value     |
|      straight into the 4 KB transmit buffer as a Redis reply                   |
|   6. repeat 3-5 for every command that arrived in the same packet              |
|   7. ONE send() for all the replies                                            |
+--------------------------------------------------------------------------------+
   |
   v  lwIP -> WiFi -> client
```

Key points in that flow:

- **The cache runs on core 1, the network on core 0.** They work in parallel like a two-stage assembly line.
- **Nothing is copied or allocated per request.** The command is read where it landed in the receive buffer,
  the reply is written straight into the transmit buffer, and the engine never calls `malloc`.
- **Many commands, one reply packet.** If a client sends 16 commands at once ("pipelining"), all 16 are
  answered with a single `send()`.
- **Big values stream.** A 10 KB value does not need a 10 KB buffer: it is copied piece by piece between the
  network and the store through the normal 4 KB buffers.

---

## 5. The architecture, layer by layer

```
  network (lwIP, WiFi)                      <- Espressif's code, tuned by configuration
  -------------------------------------------------------------------------------
  server: event loop, connections           <- kvcache/main/server_opt.c
  RESP parser (Redis protocol)              <- kvcache/main/resp.c
  command layer (GET, SET, DEL, TTL, ...)   <- kvcache/main/cmd.c
  -------------------------------------------------------------------------------
  storage engine                            <- kvcache/main/kv_opt.c (+ kv_stream.inc)
     index (where is key X?)
     arena of 4 KB pages, cut into fixed-size chunks (where entries live)
     eviction, expiry, large-value chains, prefix dictionary
```

The basic C version has the same layers but simpler choices in each: one FreeRTOS task per client, a parser
that copies every argument, a reply `send()` per command, and an engine that calls `malloc` for every key.

---

## 6. Every technique, why it matters, and what it bought

Each entry says **what** it is, **why** it matters, whether it is **ESP32-specific**, and the **measured
effect**.

### 6.1 Writing it in C instead of MicroPython

- **What:** compiled C instead of interpreted Python bytecode.
- **Why it matters:** an empty loop iteration in MicroPython costs ~2 µs (~500 instructions); a C function
  call costs a few instructions. And C lets us control memory layout exactly.
- **ESP32-specific?** No — this is true on any small chip.
- **Effect:** basic C vs microredis: **SET 91×, GET 49×, DEL 46× faster**; 3× more keys.

### 6.2 One fixed memory area for the data (the "arena"), no `malloc` per request

- **What:** at startup, after WiFi and lwIP have taken what they need, the cache takes most of the remaining
  memory as **4 KB pages** and keeps it. Entries are placed in those pages; nothing is allocated or freed
  from the general heap while serving requests.
- **Why it matters:** with only ~280 KB available, the general-purpose heap fragments quickly (microredis
  failed with free memory still available). `malloc` also takes a lock and searches free blocks on every
  call. A private arena avoids both, and guarantees a fixed amount of memory stays free for WiFi.
- **ESP32-specific?** Yes in spirit: it exists because the heap is tiny and shared with the WiFi stack.
- **Effect:** part of the basic→optimized speed-up (`malloc`/`free` was ~1–2 µs per operation); and it
  makes the memory budget predictable (a 40 KB reserve is always kept for the network).

### 6.3 Size classes ("slabs") with page-fit chunk sizes

- **What:** each 4 KB page is cut into equal chunks of one size (8, 12, 16 … bytes, up to 4,088). An entry
  goes into the smallest chunk it fits. Above 256 bytes the chunk sizes are chosen as 4,088 ÷ n (584, 680,
  816, 1,020 …) so that a page always divides into whole chunks with almost nothing left over.
- **Why it matters:** fixed-size chunks never fragment, and allocating one is just "take the next free
  chunk of this size". Choosing sizes that divide the page evenly wastes less: a 532-byte entry fits 7 per
  page in a 584-byte chunk, but only 6 per page in a 640-byte chunk.
- **ESP32-specific?** Not inherently (memcached uses slabs), but the page-fit tuning matters because
  every wasted byte counts here.
- **Effect:** +14.5 % more 512-byte values than the previous engine. (Honest limit: for ~1 KB values a 4 KB
  page only holds 3, so plain `malloc` still stores more of them — see section 8.)

### 6.4 A compact entry format

- **What:** each entry is `[4-byte header][expiry, only if it has a TTL][key][value]`. The previous engine
  always spent 8 bytes on the header.
- **Why it matters:** with 16-byte keys and 32-byte values, 4 bytes is ~8 % of the entry.
- **ESP32-specific?** It matters because memory is so scarce.
- **Effect:** together with finer size classes, the final engine stores 5–15 % more keys at most value sizes
  than the previous engine (2 % fewer at 128 B and 1 KB, where the class rounding happens to be worse).

### 6.5 A compact index: 4 bytes per key, open addressing

- **What:** to find a key, we compute a hash and look in one flat array. Each slot is 4 bytes: 21 bits say
  where the entry lives (which page, which chunk), and 11 bits are a "tag" taken from the hash. Collisions
  are handled by checking the next slot ("linear probing"); deletions shift later entries back instead of
  leaving tombstones.
- **Why it matters:** the tag lets us skip almost every wrong slot without even looking at the entry. One
  flat array is cheaper than chasing pointers through linked lists (what basic C does). At ~4.4 bytes per
  key (at 90 % fill) the index is a small part of each entry.
- **ESP32-specific?** One small trick is: mapping the hash to a slot uses a "multiply-high" instruction that
  the ESP32's Xtensa core has, so the table size does not have to be a power of two.
- **Effect:** part of the 4–5× basic→optimized speed-up.

### 6.6 A good hash function

- **What:** FNV-1a followed by a "finalizer" (murmur3's `fmix32`) that mixes all bits.
- **Why it matters:** key names in IoT are often sequential (`sensor:0001`, `sensor:0002` …). A weak hash
  clusters them — exactly what slowed microredis 4× on such keys.
- **Effect:** the C engines run the same speed on sequential and random keys (±0.3 %).

### 6.7 Building replies without `printf`

- **What:** the `$32\r\n` header of a Redis reply is written by a ten-line integer-to-text routine.
- **Why it matters:** the C library's `snprintf` is a general-purpose formatter that runs from flash; it cost
  ~5 µs per GET.
- **Effect:** basic C's GET went from 76,995 to 129,760 per second with just this change (+69 %).

### 6.8 Hot code in IRAM

- **What:** the functions every request goes through (hash, index lookup, parser, command dispatch) are placed
  in the fast code RAM instead of flash.
- **Why it matters:** a flash-cache miss costs ~4 µs. When WiFi's large code base runs on the same core, it
  keeps pushing our code out of the cache.
- **ESP32-specific?** Yes, completely.
- **Effect (measured, honest):** no difference when the chip is quiet; **+7.8 %** when WiFi-like load shares
  the core; **−3.5 %** when that load is on the *other* core (the two cores compete for the RAM bus).
  IRAM is also nearly full (99.4 % in the WiFi build), so it is a budget to spend carefully.

### 6.9 Eviction: CLOCK per size class, plus page stealing

- **What:** when memory is full, an approximation of "least recently used" (CLOCK: one "recently used" bit
  per entry, second chance before eviction) picks what to drop, within the size class that needs space. If a
  size class has no pages at all, a whole page is taken from the class that has the most, and fully empty
  pages are always reclaimed first.
- **Why it matters:** a cache must keep working when full. Without page stealing, memory gets stuck in size
  classes nobody needs any more ("slab calcification").
- **Effect:** the engine test suite runs 6,000 mixed operations at full memory with ~4,000 evictions and
  zero corrupted values.

### 6.10 Expiry (TTL) without a timer per key

- **What:** expired keys are removed when someone touches them, plus a small sweep of the index whenever the
  server is idle. The clock is only read for entries that actually have an expiry.
- **Why it matters:** no timers, no background thread per key; and not reading the clock on every request
  saved measurable time.

### 6.11 Large values as chains inside the arena, and streaming them

- **What:** a value bigger than one chunk is stored as a chain of 4 KB chunks. When such a value arrives over
  the network, the server reads `SET key <length>`, reserves the chain immediately (and protects those pages
  from eviction), then copies each network piece straight into it. Large GET replies are sent the same way,
  piece by piece through the normal 4 KB transmit buffer.
- **Why it matters:** before this, a 10 KB value needed 10–16 KB of extra buffer from the small reserve and
  failed (`-OOM`) on the real deployment configuration.
- **ESP32-specific?** It exists because the heap is tiny.
- **Effect:** the full test suite (including 10 KB values) passes at the real 40 KB reserve; values up to
  64 KB work.

### 6.12 Prefix compression for key names

- **What:** IoT key names repeat long prefixes (`sensor:temp:room07:`). The engine learns up to 63 such
  prefixes (up to 24 bytes, ending in `:`) and stores a 1-byte number instead of the prefix in each entry.
- **Why it matters:** with short values, the key can be half of the entry.
- **Effect:** **+14 % to +35 % more keys** for IoT names with 16–128-byte values, and **+95 % more keys than
  the previous engine** in the mixed-size test. It makes the engine itself 23–39 % slower, but because the
  engine is only ~7 % of a request (section 9), the end-to-end cost is not measurable. It is on by default.

### 6.13 One event loop instead of one task per client

- **What:** a single task watches all connections with `select()` and serves whichever has data.
- **Why it matters:** each FreeRTOS task needs its own stack (4 KB+), which is a lot of RAM here, and switching
  between tasks costs time.
- **Effect:** part of the 2.6× end-to-end advantage over basic C with one request at a time.

### 6.14 Zero-copy parsing and batched replies

- **What:** the parser points at the command where it already sits in the receive buffer (no copies); all
  replies to one packet's worth of commands go out in one `send()`.
- **Why it matters:** every `send()` is expensive here (~100–150 µs, section 9). Basic C sends one TCP
  segment per reply, which with 16 pipelined commands overwhelmed the network stack and caused
  retransmission stalls.
- **Effect:** with 16 pipelined commands per client the optimized server does **280–1,000×** the requests of
  basic C over loopback TCP (that extreme gap is partly a loopback artifact, but one segment per reply is
  expensive anywhere).

### 6.15 Putting the cache on core 1, away from WiFi

- **What:** the event loop is pinned to core 1; WiFi and lwIP stay on core 0.
- **Why it matters:** WiFi work interrupts and pre-empts whatever runs on its core.
- **ESP32-specific?** Yes — it follows from Espressif putting the network on core 0.
- **Effect:** with simulated WiFi load on the other core the engine loses only **2–4 %**; on the same core it
  loses **42 %** and its slowest requests wait ~830 µs.

### 6.16 lwIP "core locking"

- **What:** an ESP-IDF setting that lets our task run lwIP's code directly (under a lock) instead of sending
  every socket call as a message to lwIP's task and waiting for it.
- **Why it matters:** it removes a task switch, and a cross-core wake-up, from every `send()`/`recv()`.
- **ESP32-specific?** It is an lwIP/ESP-IDF option, and it matters because the network path dominates.
- **Effect:** **+5 %** quiet and **+33 %** under WiFi-like load, end to end.

---

## 7. The multi-core story (what we tried, and why most of it does not help end to end)

The ESP32 has two cores, so the obvious idea is to use both for the cache. We tested this carefully
(Entries 3–5), including the ideas proposed by the team:

| Design (both cores running the cache, 90 % GETs) | Speed vs one core |
|---|---:|
| One shared table protected by FreeRTOS's standard lock (`portMUX`) | **0.96×** — no gain |
| One shared table, plain compare-and-swap lock | 1.13× |
| Table split into shards, one lock each | 1.45× |
| One table with a readers-writer lock | 1.63× |
| Shards + seqlock (readers never lock) | 1.69× |
| Each core owns half the keys, no locks at all | **1.78×** — the ceiling |
| "Receiver task" hands requests to workers through FreeRTOS queues | **0.35×** — slower |
| Same, through a lock-free ring buffer | 1.53× |

What we learned, in plain terms:

- **Two cores never give 2× here.** Even with no locking at all, each operation slows by ~11 % when both cores
  work at once, because they share one memory bus. 1.78× is the ceiling on this chip.
- **The lock matters more than the number of cores.** FreeRTOS's standard lock switches off interrupts and is
  so expensive that two cores were no faster than one; a plain compare-and-swap (one special instruction the
  ESP32 has) costs only 4 %.
- **Handing work between tasks is expensive.** A FreeRTOS queue hand-off costs ~6.5 µs — twice the work it
  carries — so a "receiver + workers" design made things 3× slower until we replaced the queue with a
  lock-free ring buffer.
- **No data cache changes the rules.** On a PC, giving each core its own counters avoids "cache-line bouncing";
  on the ESP32 there is no cache to bounce, so per-core counters only saved 1.9 %. (They are still needed for
  correctness: a shared counter updated without care lost 316 updates in 0.4 s.)
- **But end to end, the second core loses.** When the real server runs over TCP, the second event loop has to
  live on core 0 — where the network stack already does ~93 % of the work. Splitting the cache over both cores
  made the real server **~30 % slower**. This is Amdahl's law: speeding up the part that takes 7 % of the time
  cannot help much, and stealing time from the part that takes 93 % hurts.

**So the final server uses one event loop on core 1**, and keeps the best two-core design (shards + seqlock)
available for situations where core 0 has spare capacity.

### Load imbalance (Entry 4)

When one set of keys gets most of the traffic, one core can be overloaded. We tested the team's
"adaptive load balancing" ideas and a hot-key replication idea:

- **Spill-over** (send work to the other core when one's queue is much longer): **1.59×** when 91 % of traffic
  hit one shard; never meaningfully worse otherwise. The safest policy.
- **Shard migration** (move whole shards to the less busy core): best when hot keys are spread out (**1.18×**),
  but it cannot split one hot shard; without hysteresis it thrashed (60 moves in 0.4 s), with it 0–3 moves.
- **Hot-key replication** (copies of the hottest keys on both cores): 1.53× for a hot shard if most requests
  are reads; little help when many are writes.

These matter for the "owner per core" architecture; with the single-loop server they are not needed today.

---

## 8. The memory story

| Keys that fit in the same memory | microredis | basic C | previous engine | final engine |
|---|---:|---:|---:|---:|
| 16 B keys, 32 B values | 1,223 | 3,416 | 3,869 | **4,056** |
| IoT keys, 32 B values | – | 3,076 | 3,339 | **4,784** |
| IoT keys, mixed 16–512 B values | – | 766 | 844 | **1,645** |
| 1 KB values | – | **225** | 159 | 156 |

Honest notes:

- For very small values the limit is the size of the **index** (5,447 keys), not the memory for entries:
  the index is sized for an average entry of ~44 bytes. A bigger index would waste memory when values are big.
- For ~1 KB values, `malloc` (basic C) wins, because a 4 KB page holds only 3 of them. Larger pages or
  chaining medium values would fix it (future work).
- **Adaptive memory reserve** (the research idea): we built a controller that gives memory back to the
  system when free memory runs low and takes it back later, and tested it against simulated bursts of
  network-buffer demand (up to 64 KB in ~5 ms). Result: a reactive controller cannot keep up with such sudden
  bursts — every adaptive setting let 10–13 % of buffer allocations fail, while a fixed 80 KB reserve let none
  fail. Conclusion: **for bursty demand, keep a fixed headroom sized for the largest burst; adaptation can only
  help with slowly changing demand.** It is available as an option, off by default.

---

## 9. The big lesson: on this chip the network dominates

We measured the network path on the chip itself using its loopback interface (client and server on the same
board, real TCP in between):

- A bare **echo** server (no parsing, no cache) needs **545–680 µs per round trip**.
- The full cache server needs **~735 µs**. So the cache — parser, commands, engine — adds only **~50 µs**,
  about **7 %** of a request.
- Inside the server, each `recv()` took ~150 µs and each `send()` ~107 µs of waiting on the network stack.

This is why:
- optimizing the engine further barely changes end-to-end speed,
- prefix compression's engine cost is invisible end to end,
- a second core running the cache hurts (it steals time from the network stack),
- the most promising next step is to make the network path itself cheaper (section 10).

---

## 10. What is still open

- **Real WiFi.** This board's 3.3 V supply collapses when the radio starts (it resets, even on a wall
  charger). A 470 µF capacitor across 3V3 and GND, or another board, fixes it. Until then, network numbers
  come from loopback and a WiFi-load simulator. Real WiFi measurements are needed to confirm them, to
  benchmark microredis over the network, and to calibrate the simulator and the memory reserve.
- **Energy per 1,000 requests** needs a current sensor (INA219) or a USB power meter.
- **PSRAM tiering** (fast internal RAM for hot keys, external RAM for the rest) needs a board with PSRAM.
- **Running the server inside lwIP's own task** (no socket layer at all) — the measurement in section 9
  says this is where the remaining time is.

---

## 11. How we got here, step by step

| Step | What we did | What it showed |
|---|---|---|
| Entry 1 | Ran microredis on the board; wrote basic C and optimized C engines | C is 50–90× faster; optimized engine ~5× faster than basic C |
| Entry 2 | Measured each optimization separately; added per-operation latency and a WiFi-load simulator | `snprintf`, locks, `malloc` explained the gaps; IRAM helps only under same-core load; pinning to core 1 isolates the cache |
| Entry 3 | Tested 11 multi-core designs + queue/ring hand-offs + counters | 1.78× ceiling from the shared bus; FreeRTOS lock gives no gain; queues cost more than they carry |
| Entry 4 | Created real load imbalance; tested spill-over, migration, replication; reran Phase 1 under WiFi load | spill-over safest; migration needs hysteresis; only lock-free sharding keeps a gain under WiFi load |
| Entry 5 | Put the winners into the real server; tested it end to end over loopback | the network is ~93 % of a request; one loop on core 1 + lwIP core locking is fastest |
| Entry 6 | Rebuilt the engine for memory: compact entries, page-fit classes, prefixes, chains, streaming | up to +95 % more keys, 64 KB values in fixed memory, no end-to-end slowdown |

Mistakes we made and corrected along the way are recorded in each entry's "Corrections" section — for example
an engine slowdown introduced in Entry 5 that went unnoticed because it was invisible end to end, found and
mostly fixed in Entry 6.

---

## 12. Glossary

- **Cache / key-value store:** a program that stores values under names (keys) in memory and returns them fast.
- **Redis / RESP:** a popular cache and its simple text protocol (`*2\r\n$3\r\nGET\r\n...`); our server speaks it,
  so standard Redis tools work.
- **lwIP:** the TCP/IP software that handles networking on the ESP32.
- **IRAM:** the ESP32's small, fast memory for program code.
- **Flash cache:** the 32 KB-per-core cache that holds recently used code from flash memory.
- **Arena / page / chunk / size class:** our private memory area, split into 4 KB pages, each cut into equal
  chunks of one size class.
- **Index / hash / tag:** the lookup table that says where each key's entry is; the hash is a number computed
  from the key; the tag is a few bits of it stored in the table to skip wrong slots quickly.
- **Eviction / CLOCK:** dropping old entries to make room; CLOCK is a cheap way to approximate "least recently used".
- **TTL:** "time to live", an expiry time on a key.
- **Pipelining:** a client sending several commands before waiting for answers.
- **Lock, CAS, seqlock, readers-writer lock:** ways to keep two cores from corrupting shared data; CAS is a single
  atomic hardware instruction; a seqlock lets readers proceed without locking and retry if a write happened.
- **Shard:** one part of a table split into independent pieces.
- **Amdahl's law:** speeding up one part of a job only helps in proportion to how much of the time that part takes.
