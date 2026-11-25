# Sanity microbenchmarks: is MicroPython itself this slow here, and why?
import gc
import machine
import time

machine.freq(240_000_000)


def bench(name, f, n):
    gc.collect()
    t = time.ticks_us()
    f(n)
    dt = time.ticks_diff(time.ticks_us(), t)
    print(name, n * 1_000_000 // dt, "ops/s", dt // n, "us/op")


def empty(n):
    for i in range(n):
        pass


def dict_int(n):
    d = {}
    for i in range(n):
        d[i] = i


def mk(n):
    for i in range(n):
        b"key:%012d" % i


print("freq", machine.freq())
bench("empty loop", empty, 20000)
bench("dict int insert", dict_int, 500)
bench("bytes fmt", mk, 500)
ks = [b"key:%012d" % i for i in range(500)]
d = {}
gc.collect()
t = time.ticks_us()
for k in ks:
    d[k] = 1
dt = time.ticks_diff(time.ticks_us(), t)
print("bytes-key insert", 500 * 1_000_000 // dt, "ops/s")
t = time.ticks_us()
for k in ks:
    d.get(k)
dt = time.ticks_diff(time.ticks_us(), t)
print("bytes-key get", 500 * 1_000_000 // dt, "ops/s")
t = time.ticks_us()
gc.collect()
print("gc.collect us", time.ticks_diff(time.ticks_us(), t))
print("free", gc.mem_free(), "alloc", gc.mem_alloc())
