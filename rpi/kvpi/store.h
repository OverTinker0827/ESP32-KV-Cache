/* Shared store (64 B-aligned seqlock stripes over engine instances) and the RESP command table,
 * used by the multi-core servers. v5: expiry, memory limit + LRU eviction, string commands,
 * lists / hashes / sets / sorted sets (objects.c). frozen_v4/store.h is the measured v3/v4 version.
 *
 * Concurrency:
 *  - GET / MGET / EXISTS / TTL / PTTL / STRLEN / TYPE on strings: optimistic seqlock reads (no shared
 *    line written, except the once-a-second LRU stamp in the index slot, done by CAS).
 *  - everything that writes, and every command on an object value: the stripe's writer lock.
 *    Objects are heap structures; optimistic readers must never follow their pointers. */
#pragma once
#include "proto.h"
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include "engine.h"
#include "objects.h"

typedef struct {
    _Alignas(64) _Atomic uint32_t seq;
    kv_t *kv;
} stripe_t;

static stripe_t *stripes;
static uint32_t nstripes;

typedef struct {
    _Alignas(128) uint64_t rd_retries, wr_spins, reqs;
    uint64_t sweep_ms;
    uint32_t sweep_next;
} wstat_t;
static wstat_t wstats[16];
static _Thread_local wstat_t *ws;
static int nworkers = 1;

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
    atomic_thread_fence(memory_order_release);
}
static inline void wr_unlock(stripe_t *st)
{
    atomic_store_explicit(&st->seq, atomic_load_explicit(&st->seq, memory_order_relaxed) + 1, memory_order_release);
}

/* ---------------------------------------------------------------- QSBR (quiescent-state reclamation)
 * Memory that optimistic readers on other workers may still be using (an index table replaced by a
 * grow, later: large-value blobs) is retired, not freed. A worker announces a quiescent state once per
 * event-loop pass, when it holds no pointers into shared data; it goes offline (0) while it sleeps in
 * io_uring. A retired item (stamped with the global epoch at retirement) is freed by the worker that
 * retired it once every online worker has announced an epoch newer than that stamp. */
typedef struct {
    _Alignas(64) _Atomic uint64_t seen; /* epoch at the last quiescent point; 0 = offline */
} qs_slot_t;
static qs_slot_t qs_slots[16];
static _Atomic uint64_t qs_epoch = 1;
typedef struct {
    void *p;
    void (*fn)(void *);
    uint64_t epoch;
} qs_item_t;
static _Thread_local qs_item_t *qs_list;
static _Thread_local size_t qs_n, qs_cap;
static _Thread_local int qs_wid = -1;

static void qs_retire(void *p, void (*fn)(void *))
{
    if (qs_wid < 0) return fn(p); /* not a worker (setup / single thread): nobody else can hold it */
    if (qs_n == qs_cap) qs_list = realloc(qs_list, (qs_cap = qs_cap ? qs_cap * 2 : 64) * sizeof *qs_list);
    qs_list[qs_n++] = (qs_item_t){p, fn, atomic_fetch_add(&qs_epoch, 1)};
}
static void qs_reclaim(void)
{
    if (!qs_n) return;
    uint64_t min = UINT64_MAX;
    for (int i = 0; i < nworkers; i++) {
        uint64_t s = atomic_load_explicit(&qs_slots[i].seen, memory_order_acquire);
        if (s && s < min) min = s;
    }
    size_t k = 0;
    for (size_t i = 0; i < qs_n; i++) {
        if (qs_list[i].epoch < min) qs_list[i].fn(qs_list[i].p); /* everyone announced after the retire */
        else qs_list[k++] = qs_list[i];
    }
    qs_n = k;
}
static inline void qs_quiescent(int wid)
{
    atomic_store_explicit(&qs_slots[wid].seen, atomic_load_explicit(&qs_epoch, memory_order_acquire),
                          memory_order_seq_cst);
    qs_reclaim();
}
static inline void qs_offline(int wid) { atomic_store_explicit(&qs_slots[wid].seen, 0, memory_order_seq_cst); }

