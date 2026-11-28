/* Hardware probe for the Pi 5 (Cortex-A76): numbers the KV design depends on.
 *  1. dependent-load latency vs working-set size (L1 / L2 / L3 / DRAM), 16 K and huge-ish pages
 *  2. core-to-core cache-line round trip (what every cross-core handoff / shared lock costs)
 *  3. atomics: LSE CAS / LDADD uncontended and contended
 *  4. hash throughput: FNV-1a+fmix (ESP engine), CRC32C hardware, on 16 B keys
 *  5. memory bandwidth (memcpy-like stream) single core vs all cores
 * build: gcc -O2 -march=armv8.2-a+crc -pthread hw_probe.c -o hw_probe */
#define _GNU_SOURCE
#include <arm_acle.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void pin(int c)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(c, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

/* ---------------------------------------------------------------- 1. latency */
static void latency(void)
{
    printf("# 1. dependent-load latency (random cycle, 64 B stride)\nsize_KiB,ns_per_load\n");
    size_t sizes[] = {16, 32, 48, 128, 256, 384, 768, 1536, 2048, 3072, 8192, 32768, 131072, 524288};
    for (unsigned si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t bytes = sizes[si] * 1024, n = bytes / 64;
        char *buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        size_t *perm = malloc(n * sizeof *perm);
        for (size_t i = 0; i < n; i++) perm[i] = i;
        srand(1);
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = ((size_t)rand() * 65536u + rand()) % (i + 1), t = perm[i];
            perm[i] = perm[j], perm[j] = t;
        }
        for (size_t i = 0; i < n; i++) *(void **)(buf + perm[i] * 64) = buf + perm[(i + 1) % n] * 64;
        void **p = (void **)(buf + perm[0] * 64);
        size_t iters = 20000000;
        for (size_t i = 0; i < n; i++) p = *p; /* warm */
        double t0 = now();
        for (size_t i = 0; i < iters; i++) p = *p;
        double t1 = now();
        if (!p) puts("");
        printf("%zu,%.2f\n", sizes[si], (t1 - t0) / iters * 1e9);
        munmap(buf, bytes);
        free(perm);
    }
}

/* ---------------------------------------------------------------- 2. ping-pong */
static _Alignas(64) _Atomic uint32_t flag;
static int pp_peer;
static void *pp_thread(void *a)
{
    pin(pp_peer);
    for (uint32_t i = 1; i < 2000000; i += 2) {
        while (atomic_load_explicit(&flag, memory_order_acquire) != i) {
        }
        atomic_store_explicit(&flag, i + 1, memory_order_release);
    }
    return a;
}
static void pingpong(void)
{
    printf("# 2. core-to-core cache-line round trip (ns)\nfrom,to,rtt_ns\n");
    for (int b = 1; b < 4; b++) {
        atomic_store(&flag, 0);
        pp_peer = b;
        pthread_t t;
        pin(0);
        pthread_create(&t, NULL, pp_thread, NULL);
        double t0 = now();
        for (uint32_t i = 0; i < 2000000; i += 2) {
            atomic_store_explicit(&flag, i + 1, memory_order_release);
            while (atomic_load_explicit(&flag, memory_order_acquire) != i + 2) {
            }
        }
        double t1 = now();
        pthread_join(t, NULL);
        printf("0,%d,%.1f\n", b, (t1 - t0) / 1000000 * 1e9);
    }
}

/* ---------------------------------------------------------------- 3. atomics */
static _Alignas(64) _Atomic uint64_t ctr;
static volatile int go;
static int nthr_atom;
static double atom_rate[4];
static void *atom_thread(void *a)
{
    int id = (int)(intptr_t)a;
    pin(id);
    while (!go) {
    }
    double t0 = now();
    for (int i = 0; i < 5000000; i++) atomic_fetch_add_explicit(&ctr, 1, memory_order_relaxed);
    atom_rate[id] = 5000000 / (now() - t0);
    return NULL;
}
static void atomics(void)
{
    printf("# 3. atomic fetch_add on ONE shared line (LSE ldadd)\nthreads,ns_per_op_per_thread,total_Mops\n");
    for (int n = 1; n <= 4; n++) {
        pthread_t t[4];
        go = 0;
        nthr_atom = n;
        for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, atom_thread, (void *)(intptr_t)i);
        go = 1;
        for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
        double tot = 0;
        for (int i = 0; i < n; i++) tot += atom_rate[i];
        printf("%d,%.1f,%.1f\n", n, n / tot * 1e9, tot / 1e6);
    }
    /* uncontended CAS */
    _Atomic uint32_t w = 0;
    double t0 = now();
    for (uint32_t i = 0; i < 20000000; i++) {
        uint32_t e = i;
        atomic_compare_exchange_strong(&w, &e, i + 1);
    }
    printf("uncontended_cas_ns,%.2f\n", (now() - t0) / 20000000 * 1e9);
}

