/* kvpi engine v5: v4 engine (frozen_v4/engine.c) plus the features a fair comparison needs.
 *  - expiry: optional 8 B absolute deadline (ms) after the header, flagged per entry (as on the ESP32).
 *    Readers treat an expired entry as absent; writers delete it on access; kv_expire_step() sweeps.
 *  - memory limit + eviction: approximate LRU like Redis. The index slot is now
 *      [ttl:1 | tag:11 | lru:12 | ref:40]   (v0-v4: [tag:16 | ref:48]; ref = arena offset / 8 -> 8 TiB)
 *    lru = last-access time in seconds (mod 4096). A reader refreshes it with a CAS on the slot only
 *    when the second has changed, so a hot key costs at most one shared-line write per second, and a
 *    CAS can never corrupt a slot that a writer changed meanwhile. At the limit, kv_put samples index
 *    slots for entries of the slab class it needs and evicts the least recently used of 5.
 *  - object values (lists, hashes, sets, sorted sets): the entry value is an 8 B pointer; the object
 *    is freed through a callback when the key is overwritten, deleted, expired, evicted or flushed.
 *  - entry: [vlen u32 | klen u8 | cls u8 | flags u16] [expire u64 if F_TTL] [key] [value]
 *    A free chunk holds the free-list link in its first 8 B; links are < 2^48, so the flags field of a
 *    free chunk reads 0 and F_USED tells live entries from free chunks.
 * Compile-time: KV_HASH_CRC (hardware CRC32C hash). */
#define _GNU_SOURCE
#include "engine.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if KV_HASH_CRC
#include <arm_acle.h>
#endif

#define SLAB_PAGE (1u << 20) /* largest chunk (class sizes stop here) */
#define PAGE_SMALL (64u << 10) /* slab page for classes <= 64 KiB; bigger classes: one chunk per page */
#define HDR 8
#define MAXCLS 96
#define EMPTY 0
#define F_USED 1
#define F_TTL 2
#define F_OBJ 4
#define F_BLOB 8 /* value is a kv_blob_t* (large strings, stored out of line) */
#define EVICT_SAMPLES 5
#define EVICT_TRIES 64

typedef struct {
    uint32_t vlen;
    uint8_t klen, cls;
    uint16_t flags;
} ehdr_t;

struct kv {
    uint64_t *idx;
    uint64_t mask, count;
    uint8_t *arena;
    size_t arena_bytes, arena_top;
    size_t limit; /* slab pages may not grow past this */
    int policy;
    uint32_t ncls;
    uint32_t cls_size[MAXCLS];
    uint64_t free_head[MAXCLS];
    uint64_t cls_page_off[MAXCLS], cls_page_left[MAXCLS];
    uint64_t used_bytes, slabs_bytes, blob_bytes, evictions, expired, exp_cursor, rng;
    int shared;
};

static void (*obj_free_fn)(void *);
void kv_set_obj_free(void (*fn)(void *)) { obj_free_fn = fn; }
/* deferred free for memory optimistic readers on other threads may still be using (old index tables):
 * the server hands it to its QSBR (store.h); without a hook, shared indexes are kept mapped (v4 rule) */
static void (*retire_fn)(void *p, void (*fn)(void *));
void kv_set_retire(void (*fn)(void *p, void (*free_fn)(void *))) { retire_fn = fn; }

static volatile uint64_t g_now_ms;
void kv_clock_set(uint64_t now_ms) { g_now_ms = now_ms; }
uint64_t kv_now_ms(void) { return g_now_ms; }
static inline uint64_t lru_now(void) { return (g_now_ms / 1000) & 0xFFF; }

/* ---------------------------------------------------------------- hash */
uint64_t kv_hash(const void *kp, size_t n)
{
    const uint8_t *k = kp;
#if KV_HASH_CRC
    uint32_t a = 0xFFFFFFFFu, b = 0x9E3779B9u;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, k + i, 8);
        a = __crc32cd(a, v);
        b = __crc32cd(b, v ^ 0x5bd1e9955bd1e995ull);
    }
    for (; i < n; i++) a = __crc32cb(a, k[i]), b = __crc32cb(b, k[i] ^ 0x5b);
    uint64_t h = ((uint64_t)a << 32) | b;
    h ^= h >> 33, h *= 0xff51afd7ed558ccdull, h ^= h >> 33;
    return h;
