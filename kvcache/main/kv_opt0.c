/* FROZEN copy of the optimized engine exactly as in Entry 2 (git e82bda2), for isolating
 * engine-code vs firmware effects in Entry 6. Select with CONFIG_KV_ENGINE_OPT0. */
/* Optimized engine, designed around the ESP32's constraints.
 *
 * Memory
 *  - The store is carved from the heap ONCE (after WiFi/lwIP are up) as 4 KiB pages.
 *    No malloc/free on the request path -> no heap fragmentation, no allocator locks.
 *  - Pages are assigned on demand to slab classes (8-byte granular at small sizes).
 *    Entry = 8 B header + key + value, stored inline in one chunk.
 *  - Index: open addressing, linear probing, 4 B per slot:
 *        [ tag:11 | ref:21 ]   ref = page:12 | chunk-offset/8:9   (0 = empty)
 *    The 11-bit tag rejects ~all wrong slots without touching the entry.
 *    Slot = fastrange(hash, nslots) (one MULUH on Xtensa) so the table can be sized
 *    exactly instead of rounding to a power of two.
 *  - Backward-shift deletion (no tombstones): probe lengths stay short forever.
 *  - In-place overwrite when the new value fits the same slab class (sensor updates).
 * Eviction: CLOCK (1 ref bit per entry) within the slab class; if a class has no pages,
 *  a page is stolen from the largest class (fixes slab calcification).
 * Expiry: lazy on access + incremental active sweep from the event loop when idle.
 * Concurrency: 1 shard (single / shared modes) or 2 shards (sharded), each with an
 *  optional portMUX spinlock. */
#include <stdio.h>
#include "kv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "kv_opt";

#if CONFIG_KV_CORES_SHARDED
#define NSHARDS 2
#else
#define NSHARDS 1
#endif

#if CONFIG_KV_CORES_SHARED || CONFIG_KV_CORES_SHARDED || CONFIG_KV_SERVER_BASIC || CONFIG_KV_FORCE_LOCK
#define NEED_LOCK 1
#define SH_LOCK(sh) portENTER_CRITICAL(&(sh)->mux)
#define SH_UNLOCK(sh) portEXIT_CRITICAL(&(sh)->mux)
#else
#define NEED_LOCK 0
#define SH_LOCK(sh) ((void)0)
#define SH_UNLOCK(sh) ((void)0)
#endif

#define PAGE_SZ 4096
#define PAGE_HDR 8
#define MAX_PAGES 256 /* 1 MiB; raise for PSRAM builds (ref supports 4095) */
#define REF_BITS 21
#define REF_MASK ((1u << REF_BITS) - 1)
#define TAG_OF(h) ((h) & 0x7FFu)
#define SLOT(tag, ref) (((uint32_t)(tag) << REF_BITS) | (ref))
#define MAX_KEY 255
#define MAX_VAL 65535
#define AVG_ENTRY_EST 48 /* bytes/entry used to size the index */
#define MAX_LOAD_NUM 9   /* index max load 0.9 */
#define MAX_LOAD_DEN 10

/* entry header, 8 bytes, chunk-aligned */
typedef struct {
    uint32_t expire; /* tick deadline (0 = none); next-free ref when free */
    uint16_t vlen;
    uint8_t klen;
    uint8_t flags;
} ehdr_t;
#define F_USED 1
#define F_REF 2   /* CLOCK reference bit */
#define F_LARGE 4 /* value lives in a separate heap block, pointer stored after key */

typedef struct {
    uint8_t cls;
    uint8_t shard;
    uint16_t used;
    uint16_t next; /* next page in class list / free list */
    uint16_t nchunks;
} phdr_t;

static const uint16_t cls_size[] = {16,  24,  32,  40,  48,  56,  64,  72,  80,  96,   112,  128,  160, 192,
                                    224, 256, 320, 384, 448, 512, 640, 768, 1016, 1360, 2040, 4088};