/* ---------------------------------------------------------------- 4. hashing */
static inline uint32_t fnv_fmix(const uint8_t *k, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ k[i]) * 16777619u;
    h ^= h >> 16, h *= 0x85ebca6b, h ^= h >> 13, h *= 0xc2b2ae35, h ^= h >> 16;
    return h;
}
static inline uint32_t crc_hash(const uint8_t *k, size_t n)
{
    uint32_t h = 0xFFFFFFFF;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, k + i, 8);
        h = __crc32cd(h, v);
    }
    for (; i < n; i++) h = __crc32cb(h, k[i]);
    h ^= h >> 16, h *= 0x85ebca6b, h ^= h >> 13; /* crc alone is linear: finalise for bit spread */
    return h;
}
static void hashing(void)
{
    printf("# 4. hash cost on 16 B keys (ns/key)\n");
    uint8_t keys[1024][16];
    for (int i = 0; i < 1024; i++) snprintf((char *)keys[i], 16, "key:%011d", i * 7919);
    volatile uint32_t sink = 0;
    double t0 = now();
    for (int r = 0; r < 20000; r++)
        for (int i = 0; i < 1024; i++) sink += fnv_fmix(keys[i], 16);
    double t1 = now();
    for (int r = 0; r < 20000; r++)
        for (int i = 0; i < 1024; i++) sink += crc_hash(keys[i], 16);
    double t2 = now();
    printf("fnv1a_fmix,%.2f\ncrc32c_hw,%.2f\n", (t1 - t0) / 20480000 * 1e9, (t2 - t1) / 20480000 * 1e9);
}

/* ---------------------------------------------------------------- 5. bandwidth */
#define BW_BYTES (256u << 20)
static char *bw_src[4], *bw_dst[4];
static double bw_res[4];
static void *bw_thread(void *a)
{
    int id = (int)(intptr_t)a;
    pin(id);
    while (!go) {
    }
    double t0 = now();
    for (int r = 0; r < 4; r++) memcpy(bw_dst[id], bw_src[id], BW_BYTES / 4);
    bw_res[id] = 4.0 * (BW_BYTES / 4) * 2 / (now() - t0) / 1e9; /* read+write GB/s */
    return NULL;
}
static void bandwidth(void)
{
    printf("# 5. memcpy bandwidth, 64 MiB per thread (GB/s read+write)\nthreads,total_GBps\n");
    for (int i = 0; i < 4; i++) {
        bw_src[i] = malloc(BW_BYTES / 4), bw_dst[i] = malloc(BW_BYTES / 4);
        memset(bw_src[i], i, BW_BYTES / 4), memset(bw_dst[i], 0, BW_BYTES / 4);
    }
    for (int n = 1; n <= 4; n++) {
        pthread_t t[4];
        go = 0;
        for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, bw_thread, (void *)(intptr_t)i);
        go = 1;
        for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
        double tot = 0;
        for (int i = 0; i < n; i++) tot += bw_res[i];
        printf("%d,%.2f\n", n, tot);
    }
}

int main(int argc, char **argv)
{
    pin(0);
    const char *only = argc > 1 ? argv[1] : "12345";
    if (strchr(only, '1')) latency();
    if (strchr(only, '2')) pingpong();
    if (strchr(only, '3')) atomics();
    if (strchr(only, '4')) hashing();
    if (strchr(only, '5')) bandwidth();
    return 0;
}