/* ---------------------------------------------------------------- time, sweep, setup */
static inline uint64_t mono_ms(void)
{
    struct timespec t; /* not _COARSE: that ticks every 4 ms here (HZ=250) and kept 1 ms TTLs alive */
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
/* once per event-loop pass: refresh the engine clock (written only when it changed) and, once per
 * millisecond, sweep 64 index slots of one stripe for expired keys (stripes taken round-robin by
 * the workers) */
static void store_tick(int wid, int idle)
{
    uint64_t now = mono_ms();
    if (now != kv_now_ms()) kv_clock_set(now);
    if (idle) { /* woke from an idle wait: sweep one whole stripe, 8192 slots (~4 us) per lock hold so a
                   busy worker on the same stripe never waits long */
        stripe_t *st = &stripes[(ws->sweep_next++ * nworkers + wid) & (nstripes - 1)];
        kv_info_t in;
        kv_info(st->kv, &in); /* index_slots only changes under the lock; a stale value just ends early */
        for (uint64_t done = 0; done < in.index_slots; done += 8192) {
            wr_lock(st);
            kv_expire_step(st->kv, 8192);
            wr_unlock(st);
        }
        return;
    }
    if (now == ws->sweep_ms) return;
    ws->sweep_ms = now;
    stripe_t *st = &stripes[(ws->sweep_next * nworkers + wid) & (nstripes - 1)];
    ws->sweep_next++;
    wr_lock(st);
    /* adaptive, like Redis's expire cycle: keep going while a quarter or more of what we scan is dead */
    /* 4096 slots: slots without the ttl bit are skipped without touching memory beyond the index */
    for (int round = 0; round < 4 && kv_expire_step(st->kv, 4096) >= 64; round++) {
    }
    wr_unlock(st);
}
static int store_init(uint32_t ns, size_t arena_mb, size_t keys, size_t maxmem_mb, int policy, int nw)
{
    nstripes = ns;
    nworkers = nw;
    kv_set_obj_free(obj_free);
    kv_set_retire(qs_retire);
    kv_clock_set(mono_ms());
    stripes = aligned_alloc(64, sizeof(stripe_t) * nstripes);
    memset(stripes, 0, sizeof(stripe_t) * nstripes);
    for (uint32_t i = 0; i < nstripes; i++) {
        stripes[i].kv = kv_create(arena_mb << 20, keys / nstripes + 1);
        if (!stripes[i].kv) return -1;
        kv_set_shared(stripes[i].kv, 1);
        if (maxmem_mb) kv_set_limit(stripes[i].kv, (maxmem_mb << 20) / nstripes, policy);
    }
    return 0;
}

/* ---------------------------------------------------------------- reply helpers */
#define ERR_TYPE "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"
#define ERR_INT "-ERR value is not an integer or out of range\r\n"
#define ERR_SYNTAX "-ERR syntax error\r\n"
#define ERR_OOM "-OOM command not allowed when used memory > 'maxmemory'.\r\n"
#define ERR_ARGS "-ERR wrong number of arguments\r\n"
#define ERR_FLOAT "-ERR value is not a valid float\r\n"
static void wdouble(conn_t *c, double d)
{
    char b[40];
    int n = snprintf(b, sizeof b, "%.17g", d);
    wbulk(c, b, n);
}
static int parse_ll(const char *s, size_t n, long long *out)
{
    /* same rules as Redis string2ll: optional '-', no '+', no leading zeros ("07" is not an integer) */
    if (n == 0 || n > 20) return 0;
    if (n == 1 && s[0] == '0') return *out = 0, 1;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') neg = 1, i = 1;
    if (i == n || s[i] == '0') return 0;
    unsigned long long v = 0;
    for (; i < n; i++) {
        if ((unsigned)(s[i] - '0') > 9) return 0;
        unsigned long long nv = v * 10 + (s[i] - '0');
        if (nv / 10 != v) return 0;
        v = nv;
    }
    if (neg ? v > 9223372036854775808ull : v > 9223372036854775807ull) return 0;
    *out = neg ? -(long long)v : (long long)v;
    return 1;
}
static int parse_dbl(const char *s, size_t n, double *out)
{
    char b[64];
    if (n == 0 || n >= sizeof b) return 0;
    memcpy(b, s, n), b[n] = 0;
    if (!strcasecmp(b, "+inf") || !strcasecmp(b, "inf")) return *out = INFINITY, 1;
    if (!strcasecmp(b, "-inf")) return *out = -INFINITY, 1;
    char *e;
    *out = strtod(b, &e);
    return *e == 0 && !isnan(*out);
}

/* ---------------------------------------------------------------- optimistic string reads */
enum { RD_GET, RD_EXISTS, RD_TTL, RD_PTTL, RD_STRLEN, RD_TYPE, RD_MGET };
/* returns 1 if the reader handled it, 0 if the key holds an object and the caller must lock */
static int rd_op(conn_t *c, const char *k, size_t kl, uint64_t h, int op, long *acc)
{
    stripe_t *st = stripe_of(h);
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&st->seq, memory_order_acquire);
        if (s1 & 1) {
            __asm__ volatile("yield");
            continue;
        }
        size_t save = c->wlen;
        kv_val_t v;
        int r = kv_get_rd(st->kv, k, kl, h, &v);
        if (r == 1 && (op == RD_GET || op == RD_MGET) && v.type == KV_T_STR && !v.blob) {
            if (v.vl > (64u << 20)) r = -1;
            else wbulk(c, v.v, v.vl);
        }
        atomic_thread_fence(memory_order_acquire);
        if (r >= 0 && atomic_load_explicit(&st->seq, memory_order_relaxed) == s1) {
            /* a blob pointer read under a valid seqlock was the key's value at that moment; QSBR keeps the
             * memory valid for this loop pass, and a reference (if we can still get one) keeps it for the send */
            if (r == 1 && v.blob && (op == RD_GET || op == RD_MGET)) {
                if (!kv_blob_try_ref(v.blob)) { /* replaced just now: read the key again */
                    ws->rd_retries++;
                    continue;
                }
                if (c->nseg < 256) wblob(c, v.blob, v.blob->d, v.blob->len);
                else { /* very deep pipeline of large values: copy instead of growing the iovec list */
                    wbulk(c, v.blob->d, v.blob->len);
                    kv_blob_unref(v.blob);
                }
                return 1;
            }
            if (r == 1 && v.blob) v.vl = v.blob->len;
            switch (op) {
            case RD_GET:
                if (r == 0) WLIT(c, "$-1\r\n");
                else if (v.type != KV_T_STR) WLIT(c, ERR_TYPE);
                break;
            case RD_MGET: /* Redis: MGET answers nil for keys that hold a non-string */
                if (r == 0 || v.type != KV_T_STR) WLIT(c, "$-1\r\n");
                break;
            case RD_EXISTS: *acc += r; break;
            case RD_TTL:
            case RD_PTTL: {
                long long t = -2;
                if (r == 1) {
                    if (!v.expire_ms) t = -1;
                    else {
                        long long ms = (long long)v.expire_ms - (long long)kv_now_ms();
                        if (ms < 0) ms = 0;
                        t = op == RD_TTL ? (ms + 500) / 1000 : ms;
                    }
                }
                wint(c, ':', t);
                break;
            }
            case RD_STRLEN:
                if (r == 1 && v.type != KV_T_STR) WLIT(c, ERR_TYPE);
                else wint(c, ':', r == 1 ? (long long)v.vl : 0);
                break;
            case RD_TYPE:
                if (r == 0) WLIT(c, "+none\r\n");
                else if (v.type == KV_T_STR) WLIT(c, "+string\r\n");
                else return 0; /* object: the type is inside the object, read it under the lock */
                break;
            }
            return 1;
        }
        c->wlen = save;
        ws->rd_retries++;
    }
}
static void do_get_h(conn_t *c, const char *k, size_t kl, uint64_t h, int as_exists, long *nfound)
{
    rd_op(c, k, kl, h, as_exists ? RD_EXISTS : RD_GET, nfound);
}
static void do_get(conn_t *c, const char *k, size_t kl, int as_exists, long *nfound)
{
    do_get_h(c, k, kl, kv_hash(k, kl), as_exists, nfound);
}