#else
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ k[i]) * 16777619u;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    uint64_t x = h * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return x ^ ((uint64_t)h << 32);
#endif
}

#define REF_BITS 40
#define TAG_OF(h) ((h) >> 53) /* 11-bit tag */
#define HOME(mask, h) (((h) >> 8) & (mask))
/* slot: [ttl:1 | tag:11 | lru:12 | ref:40]; ttl = the entry has a deadline (lets the sweep skip the rest
 * without touching their entries, the job Redis gives its separate expires table) */
#define SLOT(tag, ttl, lru, ref) (((uint64_t)(ttl) << 63) | ((uint64_t)(tag) << 52) | ((uint64_t)(lru) << REF_BITS) | (ref))
#define S_TAG(s) (((s) >> 52) & 0x7FF)
#define S_TTL(s) ((s) >> 63)
#define S_WITH_LRU(s, lru) (((s) & ~(0xFFFull << REF_BITS)) | ((uint64_t)(lru) << REF_BITS))
#define S_LRU(s) (((s) >> REF_BITS) & 0xFFF)
#define REF(s) ((s) & ((1ull << REF_BITS) - 1))
#define E(kv, ref) ((ehdr_t *)((kv)->arena + ((ref) << 3)))
#define KOFF(f) (HDR + (((f) & F_TTL) ? 8 : 0))
#define KEY(e) ((uint8_t *)(e) + KOFF((e)->flags))
#define VAL(e) (KEY(e) + (e)->klen)
static inline uint64_t e_expire(const ehdr_t *e)
{
    uint64_t x = 0;
    if (e->flags & F_TTL) memcpy(&x, (const uint8_t *)e + HDR, 8);
    return x;
}
static inline size_t e_size(const ehdr_t *e) { return KOFF(e->flags) + e->klen + e->vlen; }
static inline int expired(uint64_t exp) { return exp && exp <= g_now_ms; }

/* ---------------------------------------------------------------- slabs */
static void init_classes(kv_t *kv)
{
    uint32_t s = 16, n = 0;
    while (s < SLAB_PAGE && n < MAXCLS) {
        kv->cls_size[n++] = s;
        s = s < 128 ? s + 8 : (s + s / 8 + 7) & ~7u;
    }
    if (n < MAXCLS) kv->cls_size[n++] = SLAB_PAGE;
    kv->ncls = n;
}

