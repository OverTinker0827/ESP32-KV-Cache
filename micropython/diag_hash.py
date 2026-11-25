# Confirm: sequential keys cluster in MicroPython's dict (djb2-like hash + linear probing)
import gc
import machine
import os
import time
import binascii

machine.freq(240_000_000)
N = 500


def run(name, ks):
    d = {}
    gc.collect()
    t = time.ticks_us()
    for k in ks:
        d[k] = 1
    ins = N * 1_000_000 // time.ticks_diff(time.ticks_us(), t)
    t = time.ticks_us()
    for k in ks:
        d.get(k)
    get = N * 1_000_000 // time.ticks_diff(time.ticks_us(), t)
    # count distinct low 10 bits of hash (bucket spread for a 1024-slot table)
    spread = len(set(hash(k) & 1023 for k in ks))
    print(name, "insert", ins, "get", get, "distinct low-10-bit hashes", spread, "/", N)


run("sequential key:%012d", [b"key:%012d" % i for i in range(N)])
run("random 16B hex     ", [binascii.hexlify(os.urandom(8)) for _ in range(N)])
run("sequential str     ", ["key:%012d" % i for i in range(N)])