/* ---------------------------------------------------------------- locked string writes */
static int do_put_h(const char *k, size_t kl, const char *v, size_t vl, uint64_t h, uint64_t exp, int flags)
{
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    int r = kv_put(st->kv, k, kl, v, vl, h, exp, KV_T_STR, flags);
    wr_unlock(st);
    return r;
}
static int do_set_h(const char *k, size_t kl, const char *v, size_t vl, uint64_t h)
{
    return do_put_h(k, kl, v, vl, h, 0, 0) < 0 ? -1 : 0;
}
static int do_set(const char *k, size_t kl, const char *v, size_t vl) { return do_set_h(k, kl, v, vl, kv_hash(k, kl)); }
static int do_del(const char *k, size_t kl)
{
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    int r = kv_del(st->kv, k, kl, h);
    wr_unlock(st);
    return r;
}

/* SET key value [NX|XX] [EX s|PX ms|EXAT s|PXAT ms|KEEPTTL] [GET] */
static void cmd_set(conn_t *c, int argc, char **argv, size_t *argl)
{
    int flags = 0, get = 0;
    uint64_t exp = 0;
    for (int i = 3; i < argc; i++) {
        uint64_t w = cmdword(argv[i], argl[i]);
        long long n;
        if (w == CW("nx")) flags |= KV_NX;
        else if (w == CW("xx")) flags |= KV_XX;
        else if (w == CW("keepttl")) flags |= KV_KEEPTTL;
        else if (w == CW("get")) get = 1;
        else if ((w == CW("ex") || w == CW("px") || w == CW("exat") || w == CW("pxat")) && i + 1 < argc) {
            if (!parse_ll(argv[i + 1], argl[i + 1], &n) || n <= 0) return (void)WLIT(c, "-ERR invalid expire time in 'set' command\r\n");
            uint64_t now = kv_now_ms();
            exp = w == CW("ex") ? now + (uint64_t)n * 1000 : w == CW("px") ? now + (uint64_t)n
                : w == CW("exat") ? (uint64_t)n * 1000 : (uint64_t)n;
            if (w == CW("exat") || w == CW("pxat")) { /* absolute wall-clock -> our monotonic clock */
                struct timespec rt;
                clock_gettime(CLOCK_REALTIME, &rt);
                long long wall = (long long)rt.tv_sec * 1000 + rt.tv_nsec / 1000000;
                long long d = (long long)exp - wall;
                exp = d <= 0 ? 1 : now + (uint64_t)d;
            }
            i++;
        } else return (void)WLIT(c, ERR_SYNTAX);
    }
    if ((flags & KV_NX) && (flags & KV_XX)) return (void)WLIT(c, ERR_SYNTAX);
    uint64_t h = kv_hash(argv[1], argl[1]);
    if (get) { /* SET ... GET: old value first (locked, simple) */
        stripe_t *st = stripe_of(h);
        wr_lock(st);
        kv_val_t old;
        int had = kv_lookup(st->kv, argv[1], argl[1], h, &old);
        if (had && old.type != KV_T_STR) {
            wr_unlock(st);
            return (void)WLIT(c, ERR_TYPE);
        }
        size_t save = c->wlen;
        if (had) wbulk(c, old.v, old.vl); /* copied before the write below */
        else WLIT(c, "$-1\r\n");
        int r = kv_put(st->kv, argv[1], argl[1], argv[2], argl[2], h, exp, KV_T_STR, flags);
        wr_unlock(st);
        if (r < 0) c->wlen = save, WLIT(c, ERR_OOM); /* one reply only */
        return;
    }
    int r = do_put_h(argv[1], argl[1], argv[2], argl[2], h, exp, flags);
    if (r == 0) WLIT(c, "+OK\r\n");
    else if (r == 1) WLIT(c, "$-1\r\n");
    else WLIT(c, ERR_OOM);
}