static int cls_for(kv_t *kv, size_t bytes)
{
    if (bytes <= 128) return bytes <= 16 ? 0 : (int)((bytes - 16 + 7) / 8);
    int lo = 14, hi = kv->ncls - 1;
    if (bytes > kv->cls_size[hi]) return -1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (kv->cls_size[mid] >= bytes) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

static uint64_t chunk_take(kv_t *kv, int c)
{
    uint64_t off = kv->free_head[c];
    if (off) {
        kv->free_head[c] = *(uint64_t *)(kv->arena + off);
        return off;
    }
    /* page per class: 64 KiB for small chunks; above 4 KiB a whole number of chunks (no wasted tail) */
    uint32_t sz = kv->cls_size[c], pg = sz <= 4096 ? PAGE_SMALL : ((PAGE_SMALL + sz - 1) / sz) * sz;
    if (kv->cls_page_left[c] < sz) {
        if (kv->arena_top + pg > kv->limit) return 0;
        kv->cls_page_off[c] = kv->arena_top;
        kv->cls_page_left[c] = pg;
        kv->arena_top += pg;
        kv->slabs_bytes += pg;
    }
    off = kv->cls_page_off[c];
    kv->cls_page_off[c] += sz;
    kv->cls_page_left[c] -= sz;
    return off;
}

static void chunk_free(kv_t *kv, uint64_t off, int c)
{
    *(uint64_t *)(kv->arena + off) = kv->free_head[c]; /* also zeroes the flags field (link < 2^48) */
    kv->free_head[c] = off;
}

/* ---------------------------------------------------------------- index */
static uint64_t *idx_alloc(uint64_t nslots)
{
    void *p = mmap(NULL, nslots * 8 + 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    uint64_t *s = (uint64_t *)p + 8;
    s[-8] = nslots - 1;
    return s;
}
static void idx_free(uint64_t *s) { munmap(s - 8, (s[-8] + 1) * 8 + 64); }
static void idx_free_cb(void *s) { idx_free(s); }

static void idx_grow(kv_t *kv)
{
    uint64_t old_n = kv->mask + 1, n = old_n * 2;
    uint64_t *old = kv->idx, *nw = idx_alloc(n);
    if (!nw) return;
    uint64_t nmask = n - 1;
    for (uint64_t i = 0; i < old_n; i++) {
        uint64_t s = old[i];
        if (s == EMPTY) continue;
        ehdr_t *e = E(kv, REF(s));
        uint64_t p = HOME(nmask, kv_hash(KEY(e), e->klen));
        while (nw[p] != EMPTY) p = (p + 1) & nmask;
        nw[p] = s;
    }
    __atomic_store_n(&kv->idx, nw, __ATOMIC_RELEASE);
    kv->mask = nmask;
    if (!kv->shared) idx_free(old);
    else if (retire_fn) retire_fn(old, idx_free_cb); /* freed once every worker has passed a quiescent point */
}

static inline int key_eq(ehdr_t *e, const void *k, size_t kl) { return e->klen == kl && memcmp(KEY(e), k, kl) == 0; }

static inline int64_t find(kv_t *kv, const void *k, size_t kl, uint64_t h)
{
    uint64_t p = HOME(kv->mask, h), tag = TAG_OF(h);
    for (;;) {
        uint64_t s = kv->idx[p];
        if (s == EMPTY) return -(int64_t)p - 1;
        if (S_TAG(s) == tag && key_eq(E(kv, REF(s)), k, kl)) return (int64_t)p;
        p = (p + 1) & kv->mask;
    }
}

/* ---------------------------------------------------------------- large-value blobs
 * Strings >= KV_BLOB_MIN live in an exactly-sized heap blob; the entry holds one reference. A reader
 * (after validating its seqlock) may take a reference only while the count is non-zero, so once the
 * count reaches zero it stays zero; the thread that drops it to zero retires the blob, and QSBR keeps
 * the memory valid for readers still inside their current loop pass. Sends hold a reference until the
 * kernel has the bytes, so the value is sent straight from the blob without a copy. */
static void blob_free_cb(void *b) { free(b); }
int kv_blob_try_ref(kv_blob_t *b)
{
    uint32_t r = __atomic_load_n(&b->refs, __ATOMIC_RELAXED);
    while (r) {
        if (__atomic_compare_exchange_n(&b->refs, &r, r + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    }
    return 0;
}
void kv_blob_unref(kv_blob_t *b)
{
    if (__atomic_fetch_sub(&b->refs, 1, __ATOMIC_ACQ_REL) == 1) {
        if (retire_fn) retire_fn(b, blob_free_cb);
        else free(b);
    }
}

static void drop_value(kv_t *kv, ehdr_t *e)
{
    if ((e->flags & F_OBJ) && obj_free_fn) {
        void *o;
        memcpy(&o, VAL(e), sizeof o);
        obj_free_fn(o);
    } else if (e->flags & F_BLOB) {
        kv_blob_t *b;
        memcpy(&b, VAL(e), sizeof b);
        kv->blob_bytes -= b->len;
        kv_blob_unref(b);
    }
}

/* remove the entry at index position pos: free value + chunk, backward-shift the cluster */
static void del_at(kv_t *kv, uint64_t pos)
{
    uint64_t ref = REF(kv->idx[pos]);
    ehdr_t *e = E(kv, ref);
    drop_value(kv, e);
    kv->used_bytes -= e_size(e);
    chunk_free(kv, ref << 3, e->cls);
    kv->count--;
    uint64_t i = pos, j = i;
    for (;;) {
        j = (j + 1) & kv->mask;
        uint64_t s = kv->idx[j];
        if (s == EMPTY) break;
        ehdr_t *ej = E(kv, REF(s));
        uint64_t home = HOME(kv->mask, kv_hash(KEY(ej), ej->klen));
        if (((j - home) & kv->mask) >= ((j - i) & kv->mask)) {
            kv->idx[i] = s;
            i = j;
        }
    }
    kv->idx[i] = EMPTY;
}

static inline uint64_t xs(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *s = x;
}

/* approximate LRU: sample random slots, among entries of slab class c pick the oldest of up to
 * EVICT_SAMPLES and delete it. Returns 1 if something of class c was freed. Expired entries found
 * on the way are always taken first. */
static int evict_one(kv_t *kv, int c)
{
    if (kv->policy != KV_LRU || kv->count == 0) return 0;
    uint64_t now = lru_now(), best = ~0ull, best_age = 0;
    int got = 0;
    for (int t = 0; t < EVICT_TRIES && got < EVICT_SAMPLES; t++) {
        uint64_t p = xs(&kv->rng) & kv->mask;
        /* walk to the next used slot (bounded) */
        for (int w = 0; w < 16 && kv->idx[p] == EMPTY; w++) p = (p + 1) & kv->mask;
        uint64_t s = kv->idx[p];
        if (s == EMPTY) continue;
        ehdr_t *e = E(kv, REF(s));
        if (c >= 0 && e->cls != c) continue; /* c < 0: any class (blob memory) */
        if (expired(e_expire(e))) {
            best = p;
            break;
        }
        uint64_t age = (now - S_LRU(s)) & 0xFFF;
        if (best == ~0ull || age > best_age) best = p, best_age = age;
        got++;
    }
    if (best == ~0ull) return 0;
    del_at(kv, best);
    kv->evictions++;
    return 1;
}

static uint64_t chunk_alloc(kv_t *kv, int c)
{
    uint64_t off = chunk_take(kv, c);
    for (int i = 0; !off && i < 8 && evict_one(kv, c); i++) off = chunk_take(kv, c);
    return off;
}

/* ---------------------------------------------------------------- API */
kv_t *kv_create(size_t arena_bytes, size_t expected_keys)
{
    kv_t *kv = calloc(1, sizeof *kv);
    if (!kv) return NULL;
    init_classes(kv);
    kv->arena_bytes = arena_bytes;
    kv->limit = arena_bytes;
    kv->arena = mmap(NULL, arena_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (kv->arena == MAP_FAILED) return free(kv), NULL;
    kv->arena_top = PAGE_SMALL; /* offset 0 means empty: burn the first page */
    kv->rng = 0x9E3779B97F4A7C15ull ^ (uintptr_t)kv;
    uint64_t n = 1024;
    while (n * 3 / 4 < expected_keys) n *= 2;
    kv->idx = idx_alloc(n);
    kv->mask = n - 1;
    return kv;
}

void kv_set_limit(kv_t *kv, size_t limit_bytes, int policy)
{
    size_t l = limit_bytes ? limit_bytes + PAGE_SMALL : kv->arena_bytes; /* + the burnt first page */
    kv->limit = l < PAGE_SMALL * 2 ? PAGE_SMALL * 2 : l > kv->arena_bytes ? kv->arena_bytes : l;
    kv->policy = policy;
}

void kv_destroy(kv_t *kv)
{
    kv_flush(kv);
    idx_free(kv->idx);
    munmap(kv->arena, kv->arena_bytes);
    free(kv);
}

static void write_entry(ehdr_t *n, int c, const void *k, size_t kl, const void *v, size_t vl, uint64_t exp, int type)
{
    *n = (ehdr_t){.vlen = (uint32_t)vl, .klen = (uint8_t)kl, .cls = (uint8_t)c,
                  .flags = (uint16_t)(F_USED | (exp ? F_TTL : 0) | (type == KV_T_OBJ ? F_OBJ : 0) |
                                      (type == KV_T_BLOB ? F_BLOB : 0))};
    if (exp) memcpy((uint8_t *)n + HDR, &exp, 8);
    memcpy(KEY(n), k, kl);
    memcpy(VAL(n), v, vl);
}

static int put_raw(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t h, uint64_t exp, int type,
                   int flags);
int kv_put(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t h, uint64_t exp, int type, int flags)
{
    if (type != KV_T_STR || vl < KV_BLOB_MIN) return put_raw(kv, k, kl, v, vl, h, exp, type, flags);
    if (kl == 0 || kl > 255 || vl > 0xFFFFFFFFu) return -1;
    /* large string: out-of-line blob; it counts against the memory limit like slab pages do */
    for (int i = 0; kv->arena_top + kv->blob_bytes + vl > kv->limit; i++)
        if (i >= 64 || !evict_one(kv, -1)) return -1;
    kv_blob_t *b = malloc(sizeof *b + vl);
    if (!b) return -1;
    b->refs = 1, b->len = (uint32_t)vl;
    memcpy(b->d, v, vl);
    int r = put_raw(kv, k, kl, &b, sizeof b, h, exp, KV_T_BLOB, flags);
    if (r == 0) kv->blob_bytes += vl;
    else free(b); /* never published */
    return r;
}

static int put_raw(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t h, uint64_t exp, int type,
                   int flags)
{
    if (kl == 0 || kl > 255 || vl > 0xFFFFFFFFu) return -1;
    int64_t pos = find(kv, k, kl, h);
    if (pos >= 0 && expired(e_expire(E(kv, REF(kv->idx[pos]))))) { /* lazily drop an expired old value */
        del_at(kv, (uint64_t)pos);
        kv->expired++;
        pos = find(kv, k, kl, h);
    }
    if ((flags & KV_NX) && pos >= 0) return 1;
    if ((flags & KV_XX) && pos < 0) return 1;
    if (pos >= 0 && (flags & KV_KEEPTTL)) exp = e_expire(E(kv, REF(kv->idx[pos])));
    size_t need = KOFF(exp ? F_TTL : 0) + kl + vl;
    int c = cls_for(kv, need);
    if (c < 0) return -1;
    if (pos >= 0) {
        uint64_t s = kv->idx[pos], ref = REF(s);
        ehdr_t *e = E(kv, ref);
        if (e->cls == c && ((e->flags & F_TTL) != 0) == (exp != 0) && !(e->flags & (F_OBJ | F_BLOB)) && type == KV_T_STR) {
            kv->used_bytes += vl, kv->used_bytes -= e->vlen; /* in-place overwrite, same layout */
            e->vlen = (uint32_t)vl;
            if (exp) memcpy((uint8_t *)e + HDR, &exp, 8);
            memcpy(VAL(e), v, vl);
            kv->idx[pos] = SLOT(S_TAG(s), exp != 0, lru_now(), ref);
            return 0;
        }
        uint64_t off = chunk_alloc(kv, c);
        if (!off) return -1;
        pos = find(kv, k, kl, h); /* eviction may have moved (or removed) our slot */
        if (pos < 0) goto insert_new;
        s = kv->idx[pos], ref = REF(s), e = E(kv, ref);
        write_entry((ehdr_t *)(kv->arena + off), c, k, kl, v, vl, exp, type);
        drop_value(kv, e);
        kv->used_bytes += need, kv->used_bytes -= e_size(e);
        chunk_free(kv, ref << 3, e->cls);
        kv->idx[pos] = SLOT(TAG_OF(h), exp != 0, lru_now(), off >> 3);
        return 0;
    insert_new:
        write_entry((ehdr_t *)(kv->arena + off), c, k, kl, v, vl, exp, type);
        if ((kv->count + 1) * 4 > (kv->mask + 1) * 3) idx_grow(kv);
        pos = find(kv, k, kl, h);
        kv->idx[-pos - 1] = SLOT(TAG_OF(h), exp != 0, lru_now(), off >> 3);
        kv->count++;
        kv->used_bytes += need;
        return 0;
    }
    uint64_t off = chunk_alloc(kv, c);
    if (!off) return -1;
    write_entry((ehdr_t *)(kv->arena + off), c, k, kl, v, vl, exp, type);
    if ((kv->count + 1) * 4 > (kv->mask + 1) * 3) idx_grow(kv);
    pos = find(kv, k, kl, h); /* eviction / growth may have moved the free position */
    kv->idx[-pos - 1] = SLOT(TAG_OF(h), exp != 0, lru_now(), off >> 3);
    kv->count++;
    kv->used_bytes += need;
    return 0;
}

static void fill(ehdr_t *e, kv_val_t *o)
{
    o->v = VAL(e);
    o->vl = e->vlen;
    o->blob = NULL;
    o->expire_ms = e_expire(e);
    o->type = (e->flags & F_OBJ) ? KV_T_OBJ : KV_T_STR;
    if (e->flags & F_BLOB) { /* writer side (lock held): the blob is stable, expose its bytes */
        memcpy(&o->blob, VAL(e), sizeof o->blob);
        o->v = (const uint8_t *)o->blob->d;
        o->vl = o->blob->len;
    }
}

int kv_lookup(kv_t *kv, const void *k, size_t kl, uint64_t h, kv_val_t *out)
{
    int64_t pos = find(kv, k, kl, h);
    if (pos < 0) return 0;
    uint64_t s = kv->idx[pos];
    ehdr_t *e = E(kv, REF(s));
    if (expired(e_expire(e))) {
        del_at(kv, (uint64_t)pos);
        kv->expired++;
        return 0;
    }
    if (S_LRU(s) != lru_now()) kv->idx[pos] = S_WITH_LRU(s, lru_now());
    fill(e, out);
    return 1;
}

int kv_del(kv_t *kv, const void *k, size_t kl, uint64_t h)
{
    int64_t pos = find(kv, k, kl, h);
    if (pos < 0) return 0;
    int was_live = !expired(e_expire(E(kv, REF(kv->idx[pos]))));
    del_at(kv, (uint64_t)pos);
    return was_live;
}

int kv_set_expire(kv_t *kv, const void *k, size_t kl, uint64_t h, uint64_t exp)
{
    kv_val_t cur;
    if (!kv_lookup(kv, k, kl, h, &cur)) return 0;
    int64_t pos = find(kv, k, kl, h);
    ehdr_t *e = E(kv, REF(kv->idx[pos]));
    if ((e->flags & F_TTL) && exp) { /* same layout: just rewrite the deadline */
        memcpy((uint8_t *)e + HDR, &exp, 8);
        return 1;
    }
    if (!(e->flags & F_TTL) && !exp) return 1;
    /* layout changes (deadline added or removed): rewrite the entry; an object pointer moves along */
    int own = e->flags & (F_OBJ | F_BLOB);
    int type = (e->flags & F_OBJ) ? KV_T_OBJ : (e->flags & F_BLOB) ? KV_T_BLOB : KV_T_STR;
    size_t vl = e->vlen;
    uint8_t stackbuf[512], *tmp = vl <= sizeof stackbuf ? stackbuf : malloc(vl);
    memcpy(tmp, VAL(e), vl);
    e->flags &= ~(F_OBJ | F_BLOB); /* the pointer is copied, not freed */
    int r = put_raw(kv, k, kl, tmp, vl, h, exp, type, 0);
    if (tmp != stackbuf) free(tmp);
    if (r != 0 && own) { /* failed: the old entry still owns the object / blob */
        pos = find(kv, k, kl, h);
        if (pos >= 0) E(kv, REF(kv->idx[pos]))->flags |= own;
    }
    return r == 0;
}

size_t kv_expire_step(kv_t *kv, size_t nslots)
{
    size_t n = 0;
    if (kv->count == 0) return 0;
    if (nslots > kv->mask + 1) nslots = kv->mask + 1; /* at most one full pass */
    for (size_t i = 0; i < nslots; i++) {
        uint64_t p = kv->exp_cursor & kv->mask;
        uint64_t s = kv->idx[p];
        if (S_TTL(s) && expired(e_expire(E(kv, REF(s))))) { /* no ttl bit: no entry access */
            del_at(kv, p); /* something may have shifted into p: look at it again next */
            kv->expired++;
            n++;
            continue;
        }
        kv->exp_cursor++;
    }
    return n;
}

void kv_set_shared(kv_t *kv, int on) { kv->shared = on; }

int kv_get_rd(kv_t *kv, const void *k, size_t kl, uint64_t h, kv_val_t *out)
{
    const uint64_t *ix = __atomic_load_n(&kv->idx, __ATOMIC_ACQUIRE);
    uint64_t mask = ix[-8], p = HOME(mask, h), tag = TAG_OF(h);
    for (uint64_t n = 0; n <= mask; n++) {
        uint64_t s = __atomic_load_n(&ix[p], __ATOMIC_RELAXED);
        if (s == EMPTY) return 0;
        if (S_TAG(s) == tag) {
            uint64_t off = REF(s) << 3;
            if (off + HDR + 8 > kv->arena_bytes) return -1;
            ehdr_t e;
            memcpy(&e, kv->arena + off, sizeof e);
            size_t ko = KOFF(e.flags);
            if (off + ko + e.klen + (uint64_t)e.vlen > kv->arena_bytes) return -1;
            if (e.klen == kl && memcmp(kv->arena + off + ko, k, kl) == 0) {
                uint64_t exp = 0;
                if (e.flags & F_TTL) memcpy(&exp, kv->arena + off + HDR, 8);
                if (expired(exp)) return 0;
                uint64_t now = lru_now();
                if (S_LRU(s) != now) /* at most once a second per key; CAS never clobbers a changed slot */
                    __atomic_compare_exchange_n((uint64_t *)&ix[p], &s, S_WITH_LRU(s, now), 0,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
                out->v = kv->arena + off + ko + kl;
                out->vl = e.vlen;
                out->blob = NULL;
                out->expire_ms = exp;
                out->type = (e.flags & F_OBJ) ? KV_T_OBJ : KV_T_STR;
                if (e.flags & F_BLOB) { /* pointer only: the caller validates its seqlock before using it */
                    memcpy(&out->blob, out->v, sizeof out->blob);
                    out->v = NULL, out->vl = 0;
                }
                return 1;
            }
        }
        p = (p + 1) & mask;
    }
    return -1;
}

void kv_prefetch(kv_t *kv, uint64_t h) { __builtin_prefetch(&kv->idx[HOME(kv->mask, h)]); }
void kv_prefetch_entry(kv_t *kv, uint64_t h)
{
    const uint64_t *ix = __atomic_load_n(&kv->idx, __ATOMIC_ACQUIRE);
    uint64_t s = __atomic_load_n(&ix[HOME(ix[-8], h)], __ATOMIC_RELAXED);
    if (s != EMPTY && S_TAG(s) == TAG_OF(h)) __builtin_prefetch(E(kv, REF(s)));
}

void kv_flush(kv_t *kv)
{
    for (uint64_t i = 0; i <= kv->mask; i++)
        if (kv->idx[i] != EMPTY) drop_value(kv, E(kv, REF(kv->idx[i])));
    memset(kv->idx, 0, (kv->mask + 1) * 8);
    memset(kv->free_head, 0, sizeof kv->free_head);
    memset(kv->cls_page_left, 0, sizeof kv->cls_page_left);
    madvise(kv->arena + PAGE_SMALL, kv->arena_top - PAGE_SMALL, MADV_DONTNEED);
    kv->arena_top = PAGE_SMALL;
    kv->count = kv->used_bytes = kv->slabs_bytes = kv->blob_bytes = 0;
}

void kv_info(kv_t *kv, kv_info_t *o)
{
    *o = (kv_info_t){.count = kv->count,
                     .used_bytes = kv->used_bytes + kv->blob_bytes,
                     .blob_bytes = kv->blob_bytes,
                     .arena_bytes = kv->arena_bytes,
                     .index_slots = kv->mask + 1,
                     .slabs_bytes = kv->slabs_bytes,
                     .evictions = kv->evictions,
                     .expired = kv->expired,
                     .limit_bytes = kv->limit - PAGE_SMALL};
}