#define NCLS (sizeof(cls_size) / sizeof(cls_size[0]))
#define CLS_NONE 0xFF

typedef struct {
    uint32_t *idx;
    uint32_t nslots, count, max_count;
    uint32_t free_head[NCLS];
    uint16_t cls_pages[NCLS]; /* head of class page list */
    uint16_t npages_cls[NCLS];
    uint16_t hand_page[NCLS], hand_chunk[NCLS];
    uint16_t free_pages;
    uint32_t used_bytes, evictions, expired, exp_cursor;
    uint32_t large_bytes;
#if NEED_LOCK
    portMUX_TYPE mux;
#endif
} shard_t;

static shard_t shards[NSHARDS];
static uint8_t *pages[MAX_PAGES]; /* page 0 unused so that ref != 0 */
static uint16_t npages;
static void *blocks[16];
static int nblocks;
static int evict_on = 1;
static uint32_t index_bytes;

const char *kv_engine_name(void)
{
#if CONFIG_KV_CORES_SHARDED
    return "opt-slab-oa-sharded2";
#elif NEED_LOCK
    return "opt-slab-oa-locked";
#else
    return "opt-slab-oa";
#endif
}

/* ---------------------------------------------------------------- hashing */
static inline HOT uint32_t hash32(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u; /* FNV-1a ... */
    while (n >= 4) {
        h = (h ^ p[0]) * 16777619u;
        h = (h ^ p[1]) * 16777619u;
        h = (h ^ p[2]) * 16777619u;
        h = (h ^ p[3]) * 16777619u;
        p += 4;
        n -= 4;
    }
    while (n--) h = (h ^ *p++) * 16777619u;
    h ^= h >> 16; /* ... + murmur3 fmix32: full avalanche, fixes sequential-key clustering */
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static inline HOT uint32_t home(const shard_t *sh, uint32_t h)
{
    return (uint32_t)(((uint64_t)h * sh->nslots) >> 32);
}

static inline HOT shard_t *shard_of(uint32_t h)
{
#if NSHARDS > 1
    return &shards[(h >> 11) & 1];
#else
    (void)h;
    return &shards[0];
#endif
}

/* --------------------------------------------------------------- pages/refs */
static inline HOT ehdr_t *ref2e(uint32_t ref) { return (ehdr_t *)(pages[ref >> 9] + ((ref & 511) << 3)); }
static inline phdr_t *page_hdr(uint16_t pg) { return (phdr_t *)pages[pg]; }
static inline uint32_t mkref(uint16_t pg, uint32_t off) { return ((uint32_t)pg << 9) | (off >> 3); }
static inline uint8_t *ekey(ehdr_t *e) { return (uint8_t *)(e + 1); }

static inline HOT const uint8_t *eval(ehdr_t *e)
{
    if (unlikely(e->flags & F_LARGE)) {
        const uint8_t *p;
        memcpy(&p, ekey(e) + e->klen, sizeof p);
        return p;
    }
    return ekey(e) + e->klen;
}

static int cls_for(size_t total)
{
    for (int c = 0; c < (int)NCLS; c++)
        if (cls_size[c] >= total) return c;
    return -1;
}

static void page_assign(shard_t *sh, uint16_t pg, int cls)
{
    phdr_t *ph = page_hdr(pg);
    ph->cls = cls;
    ph->used = 0;
    ph->nchunks = (PAGE_SZ - PAGE_HDR) / cls_size[cls];
    ph->next = sh->cls_pages[cls];
    sh->cls_pages[cls] = pg;
    sh->npages_cls[cls]++;
    /* thread all chunks onto the class free list (last chunk first so allocation walks forward) */
    for (int i = ph->nchunks - 1; i >= 0; i--) {
        uint32_t off = PAGE_HDR + i * cls_size[cls];
        ehdr_t *e = (ehdr_t *)(pages[pg] + off);
        e->flags = 0;
        e->expire = sh->free_head[cls];
        sh->free_head[cls] = mkref(pg, off);
    }
}

/* ------------------------------------------------------------------ index */
static HOT ehdr_t *lookup(shard_t *sh, const uint8_t *k, size_t kl, uint32_t h, uint32_t *pos)
{
    uint32_t i = home(sh, h), tag = TAG_OF(h);
    uint32_t *idx = sh->idx, n = sh->nslots;
    for (;;) {
        uint32_t s = idx[i];
        if (s == 0) {
            *pos = i;
            return NULL;
        }
        if ((s >> REF_BITS) == tag) {
            ehdr_t *e = ref2e(s & REF_MASK);
            if (e->klen == kl && memcmp(ekey(e), k, kl) == 0) {
                *pos = i;
                return e;
            }
        }
        if (++i == n) i = 0;
    }
}

/* backward-shift delete of slot i */
static void slot_delete(shard_t *sh, uint32_t i)
{
    uint32_t *idx = sh->idx, n = sh->nslots, j = i;
    for (;;) {
        if (++j == n) j = 0;
        uint32_t s = idx[j];
        if (s == 0) break;
        ehdr_t *e = ref2e(s & REF_MASK);
        uint32_t hp = home(sh, hash32(ekey(e), e->klen));
        /* move s back to i unless its home lies cyclically in (i, j] */
        int stay = (i <= j) ? (hp > i && hp <= j) : (hp > i || hp <= j);
        if (!stay) {
            idx[i] = s;
            i = j;
        }
    }
    idx[i] = 0;
    sh->count--;
}

static void chunk_free(shard_t *sh, uint32_t ref)
{
    ehdr_t *e = ref2e(ref);
    uint16_t pg = ref >> 9;
    phdr_t *ph = page_hdr(pg);
    if (e->flags & F_LARGE) {
        void *p;
        memcpy(&p, ekey(e) + e->klen, sizeof p);
        sh->large_bytes -= e->vlen;
        heap_caps_free(p);
    }
    sh->used_bytes -= cls_size[ph->cls];
    ph->used--;
    e->flags = 0;
    e->expire = sh->free_head[ph->cls];
    sh->free_head[ph->cls] = ref;
}

/* remove entry (by its ref) from index and free its chunk */
static void entry_remove(shard_t *sh, uint32_t ref)
{
    ehdr_t *e = ref2e(ref);
    uint32_t h = hash32(ekey(e), e->klen);
    uint32_t i = home(sh, h);
    while ((sh->idx[i] & REF_MASK) != ref) {
        if (++i == sh->nslots) i = 0;
    }
    slot_delete(sh, i);
    chunk_free(sh, ref);
}

/* CLOCK sweep inside one class. Returns 1 if an entry was evicted. */
static int evict_in_class(shard_t *sh, int cls, uint32_t now)
{
    if (!sh->cls_pages[cls]) return 0;
    uint16_t pg = sh->hand_page[cls];
    uint16_t ci = sh->hand_chunk[cls];
    if (!pg || page_hdr(pg)->cls != cls) {
        pg = sh->cls_pages[cls];
        ci = 0;
    }
    uint32_t limit = 2u * sh->npages_cls[cls] * ((PAGE_SZ - PAGE_HDR) / cls_size[cls]) + 2;
    while (limit--) {
        phdr_t *ph = page_hdr(pg);
        if (ci >= ph->nchunks) {
            pg = ph->next ? ph->next : sh->cls_pages[cls];
            ci = 0;
            continue;
        }
        uint32_t off = PAGE_HDR + ci * cls_size[cls];
        ehdr_t *e = (ehdr_t *)(pages[pg] + off);
        ci++;
        if (!(e->flags & F_USED)) continue;
        if ((e->flags & F_REF) && !kv_expired(e->expire, now)) {
            e->flags &= ~F_REF; /* second chance */
            continue;
        }
        if (kv_expired(e->expire, now)) sh->expired++;
        else sh->evictions++;
        entry_remove(sh, mkref(pg, off));
        sh->hand_page[cls] = pg;
        sh->hand_chunk[cls] = ci;
        return 1;
    }
    return 0;
}

/* Steal a page from the class holding the most pages: evict its entries, return it. */
static uint16_t steal_page(shard_t *sh, int want_cls)
{
    int victim = -1;
    for (int c = 0; c < (int)NCLS; c++)
        if (c != want_cls && sh->npages_cls[c] > 0 && (victim < 0 || sh->npages_cls[c] > sh->npages_cls[victim]))
            victim = c;
    if (victim < 0) return 0;
    uint16_t pg = sh->cls_pages[victim];
    phdr_t *ph = page_hdr(pg);
    uint16_t sz = cls_size[victim];
    for (int i = 0; i < ph->nchunks; i++) {
        ehdr_t *e = (ehdr_t *)(pages[pg] + PAGE_HDR + i * sz);
        if (e->flags & F_USED) {
            sh->evictions++;
            entry_remove(sh, mkref(pg, PAGE_HDR + i * sz));
        }
    }
    /* unlink this page's chunks from the class free list */
    uint32_t *pp = &sh->free_head[victim];
    while (*pp) {
        if ((*pp >> 9) == pg) *pp = ref2e(*pp)->expire;
        else pp = &ref2e(*pp)->expire;
    }
    sh->cls_pages[victim] = ph->next;
    sh->npages_cls[victim]--;
    if (sh->hand_page[victim] == pg) sh->hand_page[victim] = 0;
    return pg;
}

static uint32_t chunk_alloc(shard_t *sh, int cls, uint32_t now)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t ref = sh->free_head[cls];
        if (ref) {
            ehdr_t *e = ref2e(ref);
            sh->free_head[cls] = e->expire;
            page_hdr(ref >> 9)->used++;
            sh->used_bytes += cls_size[cls];
            return ref;
        }
        if (sh->free_pages) {
            uint16_t pg = sh->free_pages;
            sh->free_pages = page_hdr(pg)->next;
            page_assign(sh, pg, cls);
            continue;
        }
        if (!evict_on) return 0;
        if (evict_in_class(sh, cls, now)) continue;
        uint16_t pg = steal_page(sh, cls);
        if (!pg) return 0;
        page_assign(sh, pg, cls);
    }
    return 0;
}