/* INCR / DECR / INCRBY / DECRBY / APPEND / GETDEL / GETSET: read-modify-write under the lock */
static void cmd_incr(conn_t *c, const char *k, size_t kl, long long by)
{
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    kv_val_t v;
    long long cur = 0;
    if (kv_lookup(st->kv, k, kl, h, &v)) {
        if (v.type != KV_T_STR) {
            wr_unlock(st);
            return (void)WLIT(c, ERR_TYPE);
        }
        if (!parse_ll((const char *)v.v, v.vl, &cur)) {
            wr_unlock(st);
            return (void)WLIT(c, ERR_INT);
        }
    }
    if ((by > 0 && cur > LLONG_MAX - by) || (by < 0 && cur < LLONG_MIN - by)) {
        wr_unlock(st);
        return (void)WLIT(c, "-ERR increment or decrement would overflow\r\n");
    }
    cur += by;
    char b[24];
    int n = snprintf(b, sizeof b, "%lld", cur);
    int r = kv_put(st->kv, k, kl, b, n, h, 0, KV_T_STR, KV_KEEPTTL);
    wr_unlock(st);
    if (r < 0) WLIT(c, ERR_OOM);
    else wint(c, ':', cur);
}
static void cmd_append(conn_t *c, const char *k, size_t kl, const char *a, size_t al)
{
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    kv_val_t v;
    size_t ol = 0;
    char *buf;
    if (kv_lookup(st->kv, k, kl, h, &v)) {
        if (v.type != KV_T_STR) {
            wr_unlock(st);
            return (void)WLIT(c, ERR_TYPE);
        }
        ol = v.vl;
    }
    buf = malloc(ol + al + 1);
    if (ol) memcpy(buf, v.v, ol);
    memcpy(buf + ol, a, al);
    int r = kv_put(st->kv, k, kl, buf, ol + al, h, 0, KV_T_STR, KV_KEEPTTL);
    wr_unlock(st);
    free(buf);
    if (r < 0) WLIT(c, ERR_OOM);
    else wint(c, ':', (long long)(ol + al));
}
static void cmd_getdel(conn_t *c, const char *k, size_t kl, const char *nv, size_t nvl, int set)
{
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    kv_val_t v;
    int had = kv_lookup(st->kv, k, kl, h, &v);
    if (had && v.type != KV_T_STR) {
        wr_unlock(st);
        return (void)WLIT(c, ERR_TYPE);
    }
    if (had) wbulk(c, v.v, v.vl);
    else WLIT(c, "$-1\r\n");
    if (set) kv_put(st->kv, k, kl, nv, nvl, h, 0, KV_T_STR, 0);
    else if (had) kv_del(st->kv, k, kl, h);
    wr_unlock(st);
}
static void cmd_expire(conn_t *c, const char *k, size_t kl, const char *t, size_t tl, int unit_ms, int absolute)
{
    long long n;
    if (!parse_ll(t, tl, &n)) return (void)WLIT(c, ERR_INT);
    uint64_t now = kv_now_ms(), h = kv_hash(k, kl);
    long long rel_ms;
    if (absolute) {
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        long long wall = (long long)rt.tv_sec * 1000 + rt.tv_nsec / 1000000;
        rel_ms = (unit_ms ? n : n * 1000) - wall;
    } else {
        rel_ms = unit_ms ? n : n * 1000;
    }
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    int r;
    if (rel_ms <= 0) r = kv_del(st->kv, k, kl, h); /* a deadline in the past deletes the key */
    else r = kv_set_expire(st->kv, k, kl, h, now + (uint64_t)rel_ms);
    wr_unlock(st);
    wint(c, ':', r);
}
static void cmd_persist(conn_t *c, const char *k, size_t kl)
{
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    wr_lock(st);
    kv_val_t v;
    int r = 0;
    if (kv_lookup(st->kv, k, kl, h, &v) && v.expire_ms) r = kv_set_expire(st->kv, k, kl, h, 0);
    wr_unlock(st);
    wint(c, ':', r);
}

/* ---------------------------------------------------------------- objects (locked) */
/* look up key's object of type `want` under the (held) lock. create: make it if absent.
 * returns the object, NULL with *err set (1 wrong type, 2 oom) or NULL with *err = 0 (absent) */
