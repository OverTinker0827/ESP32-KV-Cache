/* Shared store (64 B-aligned seqlock stripes over engine instances) and the RESP command table,
 * used by the multi-core servers (epoll: server_mt.c, io_uring: server_uring.c). See server_mt.c header. */
#pragma once
#include "proto.h"
#include <stdatomic.h>
#include <stdint.h>
#include "engine.h"

typedef struct {
    _Alignas(64) _Atomic uint32_t seq;
    kv_t *kv; /* read-only after start; shares the line with seq (it never changes, so no ping-pong) */
} stripe_t; /* aligned to 64 B: neighbouring stripes never share a line */

static stripe_t *stripes;
static uint32_t nstripes;

typedef struct {
    _Alignas(128) uint64_t rd_retries, wr_spins, reqs;
} wstat_t;
static wstat_t wstats[16];
static _Thread_local wstat_t *ws;

static inline stripe_t *stripe_of(uint64_t h) { return &stripes[(h >> 32) & (nstripes - 1)]; }

static inline void wr_lock(stripe_t *st)
{
    for (;;) {
        uint32_t s = atomic_load_explicit(&st->seq, memory_order_relaxed);
        if (!(s & 1) && atomic_compare_exchange_weak_explicit(&st->seq, &s, s + 1, memory_order_acquire,
                                                              memory_order_relaxed))
            break;
        ws->wr_spins++;
        __asm__ volatile("yield");
    }
    atomic_thread_fence(memory_order_release); /* odd seq visible before the data writes */
}
static inline void wr_unlock(stripe_t *st)
{
    atomic_store_explicit(&st->seq, atomic_load_explicit(&st->seq, memory_order_relaxed) + 1,
                          memory_order_release);
}

/* GET: optimistic read straight into the reply buffer */
static void do_get_h(conn_t *c, const char *k, size_t kl, uint64_t h, int as_exists, long *nfound)
{
    stripe_t *st = stripe_of(h);
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&st->seq, memory_order_acquire);
        if (s1 & 1) {
            __asm__ volatile("yield");
            continue;
        }
        size_t save = c->wlen;
        const uint8_t *v;
        size_t vl;
        int r = kv_get_rd(st->kv, k, kl, h, &v, &vl);
        if (r == 1 && !as_exists) {
            if (vl > (64u << 20)) r = -1; /* torn length: never reserve on it */
            else wbulk(c, v, vl);
        }
        atomic_thread_fence(memory_order_acquire);
        if (r >= 0 && atomic_load_explicit(&st->seq, memory_order_relaxed) == s1) {
            if (as_exists) *nfound += r;
            else if (r == 0) WLIT(c, "$-1\r\n");
            return;
        }
        c->wlen = save;
        ws->rd_retries++;
    }
}

static void do_get(conn_t *c, const char *k, size_t kl, int as_exists, long *nfound)
{
    do_get_h(c, k, kl, kv_hash(k, kl), as_exists, nfound);
}

static int do_set_h(const char *k, size_t kl, const char *v, size_t vl, uint64_t h)
{
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    int r = kv_set(st->kv, k, kl, v, vl, h);
    wr_unlock(st);
    return r;
}

static int do_set(const char *k, size_t kl, const char *v, size_t vl)
{
    return do_set_h(k, kl, v, vl, kv_hash(k, kl));
}

static int do_del(const char *k, size_t kl)
{
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    int r = kv_del(st->kv, k, kl, h);
    wr_unlock(st);
    return r;
}

