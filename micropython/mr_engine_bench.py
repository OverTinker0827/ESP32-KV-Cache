# On-device microredis storage-engine benchmark (no network in the loop).
# Measures pure engine cost: SET/GET/DEL ops/s, bytes per key, max keys.
# Prints one line "RESULT {json}" for the host to parse.
import gc
import json
import machine
import time

machine.freq(240_000_000)

KEY_LEN = 16
VAL_LEN = 32
N = 500  # 2000 keys + key list -> MemoryError (dict resize needs 13 KB contiguous)

gc.collect()
heap0 = gc.mem_free()
from microredis.storage.engine import Storage  # noqa: E402

gc.collect()


def mk_key(i):
    return b"key:%012d" % i  # 16 bytes


VAL = b"v" * VAL_LEN
res = {"impl": "microredis", "key_len": KEY_LEN, "val_len": VAL_LEN, "freq": machine.freq()}

s = Storage()
keys = [mk_key(i) for i in range(N)]
gc.collect()

# Reference floor: a bare MicroPython dict doing the same work
d = {}
t = time.ticks_us()
for k in keys:
    d[k] = VAL
res["raw_dict_set_ops"] = N * 1_000_000 // time.ticks_diff(time.ticks_us(), t)
t = time.ticks_us()
for k in keys:
    d.get(k)
res["raw_dict_get_ops"] = N * 1_000_000 // time.ticks_diff(time.ticks_us(), t)
del d
gc.collect()

# SET (insert)
t = time.ticks_us()
for k in keys:
    s.set(k, VAL)
dt = time.ticks_diff(time.ticks_us(), t)
res["set_ops"] = N * 1_000_000 // dt

# GET (hit)
t = time.ticks_us()
for k in keys:
    s.get(k)
dt = time.ticks_diff(time.ticks_us(), t)
res["get_ops"] = N * 1_000_000 // dt

# DEL
t = time.ticks_us()
for k in keys:
    s.delete(k)
dt = time.ticks_diff(time.ticks_us(), t)
res["del_ops"] = N * 1_000_000 // dt

# Memory per key: fill with fresh key/value objects (as a network SET would allocate)
del keys
s = Storage()
gc.collect()
alloc_a = gc.mem_alloc()
M = 500
for i in range(M):
    s.set(mk_key(i), bytes(VAL))
    s.get(mk_key(i))  # GET also populates _last_access, as real traffic would
gc.collect()
res["bytes_per_key"] = (gc.mem_alloc() - alloc_a) // M

# Max entries until MemoryError
i = M
try:
    while True:
        s.set(mk_key(i), bytes(VAL))
        s.get(mk_key(i))
        i += 1
except MemoryError:
    pass
res["max_keys"] = i
s = None
gc.collect()
res["heap_total"] = gc.mem_free() + gc.mem_alloc()
print("RESULT " + json.dumps(res))