static obj_t *obj_get(kv_t *kv, const char *k, size_t kl, uint64_t h, int want, int create, int *err)
{
    kv_val_t v;
    *err = 0;
    if (kv_lookup(kv, k, kl, h, &v)) {
        if (v.type != KV_T_OBJ) return *err = 1, NULL;
        obj_t *o;
        memcpy(&o, v.v, sizeof o);
        if (obj_type(o) != want) return *err = 1, NULL;
        return o;
    }
    if (!create) return NULL;
    obj_t *o = want == OBJ_LIST ? list_new() : want == OBJ_HASH ? hash_new() : want == OBJ_SET ? set_new() : zset_new();
    if (kv_put(kv, k, kl, &o, sizeof o, h, 0, KV_T_OBJ, 0) != 0) {
        obj_free(o);
        return *err = 2, NULL;
    }
    return o;
}
static size_t obj_len(obj_t *o)
{
    int t = obj_type(o);
    return t == OBJ_LIST ? list_len(o) : t == OBJ_ZSET ? zset_len(o) : dict_len(o);
}
/* Redis removes a key whose collection became empty */
static void obj_drop_if_empty(kv_t *kv, obj_t *o, const char *k, size_t kl, uint64_t h)
{
    if (o && obj_len(o) == 0) kv_del(kv, k, kl, h);
}
static void wstr(conn_t *c, const str_t *s)
{
    if (s) wbulk(c, s->d, s->len);
    else WLIT(c, "$-1\r\n");
}
static void each_kv(void *arg, const str_t *k, const str_t *v)
{
    conn_t *c = arg;
    wstr(c, k);
    if (v) wstr(c, v);
}
typedef struct {
    conn_t *c;
    int withscores;
} zr_arg_t;
static void each_z(void *arg, const str_t *m, double s)
{
    zr_arg_t *a = arg;
    wstr(a->c, m);
    if (a->withscores) wdouble(a->c, s);
}
static void range_norm(long *start, long *stop, long len)
{
    if (*start < 0) *start += len;
    if (*stop < 0) *stop += len;
    if (*start < 0) *start = 0;
    if (*stop >= len) *stop = len - 1;
}

