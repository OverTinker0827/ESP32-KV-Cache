/* kvpi engine v0: a straight Linux port of the ESP32 kv_opt design.
 *  - one mmap'd arena per shard, carved into 1 MiB slab pages; slab classes; per-class free lists;
 *    no malloc on the request path (same as ESP: pages -> slab classes -> chunks)
 *  - index: open addressing, linear probing, 8 B slot [tag:16 | ref:48] (ESP: 4 B [tag:11|ref:21]),
 *    ref = arena offset / 8 (0 = empty); backward-shift delete; in-place overwrite when the class fits
 *  - entry: [vlen u32 | klen u8 | cls u8 | flags u16] [key] [value]
 * Compile-time knobs (each is one measured step in rpi/results.md):
 *  KV_HASH_CRC   hardware CRC32C hash instead of FNV-1a + fmix (ESP hash)
 */
#define _GNU_SOURCE
#include "engine.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if KV_HASH_CRC
#include <arm_acle.h>
#endif

#define SLAB_PAGE (1u << 20)
#define HDR 8
#define MAXCLS 96
#define EMPTY 0

typedef struct {
    uint32_t vlen;
    uint8_t klen, cls;
    uint16_t flags;
} ehdr_t;

struct kv {
    uint64_t *idx;
    uint64_t mask, count;
    uint8_t *arena;
    size_t arena_bytes, arena_top; /* bump pointer for new slab pages */
    uint32_t ncls;
    uint32_t cls_size[MAXCLS];
    uint64_t free_head[MAXCLS]; /* arena offset of first free chunk, 0 = none */
    uint64_t cls_page_off[MAXCLS], cls_page_left[MAXCLS]; /* chunk carving inside current page */
    uint64_t used_bytes, slabs_bytes;
    int shared; /* readers on other threads: keep retired indexes mapped */
};

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
    h ^= h >> 33, h *= 0xff51afd7ed558ccdull, h ^= h >> 33; /* crc is linear: finalise */
    return h;
#else
    /* ESP engine hash: FNV-1a 32 then murmur3 fmix32, widened to 64 bits with a second fmix */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ k[i]) * 16777619u;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    uint64_t x = h * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return x ^ ((uint64_t)h << 32);
#endif
}

#define TAG_OF(h) ((h) >> 48)
#define HOME(kv, h) (((h) >> 8) & (kv)->mask)
#define SLOT(tag, ref) (((uint64_t)(tag) << 48) | (ref))
#define REF(s) ((s) & 0xFFFFFFFFFFFFull)
#define E(kv, ref) ((ehdr_t *)((kv)->arena + ((ref) << 3)))

/* ---------------------------------------------------------------- slabs */
static void init_classes(kv_t *kv)
{
    uint32_t s = 16, n = 0;
    while (s < SLAB_PAGE && n < MAXCLS) {
        kv->cls_size[n++] = s;
        uint32_t nx = s < 128 ? s + 8 : (s + s / 8 + 7) & ~7u;
        s = nx;
    }
    if (n < MAXCLS) kv->cls_size[n++] = SLAB_PAGE;
    kv->ncls = n;
}

static int cls_for(kv_t *kv, size_t bytes)
{
    /* small sizes are linear; binary search the rest */
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

static uint64_t chunk_alloc(kv_t *kv, int c)
{
    uint64_t off = kv->free_head[c];
    if (off) {
        kv->free_head[c] = *(uint64_t *)(kv->arena + off);
        return off;
    }
    uint32_t sz = kv->cls_size[c];
    if (kv->cls_page_left[c] < sz) {
        if (kv->arena_top + SLAB_PAGE > kv->arena_bytes) return 0; /* v0: no eviction yet */
        kv->cls_page_off[c] = kv->arena_top;
        kv->cls_page_left[c] = SLAB_PAGE;
        kv->arena_top += SLAB_PAGE;
        kv->slabs_bytes += SLAB_PAGE;
    }
    off = kv->cls_page_off[c];
    kv->cls_page_off[c] += sz;
    kv->cls_page_left[c] -= sz;
    return off;
}

static void chunk_free(kv_t *kv, uint64_t off, int c)
{
    *(uint64_t *)(kv->arena + off) = kv->free_head[c];
    kv->free_head[c] = off;
}

/* ---------------------------------------------------------------- index */
/* index block: 64 B header (slot[-8] = mask) then the slots, so a reader that loads the index
 * pointer once also gets the matching mask (no torn idx/mask pair during a grow) */
static uint64_t *idx_alloc(uint64_t nslots)
{
    void *p = mmap(NULL, nslots * 8 + 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    uint64_t *s = (uint64_t *)p + 8;
    s[-8] = nslots - 1;
    return s;
}
static void idx_free(uint64_t *s) { munmap(s - 8, (s[-8] + 1) * 8 + 64); }

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
        uint64_t h = kv_hash((uint8_t *)e + HDR, e->klen);
        uint64_t p = (h >> 8) & nmask;
        while (nw[p] != EMPTY) p = (p + 1) & nmask;
        nw[p] = s;
    }
    __atomic_store_n(&kv->idx, nw, __ATOMIC_RELEASE); /* publish the filled index */
    kv->mask = nmask;
    if (!kv->shared) idx_free(old); /* shared: a reader may still be probing it (bounded leak <= final size) */
}