/* --------------------------------------------------------------- public API */
static void shard_reset(shard_t *sh)
{
    memset(sh->idx, 0, sh->nslots * sizeof(uint32_t));
    sh->count = 0;
    memset(sh->free_head, 0, sizeof sh->free_head);
    memset(sh->cls_pages, 0, sizeof sh->cls_pages);
    memset(sh->npages_cls, 0, sizeof sh->npages_cls);
    memset(sh->hand_page, 0, sizeof sh->hand_page);
    memset(sh->hand_chunk, 0, sizeof sh->hand_chunk);
    sh->free_pages = 0;
    sh->used_bytes = sh->evictions = sh->expired = sh->exp_cursor = sh->large_bytes = 0;
}

static void distribute_pages(void)
{
    for (uint16_t pg = npages; pg >= 1; pg--) {
        shard_t *sh = &shards[pg % NSHARDS];
        phdr_t *ph = page_hdr(pg);
        ph->cls = CLS_NONE;
        ph->shard = pg % NSHARDS;
        ph->next = sh->free_pages;
        sh->free_pages = pg;
    }
}

int kv_init(size_t reserve)
{
    const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
    size_t free0 = heap_caps_get_free_size(caps);
    if (free0 < reserve + 2 * PAGE_SZ) return -1;
    size_t avail = free0 - reserve;
    uint32_t est_entries = avail / AVG_ENTRY_EST;
    uint32_t slots_total = est_entries * MAX_LOAD_DEN / MAX_LOAD_NUM + 16;
    index_bytes = 0;
    for (int s = 0; s < NSHARDS; s++) {
        shard_t *sh = &shards[s];
        sh->nslots = slots_total / NSHARDS;
        sh->max_count = sh->nslots * MAX_LOAD_NUM / MAX_LOAD_DEN;
        sh->idx = heap_caps_malloc(sh->nslots * sizeof(uint32_t), caps);
        if (!sh->idx) return -1;
        index_bytes += sh->nslots * sizeof(uint32_t);
#if NEED_LOCK
        portMUX_INITIALIZE(&sh->mux);
#endif
    }
    /* carve pages from the largest free blocks until only `reserve` is left */
    npages = 0;
    nblocks = 0;
    while (nblocks < 16 && npages < MAX_PAGES - 1) {
        size_t freeb = heap_caps_get_free_size(caps);
        size_t largest = heap_caps_get_largest_free_block(caps);
        if (freeb <= reserve + PAGE_SZ) break;
        size_t budget = freeb - reserve;
        size_t take = (largest < budget ? largest : budget);
        take = (take > 64 ? take - 64 : 0) / PAGE_SZ * PAGE_SZ; /* allocator overhead */
        if (take > (size_t)(MAX_PAGES - 1 - npages) * PAGE_SZ) take = (MAX_PAGES - 1 - npages) * PAGE_SZ;
        if (take < PAGE_SZ) break;
        uint8_t *b = heap_caps_malloc(take, caps);
        if (!b) break;
        blocks[nblocks++] = b;
        for (size_t o = 0; o < take; o += PAGE_SZ) pages[++npages] = b + o;
    }
    for (int s = 0; s < NSHARDS; s++) shard_reset(&shards[s]);
    distribute_pages();
    ESP_LOGI(TAG, "arena %u pages (%u KiB) in %d blocks, index %u slots (%u B), %d shard(s), heap left %u",
             npages, npages * 4, nblocks, (unsigned)(slots_total), (unsigned)index_bytes, NSHARDS,
             (unsigned)heap_caps_get_free_size(caps));
    return npages ? 0 : -1;
}