/* one object command; key = argv[1]; returns after writing exactly one reply */
static void cmd_obj(conn_t *c, uint64_t w, int argc, char **argv, size_t *argl)
{
    const char *k = argv[1];
    size_t kl = argl[1];
    uint64_t h = kv_hash(k, kl);
    stripe_t *st = stripe_of(h);
    kv_t *kv = st->kv;
    int err;
    obj_t *o;
    long long n;
    wr_lock(st);
/* not WLIT(c, cond ? A : B): sizeof of a conditional is sizeof(char *), which truncated the reply */
#define FAIL(e)                         \
    do {                                \
        if ((e) == 1) WLIT(c, ERR_TYPE); \
        else WLIT(c, ERR_OOM);          \
    } while (0)
    if (w == CW("lpush") || w == CW("rpush")) {
        if (argc < 3) WLIT(c, ERR_ARGS);
        else if (!(o = obj_get(kv, k, kl, h, OBJ_LIST, 1, &err))) FAIL(err);
        else {
            for (int i = 2; i < argc; i++) list_push(o, w == CW("lpush"), argv[i], argl[i]);
            wint(c, ':', (long long)list_len(o));
        }
    } else if (w == CW("lpop") || w == CW("rpop")) {
        o = obj_get(kv, k, kl, h, OBJ_LIST, 0, &err);
        if (err) FAIL(err);
        else if (argc >= 3) { /* LPOP key count */
            if (!parse_ll(argv[2], argl[2], &n) || n < 0) WLIT(c, ERR_INT);
            else if (!o) WLIT(c, "*-1\r\n");
            else {
                long m = (long)(n < (long long)list_len(o) ? n : (long long)list_len(o));
                wint(c, '*', m);
                for (long i = 0; i < m; i++) {
                    str_t *s = list_pop(o, w == CW("lpop"));
                    wstr(c, s);
                    free(s);
                }
            }
        } else if (!o) WLIT(c, "$-1\r\n");
        else {
            str_t *s = list_pop(o, w == CW("lpop"));
            wstr(c, s);
            free(s);
        }
        obj_drop_if_empty(kv, o, k, kl, h);
    } else if (w == CW("llen")) {
        o = obj_get(kv, k, kl, h, OBJ_LIST, 0, &err);
        if (err) FAIL(err);
        else wint(c, ':', o ? (long long)list_len(o) : 0);
    } else if (w == CW("lindex")) {
        o = obj_get(kv, k, kl, h, OBJ_LIST, 0, &err);
        if (err) FAIL(err);
        else if (argc != 3 || !parse_ll(argv[2], argl[2], &n)) WLIT(c, ERR_INT);
        else wstr(c, o ? list_at(o, (long)n) : NULL);
    } else if (w == CW("lrange")) {
        long long a, b;
        o = obj_get(kv, k, kl, h, OBJ_LIST, 0, &err);
        if (err) FAIL(err);
        else if (argc != 4 || !parse_ll(argv[2], argl[2], &a) || !parse_ll(argv[3], argl[3], &b)) WLIT(c, ERR_INT);
        else if (!o) WLIT(c, "*0\r\n");
        else {
            long s = (long)a, e = (long)b, len = (long)list_len(o);
            range_norm(&s, &e, len);
            if (s > e || s >= len) WLIT(c, "*0\r\n");
            else {
                wint(c, '*', e - s + 1);
                for (long i = s; i <= e; i++) wstr(c, list_at(o, i));
            }
        }
    } else if (w == CW("hset") || w == CW("hmset")) {
        if (argc < 4 || argc % 2) WLIT(c, ERR_ARGS);
        else if (!(o = obj_get(kv, k, kl, h, OBJ_HASH, 1, &err))) FAIL(err);
        else {
            long added = 0;
            for (int i = 2; i + 1 < argc; i += 2) added += hash_set(o, argv[i], argl[i], argv[i + 1], argl[i + 1]);
            if (w == CW("hmset")) WLIT(c, "+OK\r\n");
            else wint(c, ':', added);
        }
    } else if (w == CW("hget")) {
        o = obj_get(kv, k, kl, h, OBJ_HASH, 0, &err);
        if (err) FAIL(err);
        else if (argc != 3) WLIT(c, ERR_ARGS);
        else wstr(c, o ? hash_get(o, argv[2], argl[2]) : NULL);
    } else if (w == CW("hmget")) {
        o = obj_get(kv, k, kl, h, OBJ_HASH, 0, &err);
        if (err) FAIL(err);
        else {
            wint(c, '*', argc - 2);
            for (int i = 2; i < argc; i++) wstr(c, o ? hash_get(o, argv[i], argl[i]) : NULL);
        }
    } else if (w == CW("hdel") || w == CW("srem") || w == CW("zrem")) {
        int t = w == CW("hdel") ? OBJ_HASH : w == CW("srem") ? OBJ_SET : OBJ_ZSET;
        o = obj_get(kv, k, kl, h, t, 0, &err);
        if (err) FAIL(err);
        else {
            long r = 0;
            for (int i = 2; o && i < argc; i++) r += t == OBJ_ZSET ? zset_rem(o, argv[i], argl[i]) : dict_del(o, argv[i], argl[i]);
            wint(c, ':', r);
            obj_drop_if_empty(kv, o, k, kl, h);
        }
    } else if (w == CW("hlen") || w == CW("scard") || w == CW("zcard")) {
        int t = w == CW("hlen") ? OBJ_HASH : w == CW("scard") ? OBJ_SET : OBJ_ZSET;
        o = obj_get(kv, k, kl, h, t, 0, &err);
        if (err) FAIL(err);
        else wint(c, ':', o ? (long long)obj_len(o) : 0);
    } else if (w == CW("hexists") || w == CW("sismember")) {
        int t = w == CW("hexists") ? OBJ_HASH : OBJ_SET;
        o = obj_get(kv, k, kl, h, t, 0, &err);
        if (err) FAIL(err);
        else if (argc != 3) WLIT(c, ERR_ARGS);
        else wint(c, ':', o ? (t == OBJ_HASH ? hash_get(o, argv[2], argl[2]) != NULL : set_has(o, argv[2], argl[2])) : 0);
    } else if (w == CW("hgetall") || w == CW("smembers")) {
        int t = w == CW("hgetall") ? OBJ_HASH : OBJ_SET;
        o = obj_get(kv, k, kl, h, t, 0, &err);
        if (err) FAIL(err);
        else if (!o) WLIT(c, "*0\r\n");
        else {
            wint(c, '*', (long long)dict_len(o) * (t == OBJ_HASH ? 2 : 1));
            dict_each(o, each_kv, c);
        }
    } else if (w == CW("sadd")) {
        if (argc < 3) WLIT(c, ERR_ARGS);
        else if (!(o = obj_get(kv, k, kl, h, OBJ_SET, 1, &err))) FAIL(err);
        else {
            long r = 0;
            for (int i = 2; i < argc; i++) r += set_add(o, argv[i], argl[i]);
            wint(c, ':', r);
        }
    } else if (w == CW("spop")) {
        o = obj_get(kv, k, kl, h, OBJ_SET, 0, &err);
        if (err) FAIL(err);
        else if (!o) WLIT(c, "$-1\r\n");
        else {
            str_t *s = set_pop(o);
            wstr(c, s);
            free(s);
            obj_drop_if_empty(kv, o, k, kl, h);
        }
    } else if (w == CW("zadd") || w == CW("zincrby")) {
        int incr = w == CW("zincrby");
        if (argc < 4 || argc % 2 || (incr && argc != 4)) WLIT(c, ERR_ARGS);
        else {
            double sc[32];
            int bad = 0, np = (argc - 2) / 2;
            for (int i = 0; i < np && !bad; i++) bad = i >= 32 || !parse_dbl(argv[2 + 2 * i], argl[2 + 2 * i], &sc[i]);
            if (bad) WLIT(c, ERR_FLOAT);
            else if (!(o = obj_get(kv, k, kl, h, OBJ_ZSET, 1, &err))) FAIL(err);
            else {
                long added = 0;
                double out = 0;
                for (int i = 0; i < np; i++) added += zset_add(o, sc[i], argv[3 + 2 * i], argl[3 + 2 * i], incr, &out);
                if (incr) wdouble(c, out);
                else wint(c, ':', added);
            }
        }
    } else if (w == CW("zscore")) {
        double s;
        o = obj_get(kv, k, kl, h, OBJ_ZSET, 0, &err);
        if (err) FAIL(err);
        else if (argc != 3) WLIT(c, ERR_ARGS);
        else if (o && zset_score(o, argv[2], argl[2], &s)) wdouble(c, s);
        else WLIT(c, "$-1\r\n");
    } else if (w == CW("zrange")) {
        long long a, b;
        o = obj_get(kv, k, kl, h, OBJ_ZSET, 0, &err);
        int withs = argc == 5 && cmdword(argv[4], argl[4]) == CW("withscores");
        if (err) FAIL(err);
        else if ((argc != 4 && !withs) || !parse_ll(argv[2], argl[2], &a) || !parse_ll(argv[3], argl[3], &b)) WLIT(c, ERR_SYNTAX);
        else if (!o) WLIT(c, "*0\r\n");
        else {
            long s = (long)a, e = (long)b, len = (long)zset_len(o);
            range_norm(&s, &e, len);
            if (s > e || s >= len) WLIT(c, "*0\r\n");
            else {
                wint(c, '*', (e - s + 1) * (withs ? 2 : 1));
                zr_arg_t za = {c, withs};
                zset_range(o, s, e, each_z, &za);
            }
        }
    } else if (w == CW("zpopmin")) {
        long long cnt = 1;
        o = obj_get(kv, k, kl, h, OBJ_ZSET, 0, &err);
        if (err) FAIL(err);
        else if (argc == 3 && !parse_ll(argv[2], argl[2], &cnt)) WLIT(c, ERR_INT);
        else if (!o) WLIT(c, "*0\r\n");
        else {
            long m = (long)(cnt < (long long)zset_len(o) ? cnt : (long long)zset_len(o));
            if (m < 0) m = 0;
            wint(c, '*', m * 2);
            for (long i = 0; i < m; i++) {
                double s;
                str_t *mm = zset_popmin(o, &s);
                wstr(c, mm);
                wdouble(c, s);
                free(mm);
            }
            obj_drop_if_empty(kv, o, k, kl, h);
        }
    } else if (w == CW("type")) {
        kv_val_t v;
        if (!kv_lookup(kv, k, kl, h, &v)) WLIT(c, "+none\r\n");
        else if (v.type == KV_T_STR) WLIT(c, "+string\r\n");
        else {
            memcpy(&o, v.v, sizeof o);
            int t = obj_type(o);
            if (t == OBJ_LIST) WLIT(c, "+list\r\n");
            else if (t == OBJ_HASH) WLIT(c, "+hash\r\n");
            else if (t == OBJ_SET) WLIT(c, "+set\r\n");
            else WLIT(c, "+zset\r\n");
        }
    }
#undef FAIL
    wr_unlock(st);
}

