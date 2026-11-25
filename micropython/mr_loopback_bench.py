# Entry 7: microredis's real asyncio server over loopback TCP (127.0.0.1), no radio.
# Client coroutines run in the same MicroPython process (MicroPython uses one core, so client
# and server share it; the C loopback test puts clients on core 0 and the server on core 1).
# microredis parses one command per read, so only one request in flight per connection (P=1).
# 1. short functional check; 2. GET with 1 client, GET with 4 clients, 90/10 mix with 4
# clients: 5 repetitions x 2 s each. Prints "RESULT {json}" lines and MRLBDONE.
import gc
import json
import machine
import os

machine.freq(240_000_000)
try:
    import asyncio
except ImportError:
    import uasyncio as asyncio

from microredis.main import MicroRedisServer  # noqa: E402

PORT = 6380
NKEYS = 256
VAL = b"v" * 32
REPS = 5
RUN_MS = 2000


def resp(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


def key(i):
    return b"key:%012d" % i


async def read_reply(r):
    line = await r.readline()
    if line[:1] == b"$":
        n = int(line[1:-2])
        if n >= 0:
            line += await r.readexactly(n + 2)
    return line


async def functional():
    r, w = await asyncio.open_connection("127.0.0.1", PORT)
    cases = [
        (resp(b"PING"), b"+PONG\r\n"),
        (resp(b"SET", b"a", b"1"), b"+OK\r\n"),
        (resp(b"GET", b"a"), b"$1\r\n1\r\n"),
        (resp(b"GET", b"nope"), b"$-1\r\n"),
        (resp(b"SET", b"a", b"22", b"NX"), b"$-1\r\n"),
        (resp(b"SET", b"b", b"x", b"NX"), b"+OK\r\n"),
        (resp(b"EXISTS", b"a", b"b", b"c"), b":2\r\n"),
        (resp(b"DEL", b"a", b"b", b"c"), b":2\r\n"),
        (resp(b"TTL", b"a"), b":-2\r\n"),
        (resp(b"SET", b"e", b"v", b"EX", b"10"), b"+OK\r\n"),
        (resp(b"TTL", b"e"), b":10\r\n"),
    ]
    ok = 0
    fails = []
    for req, exp in cases:
        w.write(req)
        await w.drain()
        got = await read_reply(r)
        if got == exp or (isinstance(exp, tuple) and got in exp):
            ok += 1
        else:
            fails.append((req[:20], got[:20]))
    w.close()
    await w.wait_closed()
    print("RESULT " + json.dumps({"part": "mr_functional", "pass": ok, "fail": len(fails),
                                  "fails": [str(f) for f in fails]}))


counts = [0, 0, 0, 0]
errs = [0, 0, 0, 0]
stop = False


async def client(cid, mix):
    try:
        await client_body(cid, mix)
    except OSError as e:  # server out of memory -> connection reset
        errs[cid] += 1
        print("client", cid, "error", e)


async def client_body(cid, mix):
    r, w = await asyncio.open_connection("127.0.0.1", PORT)
    i = cid * 37
    gets = [resp(b"GET", key(j)) for j in range(16)]
    sets = [resp(b"SET", key(j), VAL) for j in range(16)]
    while not stop:
        j = i % 16
        if mix and i % 10 == 0:
            w.write(sets[j])
            await w.drain()
            await r.readexactly(5)
        else:
            w.write(gets[j])
            await w.drain()
            await r.readexactly(39)
        counts[cid] += 1
        i += 1
    w.close()
    await w.wait_closed()


async def bench(name, nclients, mix, rep):
    global stop
    for c in range(4):
        counts[c] = 0
        errs[c] = 0
    stop = False
    gc.collect()
    tasks = [asyncio.create_task(client(c, mix)) for c in range(nclients)]
    await asyncio.sleep_ms(200)  # connections up
    gc.collect()
    base = sum(counts)
    t0 = __import__("time").ticks_ms()
    await asyncio.sleep_ms(RUN_MS)
    n = sum(counts) - base
    dt = __import__("time").ticks_diff(__import__("time").ticks_ms(), t0)
    stop = True
    hung = 0
    for t in tasks:
        try:
            await asyncio.wait_for(t, 3)
        except Exception:  # client stuck (server dropped it) -> counted as an error
            hung += 1
    errs[0] += hung
    await asyncio.sleep_ms(300)  # let the server drop the connections and free their buffers
    gc.collect()
    print("RESULT " + json.dumps({"part": "loopback", "server": "microredis", "cmd": name, "conns": nclients, "P": 1,
                                  "rep": rep, "run_ms": RUN_MS, "ops_per_s": n * 1000 // dt, "errors": sum(errs),
                                  "mem_free": gc.mem_free()}))


async def main():
    srv = MicroRedisServer("127.0.0.1", PORT)
    asyncio.create_task(srv.start())
    await asyncio.sleep_ms(500)
    await functional()
    # preload keys used by the clients
    r, w = await asyncio.open_connection("127.0.0.1", PORT)
    for j in range(16):
        w.write(resp(b"SET", key(j), VAL))
        await w.drain()
        await r.readexactly(5)
    w.close()
    await w.wait_closed()
    await asyncio.sleep_ms(300)
    gc.collect()
    print("heap free before benchmarks:", gc.mem_free())
    for rep in range(REPS):
        await bench("GET", 1, False, rep)
        await bench("GET", 4, False, rep)
        await bench("MIX90", 4, True, rep)
    print("MRLBDONE")


asyncio.run(main())