static inline int key_eq(ehdr_t *e, const void *k, size_t kl)
{
    return e->klen == kl && memcmp((uint8_t *)e + HDR, k, kl) == 0;
}

/* position of the key, or -(first empty position) - 1 */
static inline int64_t find(kv_t *kv, const void *k, size_t kl, uint64_t h)
{
    uint64_t p = HOME(kv, h), tag = TAG_OF(h);
    for (;;) {
        uint64_t s = kv->idx[p];
        if (s == EMPTY) return -(int64_t)p - 1;
        if ((s >> 48) == tag && key_eq(E(kv, REF(s)), k, kl)) return (int64_t)p;
        p = (p + 1) & kv->mask;
    }
}

/* ---------------------------------------------------------------- API */
kv_t *kv_create(size_t arena_bytes, size_t expected_keys)
{
    kv_t *kv = calloc(1, sizeof *kv);
    if (!kv) return NULL;
    init_classes(kv);
    kv->arena_bytes = arena_bytes;
    kv->arena = mmap(NULL, arena_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (kv->arena == MAP_FAILED) return free(kv), NULL;
    kv->arena_top = SLAB_PAGE; /* offset 0 is "empty"; burn the first page so refs are never 0 */
    uint64_t n = 1024;
    while (n * 3 / 4 < expected_keys) n *= 2;
    kv->idx = idx_alloc(n);
    kv->mask = n - 1;
    return kv;
}

void kv_destroy(kv_t *kv)
{
    idx_free(kv->idx);
    munmap(kv->arena, kv->arena_bytes);
    free(kv);
}

int kv_set(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t h)
{
    if (kl == 0 || kl > 255 || vl > 0xFFFFFFFFu) return -1;
    size_t need = HDR + kl + vl;
    int c = cls_for(kv, need);
    if (c < 0) return -1;
    int64_t pos = find(kv, k, kl, h);
    if (pos >= 0) {
        uint64_t ref = REF(kv->idx[pos]);
        ehdr_t *e = E(kv, ref);
        if (e->cls == c) { /* in-place overwrite */
            kv->used_bytes += vl, kv->used_bytes -= e->vlen;
            e->vlen = (uint32_t)vl;
            memcpy((uint8_t *)e + HDR + kl, v, vl);
            return 0;
        }
        uint64_t off = chunk_alloc(kv, c);
        if (!off) return -1;
        ehdr_t *n = (ehdr_t *)(kv->arena + off);
        *n = (ehdr_t){.vlen = (uint32_t)vl, .klen = (uint8_t)kl, .cls = (uint8_t)c};
        memcpy((uint8_t *)n + HDR, k, kl);
        memcpy((uint8_t *)n + HDR + kl, v, vl);
        kv->used_bytes += need, kv->used_bytes -= HDR + e->klen + e->vlen;
        chunk_free(kv, ref << 3, e->cls);
        kv->idx[pos] = SLOT(TAG_OF(h), off >> 3);
        return 0;
    }
    if ((kv->count + 1) * 4 > (kv->mask + 1) * 3) {
        idx_grow(kv);
        pos = find(kv, k, kl, h);
    }
    uint64_t off = chunk_alloc(kv, c);
    if (!off) return -1;
    ehdr_t *n = (ehdr_t *)(kv->arena + off);
    *n = (ehdr_t){.vlen = (uint32_t)vl, .klen = (uint8_t)kl, .cls = (uint8_t)c};
    memcpy((uint8_t *)n + HDR, k, kl);
    memcpy((uint8_t *)n + HDR + kl, v, vl);
    kv->idx[-pos - 1] = SLOT(TAG_OF(h), off >> 3);
    kv->count++;
    kv->used_bytes += need;
    return 0;
}

int kv_get(kv_t *kv, const void *k, size_t kl, uint64_t h, const uint8_t **v, size_t *vl)
{
    int64_t pos = find(kv, k, kl, h);
    if (pos < 0) return 0;
    ehdr_t *e = E(kv, REF(kv->idx[pos]));
    *v = (uint8_t *)e + HDR + e->klen;
    *vl = e->vlen;
    return 1;
}

int kv_del(kv_t *kv, const void *k, size_t kl, uint64_t h)
{
    int64_t pos = find(kv, k, kl, h);
    if (pos < 0) return 0;
    uint64_t ref = REF(kv->idx[pos]);
    ehdr_t *e = E(kv, ref);
    kv->used_bytes -= HDR + e->klen + e->vlen;
    chunk_free(kv, ref << 3, e->cls);
    kv->count--;
    /* backward-shift delete (as on the ESP) */
    uint64_t i = (uint64_t)pos, j = i;
    for (;;) {
        j = (j + 1) & kv->mask;
        uint64_t s = kv->idx[j];
        if (s == EMPTY) break;
        ehdr_t *ej = E(kv, REF(s));
        uint64_t home = HOME(kv, kv_hash((uint8_t *)ej + HDR, ej->klen));
        /* move s back to i if its home is not cyclically in (i, j] */
        if (((j - home) & kv->mask) >= ((j - i) & kv->mask)) {
            kv->idx[i] = s;
            i = j;
        }
    }
    kv->idx[i] = EMPTY;
    return 1;
}

void kv_set_shared(kv_t *kv, int on) { kv->shared = on; }

/* Reader for optimistic (seqlock) access from another thread: may race with a writer, so it uses
 * one snapshot of the index, never trusts a length that would leave the arena, and returns -1 when
 * what it read is inconsistent (caller retries). The caller validates the stripe sequence after
 * copying the value out. Arena memory is never unmapped while running, so stale reads cannot fault. */
int kv_get_rd(kv_t *kv, const void *k, size_t kl, uint64_t h, const uint8_t **v, size_t *vl)
{
    const uint64_t *ix = __atomic_load_n(&kv->idx, __ATOMIC_ACQUIRE);
    uint64_t mask = ix[-8], p = (h >> 8) & mask, tag = TAG_OF(h);
    for (uint64_t n = 0; n <= mask; n++) {
        uint64_t s = __atomic_load_n(&ix[p], __ATOMIC_RELAXED);
        if (s == EMPTY) return 0;
        if ((s >> 48) == tag) {
            uint64_t off = REF(s) << 3;
            if (off + HDR > kv->arena_bytes) return -1;
            ehdr_t e;
            memcpy(&e, kv->arena + off, sizeof e);
            if (off + HDR + e.klen + (uint64_t)e.vlen > kv->arena_bytes) return -1;
            if (e.klen == kl && memcmp(kv->arena + off + HDR, k, kl) == 0) {
                *v = kv->arena + off + HDR + kl;
                *vl = e.vlen;
                return 1;
            }
        }
        p = (p + 1) & mask;
    }
    return -1;
}

void kv_prefetch(kv_t *kv, uint64_t h) { __builtin_prefetch(&kv->idx[HOME(kv, h)]); }
/* second prefetch stage: the home slot should be in cache now; prefetch the entry it points to */
void kv_prefetch_entry(kv_t *kv, uint64_t h)
{
    const uint64_t *ix = __atomic_load_n(&kv->idx, __ATOMIC_ACQUIRE);
    uint64_t s = __atomic_load_n(&ix[(h >> 8) & ix[-8]], __ATOMIC_RELAXED);
    if (s != EMPTY && (s >> 48) == TAG_OF(h)) __builtin_prefetch(E(kv, REF(s)));
}

void kv_flush(kv_t *kv)
{
    memset(kv->idx, 0, (kv->mask + 1) * 8);
    memset(kv->free_head, 0, sizeof kv->free_head);
    memset(kv->cls_page_left, 0, sizeof kv->cls_page_left);
    madvise(kv->arena + SLAB_PAGE, kv->arena_top - SLAB_PAGE, MADV_DONTNEED);
    kv->arena_top = SLAB_PAGE;
    kv->count = kv->used_bytes = kv->slabs_bytes = 0;
}

void kv_info(kv_t *kv, kv_info_t *o)
{
    *o = (kv_info_t){.count = kv->count,
                     .used_bytes = kv->used_bytes,
                     .arena_bytes = kv->arena_bytes,
                     .index_slots = kv->mask + 1,
                     .slabs_bytes = kv->slabs_bytes};
}