static int is_obj_cmd(uint64_t w)
{
    static const char *names[] = {"lpush", "rpush", "lpop", "rpop", "llen", "lindex", "lrange", "hset", "hmset",
                                  "hget", "hmget", "hdel", "hlen", "hexists", "hgetall", "sadd", "srem", "scard",
                                  "sismember", "smembers", "spop", "zadd", "zincrby", "zscore", "zrem", "zcard",
                                  "zrange", "zpopmin"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (w == cmdword(names[i], strlen(names[i]))) return 1;
    return 0;
}

/* ---------------------------------------------------------------- dispatch */
static void exec_cmd(conn_t *c, int argc, char **argv, size_t *argl)
{
    if (argc == 0) return;
    ws->reqs++;
    uint64_t w = cmdword(argv[0], argl[0]);
    if (w == CW("get") && argc == 2) return do_get(c, argv[1], argl[1], 0, NULL);
    if (w == CW("set") && argc >= 3) return cmd_set(c, argc, argv, argl);
    if (is_obj_cmd(w)) {
        if (argc < 2) return (void)WLIT(c, ERR_ARGS);
        return cmd_obj(c, w, argc, argv, argl);
    }
    if (w == CW("del") || w == CW("unlink")) {
        long n = 0;
        for (int i = 1; i < argc; i++) n += do_del(argv[i], argl[i]);
        wint(c, ':', n);
    } else if (w == CW("exists")) {
        long n = 0;
        for (int i = 1; i < argc; i++) do_get(c, argv[i], argl[i], 1, &n);
        wint(c, ':', n);
    } else if (w == CW("mget")) {
        wint(c, '*', argc - 1);
        long d;
        for (int i = 1; i < argc; i++) rd_op(c, argv[i], argl[i], kv_hash(argv[i], argl[i]), RD_MGET, &d);
    } else if (w == CW("mset") && argc % 2 == 1 && argc >= 3) {
        int oom = 0;
        for (int i = 1; i < argc; i += 2) oom |= do_set(argv[i], argl[i], argv[i + 1], argl[i + 1]) < 0;
        if (oom) WLIT(c, ERR_OOM);
        else WLIT(c, "+OK\r\n");
    } else if ((w == CW("setnx") && argc == 3)) {
        int r = do_put_h(argv[1], argl[1], argv[2], argl[2], kv_hash(argv[1], argl[1]), 0, KV_NX);
        if (r < 0) WLIT(c, ERR_OOM);
        else wint(c, ':', r == 0);
    } else if ((w == CW("setex") || w == CW("psetex")) && argc == 4) {
        long long n;
        if (!parse_ll(argv[2], argl[2], &n) || n <= 0) return (void)WLIT(c, "-ERR invalid expire time\r\n");
        uint64_t exp = kv_now_ms() + (uint64_t)n * (w == CW("setex") ? 1000 : 1);
        int r = do_put_h(argv[1], argl[1], argv[3], argl[3], kv_hash(argv[1], argl[1]), exp, 0);
        if (r < 0) WLIT(c, ERR_OOM);
        else WLIT(c, "+OK\r\n");
    } else if (w == CW("getset") && argc == 3) {
        cmd_getdel(c, argv[1], argl[1], argv[2], argl[2], 1);
    } else if (w == CW("getdel") && argc == 2) {
        cmd_getdel(c, argv[1], argl[1], NULL, 0, 0);
    } else if (w == CW("incr") && argc == 2) {
        cmd_incr(c, argv[1], argl[1], 1);
    } else if (w == CW("decr") && argc == 2) {
        cmd_incr(c, argv[1], argl[1], -1);
    } else if ((w == CW("incrby") || w == CW("decrby")) && argc == 3) {
        long long n;
        if (!parse_ll(argv[2], argl[2], &n)) WLIT(c, ERR_INT);
        else cmd_incr(c, argv[1], argl[1], w == CW("incrby") ? n : -n);
    } else if (w == CW("append") && argc == 3) {
        cmd_append(c, argv[1], argl[1], argv[2], argl[2]);
    } else if (w == CW("strlen") && argc == 2) {
        long d;
        rd_op(c, argv[1], argl[1], kv_hash(argv[1], argl[1]), RD_STRLEN, &d);
    } else if ((w == CW("ttl") || w == CW("pttl")) && argc == 2) {
        long d;
        rd_op(c, argv[1], argl[1], kv_hash(argv[1], argl[1]), w == CW("ttl") ? RD_TTL : RD_PTTL, &d);
    } else if (w == CW("type") && argc == 2) {
        long d;
        if (!rd_op(c, argv[1], argl[1], kv_hash(argv[1], argl[1]), RD_TYPE, &d)) cmd_obj(c, w, argc, argv, argl);
    } else if ((w == CW("expire") || w == CW("pexpire") || w == CW("expireat") || w == CW("pexpireat")) && argc == 3) {
        cmd_expire(c, argv[1], argl[1], argv[2], argl[2], w == CW("pexpire") || w == CW("pexpireat"),
                   w == CW("expireat") || w == CW("pexpireat"));
    } else if (w == CW("persist") && argc == 2) {
        cmd_persist(c, argv[1], argl[1]);
    } else if (w == CW("ping")) {
        if (argc > 1) wbulk(c, argv[1], argl[1]);
        else WLIT(c, "+PONG\r\n");
    } else if (w == CW("echo") && argc == 2) {
        wbulk(c, argv[1], argl[1]);
    } else if (w == CW("dbsize")) {
        uint64_t n = 0;
        for (uint32_t i = 0; i < nstripes; i++) {
            kv_info_t in;
            wr_lock(&stripes[i]);
            kv_expire_step(stripes[i].kv, 1024); /* reclaim recently expired keys before counting */
            kv_info(stripes[i].kv, &in);
            wr_unlock(&stripes[i]);
            n += in.count;
        }
        wint(c, ':', (long long)n);
    } else if (w == CW("flushall") || w == CW("flushdb")) {
        for (uint32_t i = 0; i < nstripes; i++) {
            wr_lock(&stripes[i]);
            kv_flush(stripes[i].kv);
            wr_unlock(&stripes[i]);
        }
        WLIT(c, "+OK\r\n");
    } else if (w == CW("config") || w == CW("command") || w == CW("client") || w == CW("select")) {
        if (w == CW("select")) WLIT(c, "+OK\r\n");
        else WLIT(c, "*0\r\n");
    } else if (w == CW("info")) {
        uint64_t rr = 0, sp = 0, rq = 0, keys = 0, used = 0, slabs = 0, ev = 0, ex = 0, lim = 0;
        for (int i = 0; i < 16; i++) rr += wstats[i].rd_retries, sp += wstats[i].wr_spins, rq += wstats[i].reqs;
        for (uint32_t i = 0; i < nstripes; i++) {
            kv_info_t in;
            wr_lock(&stripes[i]);
            kv_info(stripes[i].kv, &in);
            wr_unlock(&stripes[i]);
            keys += in.count, used += in.used_bytes, slabs += in.slabs_bytes, ev += in.evictions, ex += in.expired;
            lim += in.limit_bytes;
        }
        char b[512];
        int n = snprintf(b, sizeof b,
                         "# kvpi\r\nstripes:%u\r\nrequests:%lu\r\nread_retries:%lu\r\nwrite_spins:%lu\r\nkeys:%lu\r\n"
                         "used_bytes:%lu\r\nslab_bytes:%lu\r\nlimit_bytes:%lu\r\nevicted_keys:%lu\r\nexpired_keys:%lu\r\n",
                         nstripes, (unsigned long)rq, (unsigned long)rr, (unsigned long)sp, (unsigned long)keys,
                         (unsigned long)used, (unsigned long)slabs, (unsigned long)lim, (unsigned long)ev,
                         (unsigned long)ex);
        wbulk(c, b, n);
    } else if (w == CW("quit")) {
        WLIT(c, "+OK\r\n");
        c->closing = 1;
    } else {
        WLIT(c, "-ERR unknown command\r\n");
    }
}

#if KV_BATCH
/* v3d batched execution (MICA-style memory-level parallelism), unchanged: plain GET and plain
 * 3-argument SET are batched with prefetch; every other command flushes the batch first. */
#define BATCH_MAX 64
typedef struct {
    int argc;
    char *argv[3];
    size_t argl[3];
    uint64_t h;
    int kind;
} bcmd_t;

static inline void run_bcmd(conn_t *c, bcmd_t *x)
{
    ws->reqs++;
    if (x->kind == 1) do_get_h(c, x->argv[1], x->argl[1], x->h, 0, NULL);
    else if (do_set_h(x->argv[1], x->argl[1], x->argv[2], x->argl[2], x->h) == 0) WLIT(c, "+OK\r\n");
    else WLIT(c, ERR_OOM);
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
            if (!x->kind) {
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
        if (r != 1) return r;
    }
}
#endif
