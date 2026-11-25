# On-device microredis storage-engine benchmark (no network), v2.
# Mirrors kvcache/main/engine_bench.c: 16 B keys (sequential "key:%012d" and random hex),
# 32 B values, N = 500 keys. Every op is timed (ticks_us) -> p50/p95/p99.
# Then fill until MemoryError for max keys. Prints "RESULT {json}" lines.
import binascii
import gc
import json
import machine
import os
import time

machine.freq(240_000_000)

KEY_LEN = 16
VAL_LEN = 32
N = 500  # 2000 keys + key list -> MemoryError (dict resize needs 13 KB contiguous)
REPS = 2

from microredis.storage.engine import Storage  # noqa: E402

VAL = b"v" * VAL_LEN


def mk_key(i):
    return b"key:%012d" % i  # 16 bytes


def pct(samples):
    samples.sort()
    n = len(samples)
    return [samples[n // 2] * 1000, samples[n * 95 // 100] * 1000, samples[n * 99 // 100] * 1000]  # ns


def suite(keys):
    s = Storage()
    gc.collect()
    tot = [0, 0, 0]
    sm = [[], [], []]
    tu, td = time.ticks_us, time.ticks_diff
    for _ in range(REPS):
        t0 = tu()
        for k in keys:
            a = tu()
            s.set(k, VAL)
            sm[0].append(td(tu(), a))
        t1 = tu()
        for k in keys:
            a = tu()
            s.get(k)
            sm[1].append(td(tu(), a))
        t2 = tu()
        for k in keys:
            a = tu()
            s.delete(k)
            sm[2].append(td(tu(), a))
        t3 = tu()
        tot[0] += td(t1, t0)
        tot[1] += td(t2, t1)
        tot[2] += td(t3, t2)
    ops = [N * REPS * 1_000_000 // t for t in tot]
    return ops, [pct(x) for x in sm]


def emit(res):
    print("RESULT " + json.dumps(res))


base = {"impl": "microredis", "key_len": KEY_LEN, "val_len": VAL_LEN, "freq": machine.freq()}

for name, keys in (
    ("seq", [mk_key(i) for i in range(N)]),
    ("rand", [binascii.hexlify(os.urandom(8)) for _ in range(N)]),
):
    gc.collect()
    ops, p = suite(keys)
    r = dict(base)
    r.update({"scenario": "quiet", "keys": name, "set_ops": ops[0], "get_ops": ops[1], "del_ops": ops[2]})
    for i, op in enumerate(("set", "get", "del")):
        r[op + "_p50_ns"], r[op + "_p95_ns"], r[op + "_p99_ns"] = p[i]
    emit(r)
    del keys
    gc.collect()

# Max entries until MemoryError (sequential keys, fresh objects as a network SET would allocate)
s = Storage()
gc.collect()
alloc_a = gc.mem_alloc()
i = 0
try:
    while True:
        s.set(mk_key(i), bytes(VAL))
        s.get(mk_key(i))
        i += 1
except MemoryError:
    pass
r = dict(base)
r.update({"scenario": "fill", "max_keys": i, "bytes_per_key_alloc": (gc.mem_alloc() - alloc_a) // max(i, 1)})
s = None
gc.collect()
r["heap_total"] = gc.mem_free() + gc.mem_alloc()
emit(r)
