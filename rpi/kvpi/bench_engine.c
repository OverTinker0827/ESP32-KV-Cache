/* In-process engine benchmark (no network), the Pi counterpart of kvcache/main/engine_bench.c.
 * 16 B keys "key:%012u", 32 B values, uniform random access. Working sets chosen to sit in
 * L2 (10 k keys), L3 (~30 k), DRAM (1 M, 10 M).
 * Reports Mops/s and ns/op for GET-hit and SET-overwrite, keygen cost subtracted and reported.
 * usage: bench_engine [nkeys ...]   (env OPS=ops per test, BATCH=n for batched-prefetch GETs) */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine.h"

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static inline uint64_t xs(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *s = x;
}
static inline void mkkey(char *k, uint32_t i)
{
    memcpy(k, "key:", 4);
    for (int d = 15; d >= 4; d--) k[d] = '0' + i % 10, i /= 10;
}

int main(int argc, char **argv)
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(3, &cs);
    sched_setaffinity(0, sizeof cs, &cs);
    long ops = getenv("OPS") ? atol(getenv("OPS")) : 20000000;
    int batch = getenv("BATCH") ? atoi(getenv("BATCH")) : 0;
    uint32_t sizes_def[] = {10000, 30000, 1000000, 10000000};
    int ns = argc > 1 ? argc - 1 : 4;
    char val[32];
    memset(val, 'v', sizeof val);
    printf("nkeys,keygen_ns,get_ns,get_Mops,set_ns,set_Mops,bytes_per_key,index_slots\n");
    for (int si = 0; si < ns; si++) {
        uint32_t n = argc > 1 ? (uint32_t)atol(argv[si + 1]) : sizes_def[si];
        kv_t *kv = kv_create((size_t)n * 96 + (64u << 20), n);
        char k[16];
        for (uint32_t i = 0; i < n; i++) {
            mkkey(k, i);
            if (kv_set(kv, k, 16, val, 32, kv_hash(k, 16))) { fprintf(stderr, "load fail at %u\n", i); return 1; }
        }
        uint64_t s = 88172645463325252ull, sink = 0;
        /* keygen + rng only */
        double t0 = now();
        for (long o = 0; o < ops; o++) {
            mkkey(k, (uint32_t)(xs(&s) % n));
            sink += (uint8_t)k[15];
        }
        double kg = (now() - t0) / ops * 1e9;
        /* GET */
        const uint8_t *v;
        size_t vl;
        long hits = 0;
        t0 = now();
        if (batch > 1) {
            char kb[64][16];
            uint64_t hb[64];
            for (long o = 0; o < ops; o += batch) {
                for (int b = 0; b < batch; b++) {
                    mkkey(kb[b], (uint32_t)(xs(&s) % n));
                    hb[b] = kv_hash(kb[b], 16);
                    kv_prefetch(kv, hb[b]);
                }
                for (int b = 0; b < batch; b++) hits += kv_get(kv, kb[b], 16, hb[b], &v, &vl), sink += v[0];
            }
        } else {
            for (long o = 0; o < ops; o++) {
                mkkey(k, (uint32_t)(xs(&s) % n));
                hits += kv_get(kv, k, 16, kv_hash(k, 16), &v, &vl);
                sink += v[0];
            }
        }
        double g = (now() - t0) / ops * 1e9;
        if (hits != ops) { fprintf(stderr, "misses: %ld\n", ops - hits); return 1; }
        /* SET overwrite */
        t0 = now();
        for (long o = 0; o < ops / 2; o++) {
            mkkey(k, (uint32_t)(xs(&s) % n));
            val[0] = (char)o;
            kv_set(kv, k, 16, val, 32, kv_hash(k, 16));
        }
        double st = (now() - t0) / (ops / 2) * 1e9;
        kv_info_t in;
        kv_info(kv, &in);
        double gn = g - kg, sn = st - kg;
        printf("%u,%.1f,%.1f,%.2f,%.1f,%.2f,%.1f,%lu\n", n, kg, gn, 1e3 / gn, sn, 1e3 / sn,
               (double)(in.slabs_bytes + in.index_slots * 8) / n, (unsigned long)in.index_slots);
        fflush(stdout);
        kv_destroy(kv);
        if (sink == 42) puts("");
    }
    return 0;
}