void kv_deinit(void)
{
    for (int s = 0; s < NSHARDS; s++) {
        heap_caps_free(shards[s].idx);
        shards[s].idx = NULL;
    }
    for (int i = 0; i < nblocks; i++) heap_caps_free(blocks[i]);
    nblocks = 0;
    npages = 0;
}

void kv_flush(void)
{
    for (int s = 0; s < NSHARDS; s++) {
        shard_t *sh = &shards[s];
        SH_LOCK(sh);
        /* free large values first */
        for (uint32_t i = 0; i < sh->nslots; i++) {
            if (sh->idx[i]) {
                ehdr_t *e = ref2e(sh->idx[i] & REF_MASK);
                if (e->flags & F_LARGE) {
                    void *p;
                    memcpy(&p, ekey(e) + e->klen, sizeof p);
                    heap_caps_free(p);
                }
            }
        }
        shard_reset(sh);
        SH_UNLOCK(sh);
    }
    distribute_pages();
}

HOT int kv_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, uint32_t ttl_ms, int flags)
{
    if (unlikely(kl > MAX_KEY || vl > MAX_VAL)) return KV_ETOOBIG;
    uint32_t h = hash32(k, kl), now = kv_now_ms(), pos;
    shard_t *sh = shard_of(h);
    size_t total = sizeof(ehdr_t) + kl + vl;
    int large = 0;
    int cls = cls_for(total);
    if (cls < 0) {
        large = 1;
        cls = cls_for(sizeof(ehdr_t) + kl + sizeof(void *));
    }
    int rc = KV_OK;
    uint8_t *lp = NULL;
    if (large) { /* allocate outside the critical section */
        lp = heap_caps_malloc(vl, MALLOC_CAP_8BIT);
        if (!lp) return KV_ENOMEM;
        memcpy(lp, v, vl);
    }

    SH_LOCK(sh);
    ehdr_t *e = lookup(sh, k, kl, h, &pos);
    if (e && kv_expired(e->expire, now)) {
        entry_remove(sh, sh->idx[pos] & REF_MASK);
        sh->expired++;
        e = NULL;
    }
    if (((flags & KV_NX) && e) || ((flags & KV_XX) && !e)) {
        rc = KV_NOTSET;
        goto out;
    }
    uint32_t expire = ttl_ms ? ((now + ttl_ms) | 1) : ((flags & KV_KEEPTTL) && e ? e->expire : 0);

    /* fast path: overwrite in place when the new value fits the same slab class */
    if (e && !large && !(e->flags & F_LARGE) && page_hdr((sh->idx[pos] & REF_MASK) >> 9)->cls == cls) {
        memcpy(ekey(e) + kl, v, vl);
        e->vlen = vl;
        e->expire = expire;
        e->flags |= F_REF;
        goto out;
    }
    if (!e && sh->count >= sh->max_count) { /* index full -> evict first */
        if (!evict_on || !evict_in_class(sh, cls, now)) {
            int c2;
            for (c2 = 0; c2 < (int)NCLS && !(evict_on && evict_in_class(sh, c2, now)); c2++) {}
            if (c2 == (int)NCLS) {
                rc = KV_ENOMEM;
                goto out;
            }
        }
    }
    uint32_t ref = chunk_alloc(sh, cls, now);
    if (!ref) {
        rc = KV_ENOMEM;
        goto out;
    }
    ehdr_t *ne = ref2e(ref);
    ne->expire = expire;
    ne->klen = kl;
    ne->vlen = vl;
    ne->flags = F_USED | (large ? F_LARGE : 0);
    memcpy(ekey(ne), k, kl);
    if (large) {
        memcpy(ekey(ne) + kl, &lp, sizeof lp);
        sh->large_bytes += vl;
        lp = NULL;
    } else {
        memcpy(ekey(ne) + kl, v, vl);
    }
    /* chunk_alloc may have evicted/shifted entries: look the key up again */
    e = lookup(sh, k, kl, h, &pos);
    if (e) {
        uint32_t old = sh->idx[pos] & REF_MASK;
        sh->idx[pos] = SLOT(TAG_OF(h), ref);
        chunk_free(sh, old);
    } else {
        sh->idx[pos] = SLOT(TAG_OF(h), ref);
        sh->count++;
    }
out:
    SH_UNLOCK(sh);
    if (lp) heap_caps_free(lp);
    return rc;
}