static void exec_cmd(conn_t *c, int argc, char **argv, size_t *argl)
{
    if (argc == 0) return;
    ws->reqs++;
#if KV_FAST
    uint64_t w = cmdword(argv[0], argl[0]);
    if (w == CW("get") && argc == 2) {
        do_get(c, argv[1], argl[1], 0, NULL);
        return;
    }
    if (w == CW("set") && argc >= 3) {
        if (do_set(argv[1], argl[1], argv[2], argl[2]) == 0) WLIT(c, "+OK\r\n");
        else WLIT(c, "-OOM command not allowed when used memory > 'maxmemory'\r\n");
        return;
    }
#endif
    if (IS(0, "GET") && argc == 2) {
        do_get(c, argv[1], argl[1], 0, NULL);
    } else if (IS(0, "SET") && argc >= 3) {
        if (do_set(argv[1], argl[1], argv[2], argl[2]) == 0) WLIT(c, "+OK\r\n");
        else WLIT(c, "-OOM command not allowed when used memory > 'maxmemory'\r\n");
    } else if (IS(0, "DEL") || IS(0, "UNLINK")) {
        long n = 0;
        for (int i = 1; i < argc; i++) n += do_del(argv[i], argl[i]);
        wint(c, ':', n);
    } else if (IS(0, "EXISTS")) {
        long n = 0;
        for (int i = 1; i < argc; i++) do_get(c, argv[i], argl[i], 1, &n);
        wint(c, ':', n);
    } else if (IS(0, "MGET")) {
        wint(c, '*', argc - 1);
        for (int i = 1; i < argc; i++) do_get(c, argv[i], argl[i], 0, NULL);
    } else if (IS(0, "MSET") && argc % 2 == 1) {
        for (int i = 1; i < argc; i += 2) do_set(argv[i], argl[i], argv[i + 1], argl[i + 1]);
        WLIT(c, "+OK\r\n");
    } else if (IS(0, "PING")) {
        if (argc > 1) wbulk(c, argv[1], argl[1]);
        else WLIT(c, "+PONG\r\n");
    } else if (IS(0, "ECHO") && argc == 2) {
        wbulk(c, argv[1], argl[1]);
    } else if (IS(0, "DBSIZE")) {
        uint64_t n = 0;
        for (uint32_t i = 0; i < nstripes; i++) {
            kv_info_t in;
            wr_lock(&stripes[i]);
            kv_info(stripes[i].kv, &in);
            wr_unlock(&stripes[i]);
            n += in.count;
        }
        wint(c, ':', (long long)n);
    } else if (IS(0, "FLUSHALL") || IS(0, "FLUSHDB")) {
        for (uint32_t i = 0; i < nstripes; i++) {
            wr_lock(&stripes[i]);
            kv_flush(stripes[i].kv);
            wr_unlock(&stripes[i]);
        }
        WLIT(c, "+OK\r\n");
    } else if (IS(0, "CONFIG") || IS(0, "COMMAND") || IS(0, "CLIENT")) {
        WLIT(c, "*0\r\n");
    } else if (IS(0, "INFO")) {
        uint64_t rr = 0, sp = 0, rq = 0;
        for (int i = 0; i < 16; i++) rr += wstats[i].rd_retries, sp += wstats[i].wr_spins, rq += wstats[i].reqs;
        char b[256];
        int n = snprintf(b, sizeof b, "# kvpi-mt\r\nstripes:%u\r\nrequests:%lu\r\nread_retries:%lu\r\nwrite_spins:%lu\r\n",
                         nstripes, (unsigned long)rq, (unsigned long)rr, (unsigned long)sp);
        wbulk(c, b, n);
    } else if (IS(0, "QUIT")) {
        WLIT(c, "+OK\r\n");
        c->closing = 1;
    } else {
        WLIT(c, "-ERR unknown command\r\n");
    }
}


#if KV_BATCH
/* v3 batched execution (MICA-style memory-level parallelism): parse every complete command in the
 * receive buffer first, hash every key and prefetch the stripe word + home index slot, then (second
 * pass) read each home slot and prefetch the entry it points to, then execute in order. Up to
 * BATCH_MAX commands overlap their DRAM misses instead of paying ~115 ns each in sequence. */
#define BATCH_MAX 64
typedef struct {
    int argc;
    char *argv[3];
    size_t argl[3];
    uint64_t h;
    int kind; /* 1 GET, 2 SET, 0 other (run through exec_cmd) */
} bcmd_t;

static inline void run_bcmd(conn_t *c, bcmd_t *x)
{
    ws->reqs++;
    if (x->kind == 1) do_get_h(c, x->argv[1], x->argl[1], x->h, 0, NULL);
    else if (do_set_h(x->argv[1], x->argl[1], x->argv[2], x->argl[2], x->h) == 0) WLIT(c, "+OK\r\n");
    else WLIT(c, "-OOM command not allowed when used memory > 'maxmemory'\r\n");
}

static int exec_batch(conn_t *c)
{
    bcmd_t b[BATCH_MAX];
    int n = 0, r = 1;
    char *argv[MAXARGS];
    size_t argl[MAXARGS];
    int argc;
    for (;;) {
        n = 0;
        while (n < BATCH_MAX && (r = parse(c, &argc, argv, argl)) == 1) {
            bcmd_t *x = &b[n];
            uint64_t w = argc ? cmdword(argv[0], argl[0]) : 0;
            x->kind = (w == CW("get") && argc == 2) ? 1 : (w == CW("set") && argc == 3) ? 2 : 0;
            if (!x->kind) { /* other command: flush the batch so far, then run it alone (keeps order) */
                for (int i = 0; i < n; i++) run_bcmd(c, &b[i]);
                n = 0;
                exec_cmd(c, argc, argv, argl);
                continue;
            }
            x->argc = argc;
            for (int i = 0; i < argc; i++) x->argv[i] = argv[i], x->argl[i] = argl[i];
            x->h = kv_hash(argv[1], argl[1]);
            stripe_t *st = stripe_of(x->h);
            __builtin_prefetch(st);
            kv_prefetch(st->kv, x->h);
            n++;
        }
        for (int i = 0; i < n; i++) kv_prefetch_entry(stripe_of(b[i].h)->kv, b[i].h);
        for (int i = 0; i < n; i++) run_bcmd(c, &b[i]);
        if (r != 1) return r; /* 0 incomplete, -1 protocol error */
    }
}
#endif
