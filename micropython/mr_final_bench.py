# Entry 7: microredis storage engine, same workload as the C harness's final comparison:
# 1024 random 16 B hex keys, 32 B values, 90/10 and 50/50 GET/SET over a 4096-op stream,
# 5 repetitions x 3 s each, then one checked run per workload (every GET must return the value
# last SET for that key; all keys checked at the end). Prints "RESULT {json}" lines.
import binascii
import gc
import json
import machine
import os
import time

machine.freq(240_000_000)
from microredis.storage.engine import Storage  # noqa: E402

K = 1024
STREAM = 4096
REPS = 5
RUN_MS = 3000

keys = [binascii.hexlify(os.urandom(8)) for _ in range(K)]
VAL = b"v" * 32


def stream_for(get_pct):
    s = []
    for _ in range(STREAM):
        r = int.from_bytes(os.urandom(4), "little")
        s.append((r % K) | (0 if (r >> 16) % 100 < get_pct else 0x8000))
    return s


def run(st, ops, run_ms):
    n = 0
    i = 0
    t_end = time.ticks_add(time.ticks_ms(), run_ms)
    t0 = time.ticks_us()
    get, put = st.get, st.set
    while time.ticks_diff(t_end, time.ticks_ms()) > 0:
        for _ in range(256):  # check the clock every 256 ops
            op = ops[i & (STREAM - 1)]
            i += 1
            if op & 0x8000:
                put(keys[op & 0x7FFF], VAL)
            else:
                get(keys[op & 0x7FFF])
        n += 256
    dt = time.ticks_diff(time.ticks_us(), t0)
    return n * 1_000_000 // dt


KC = 512  # microredis runs out of heap preloading 1024 keys with distinct 32 B values


def checked(st, ops, run_ms):
    ver = [0] * KC  # expected value is rebuilt from the version (saves heap)
    for k in range(KC):
        st.set(keys[k], b"v%031d" % 0)
    bad = 0
    gets = sets = 0
    i = 0
    t_end = time.ticks_add(time.ticks_ms(), run_ms)
    while time.ticks_diff(t_end, time.ticks_ms()) > 0:
        op = ops[i & (STREAM - 1)]
        i += 1
        k = (op & 0x7FFF) % KC
        if op & 0x8000:
            ver[k] += 1
            st.set(keys[k], b"v%031d" % ver[k])
            sets += 1
        else:
            if st.get(keys[k]) != b"v%031d" % ver[k]:
                bad += 1
            gets += 1
    final_bad = sum(1 for k in range(KC) if st.get(keys[k]) != b"v%031d" % ver[k])
    return gets, sets, bad, final_bad


CHECK_ONLY = False
for name, gp in (() if CHECK_ONLY else (("g90_uniform", 90), ("g50_uniform", 50))):
    ops = stream_for(gp)
    for rep in range(REPS):
        st = Storage()
        for k in keys:
            st.set(k, VAL)
        gc.collect()
        ops_s = run(st, ops, RUN_MS)
        print("RESULT " + json.dumps({"part": "final", "design": "microredis", "workload": name, "rep": rep,
                                      "run_ms": RUN_MS, "ops_per_s": ops_s, "freq": machine.freq()}))
        st = None
        gc.collect()
for name, gp in (("g90_uniform", 90), ("g50_uniform", 50)):
    ops = None
    gc.collect()
    ops = stream_for(gp)
    gc.collect()
    st = Storage()
    g, s, bad, fb = checked(st, ops, RUN_MS)
    print("RESULT " + json.dumps({"part": "final_check", "design": "microredis", "workload": name, "run_ms": RUN_MS, "keys": KC,
                                  "gets_checked": g, "sets": s, "wrong_value": bad, "final_mismatch": fb,
                                  "pass": int(bad == 0 and fb == 0)}))
    st = None
    gc.collect()
print("MRDONE")