static inline HOT int utoa10(char *out, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

HOT int kv_get_resp(const uint8_t *k, size_t kl, obuf_t *o)
{
    uint32_t h = hash32(k, kl), now = kv_now_ms(), pos;
    shard_t *sh = shard_of(h);
    int rc;
    SH_LOCK(sh);
    ehdr_t *e = lookup(sh, k, kl, h, &pos);
    if (e && kv_expired(e->expire, now)) {
        entry_remove(sh, sh->idx[pos] & REF_MASK);
        sh->expired++;
        e = NULL;
    }
    if (!e) {
        if (!ob_room(o, 5)) rc = KV_ENOROOM;
        else {
            memcpy(o->buf + o->len, "$-1\r\n", 5);
            o->len += 5;
            rc = KV_NOTFOUND;
        }
    } else if (!ob_room(o, 1 + 5 + 2 + e->vlen + 2)) {
        rc = KV_ENOROOM;
    } else {
        uint8_t *w = o->buf + o->len;
        *w++ = '$';
        w += utoa10((char *)w, e->vlen);
        *w++ = '\r';
        *w++ = '\n';
        memcpy(w, eval(e), e->vlen);
        w += e->vlen;
        *w++ = '\r';
        *w++ = '\n';
        o->len = w - o->buf;
        e->flags |= F_REF;
        rc = KV_OK;
    }
    SH_UNLOCK(sh);
    return rc;
}

/* shared helper for DEL/EXISTS/PTTL/EXPIRE: lookup with lazy expiry */
static ehdr_t *find_live(shard_t *sh, const uint8_t *k, size_t kl, uint32_t h, uint32_t now, uint32_t *pos)
{
    ehdr_t *e = lookup(sh, k, kl, h, pos);
    if (e && kv_expired(e->expire, now)) {
        entry_remove(sh, sh->idx[*pos] & REF_MASK);
        sh->expired++;
        return NULL;
    }
    return e;
}

HOT int kv_del(const uint8_t *k, size_t kl)
{
    uint32_t h = hash32(k, kl), now = kv_now_ms(), pos;
    shard_t *sh = shard_of(h);
    SH_LOCK(sh);
    ehdr_t *e = find_live(sh, k, kl, h, now, &pos);
    if (e) {
        uint32_t ref = sh->idx[pos] & REF_MASK;
        slot_delete(sh, pos);
        chunk_free(sh, ref);
    }
    SH_UNLOCK(sh);
    return e != NULL;
}

int kv_exists(const uint8_t *k, size_t kl)
{
    uint32_t h = hash32(k, kl), now = kv_now_ms(), pos;
    shard_t *sh = shard_of(h);
    SH_LOCK(sh);
    int r = find_live(sh, k, kl, h, now, &pos) != NULL;
    SH_UNLOCK(sh);
    return r;
}

int64_t kv_pttl(const uint8_t *k, size_t kl)
{
    uint32_t h = hash32(k, kl), now = kv_now_ms(), pos;
    shard_t *sh = shard_of(h);
    int64_t r;
    SH_LOCK(sh);
    ehdr_t *e = find_live(sh, k, kl, h, now, &pos);
    if (!e) r = -2;
    else if (!e->expire) r = -1;
    else r = (int32_t)(e->expire - now);
    SH_UNLOCK(sh);
    return r;
}

int kv_expire(const uint8_t *k, size_t kl, uint32_t ttl_ms)
{
    uint32_t h = hash32(k, kl), now = kv_now_ms(), pos;
    shard_t *sh = shard_of(h);
    SH_LOCK(sh);
    ehdr_t *e = find_live(sh, k, kl, h, now, &pos);
    if (e) e->expire = (now + ttl_ms) | 1;
    SH_UNLOCK(sh);
    return e != NULL;
}

void kv_active_expire(int budget)
{
    uint32_t now = kv_now_ms();
    for (int s = 0; s < NSHARDS; s++) {
        shard_t *sh = &shards[s];
        SH_LOCK(sh);
        for (int b = 0; b < budget; b++) {
            uint32_t i = sh->exp_cursor;
            uint32_t sl = sh->idx[i];
            if (sl) {
                ehdr_t *e = ref2e(sl & REF_MASK);
                if (kv_expired(e->expire, now)) {
                    entry_remove(sh, sl & REF_MASK);
                    sh->expired++;
                    continue; /* slot i now holds a shifted entry: re-check it */
                }
            }
            if (++sh->exp_cursor >= sh->nslots) sh->exp_cursor = 0;
        }
        SH_UNLOCK(sh);
    }
}

void kv_set_evict(int on) { evict_on = on; }
int kv_get_evict(void) { return evict_on; }

void kv_stats(kv_stats_t *st)
{
    memset(st, 0, sizeof *st);
    for (int s = 0; s < NSHARDS; s++) {
        shard_t *sh = &shards[s];
        SH_LOCK(sh);
        st->keys += sh->count;
        st->mem_used += sh->used_bytes + sh->large_bytes;
        st->evictions += sh->evictions;
        st->expired += sh->expired;
        st->index_slots += sh->nslots;
        st->mem_total += sh->large_bytes;
        SH_UNLOCK(sh);
    }
    st->mem_used += index_bytes * st->keys / (st->index_slots ? st->index_slots : 1);
    st->mem_total += (uint32_t)npages * PAGE_SZ + index_bytes;
    st->shards = NSHARDS;
}
