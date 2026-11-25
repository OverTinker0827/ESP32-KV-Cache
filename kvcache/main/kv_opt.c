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

/* One lock implementation for the server AND the multicore harness (they used to differ,
 * and the server kept designs the harness had ruled out). Shard count and lock discipline are
 * runtime settings: the server takes them from Kconfig at kv_init(); the harness sets them with
 * kv_mc_set_mode() before kv_init(). Single-owner mode (KV_LOCK_NONE) costs one predictable
 * branch per operation and takes no lock. */
#include "esp_cpu.h"
#define MAXSHARDS 8 /* power of two; shard = bits 11.. of the hash (independent of tag and home slot) */
static int g_nshards = 1;
static int g_lock = KV_LOCK_NONE;
static int g_mode_set; /* 1 once kv_mc_set_mode() chose explicitly (harness) */
#define NSH g_nshards
#define NEED_LOCK 1
#define SH_LOCK(sh) do { if (g_lock) mc_wr_lock(sh); } while (0)
#define SH_UNLOCK(sh) do { if (g_lock) mc_wr_unlock(sh); } while (0)
#define RD_LOCK(sh) do { if (g_lock) mc_rd_lock(sh); } while (0)
#define RD_UNLOCK(sh) do { if (g_lock) mc_rd_unlock(sh); } while (0)
/* may a reader modify the table (lazy-expiry removal)? not under shared/optimistic reads */
#define READER_MAY_WRITE (g_lock != KV_LOCK_RW && g_lock != KV_LOCK_SEQ)

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
    volatile uint32_t lk;  /* CAS spinlock word / RW lock word (bit31 = writer, low bits = readers) */
    volatile uint32_t seq; /* seqlock sequence: odd while a writer is inside */
} shard_t;

static shard_t shards[MAXSHARDS];

/* ------------------------------------------------ runtime locks + contention stats */
typedef struct {
    uint32_t acq, contended, retries;
    uint64_t spin_cycles;
    uint32_t pad[3]; /* keep the two cores' counters apart */
} lkstat_t;
static lkstat_t lkstat[2];
#define RW_W 0x80000000u

static inline HOT bool cas(volatile uint32_t *a, uint32_t expect, uint32_t nv)
{
    return esp_cpu_compare_and_set(a, expect, nv);
}

static inline HOT void cas_spin_lock(volatile uint32_t *l, lkstat_t *st)
{
    st->acq++;
    if (likely(cas(l, 0, 1))) return;
    st->contended++;
    uint32_t t0 = ccount();
    do {
        while (*l) {
        }
    } while (!cas(l, 0, 1));
    st->spin_cycles += ccount() - t0;
}

static inline HOT void cas_spin_unlock(volatile uint32_t *l)
{
    __sync_synchronize(); /* memw: all stores of the critical section visible first */
    *l = 0;
}

static HOT void mc_wr_lock(shard_t *sh)
{
    lkstat_t *st = &lkstat[xPortGetCoreID()];
    switch (g_lock) {
    case KV_LOCK_NONE:
        return;
    case KV_LOCK_MUX: {
        st->acq++;
        uint32_t t0 = ccount();
        portENTER_CRITICAL(&sh->mux);
        uint32_t dt = ccount() - t0;
        if (dt > 200) { /* uncontended enter is ~100 cycles; more means we spun */
            st->contended++;
            st->spin_cycles += dt;
        }
        return;
    }
    case KV_LOCK_CAS:
        cas_spin_lock(&sh->lk, st);
        return;
    case KV_LOCK_RW: {
        st->acq++;
        if (likely(cas(&sh->lk, 0, RW_W))) return;
        st->contended++;
        uint32_t t0 = ccount();
        while (!cas(&sh->lk, 0, RW_W)) {
        }
        st->spin_cycles += ccount() - t0;
        return;
    }
    case KV_LOCK_SEQ:
        cas_spin_lock(&sh->lk, st);
        sh->seq++; /* odd: readers will retry */
        __sync_synchronize();
        return;
    }
}

static HOT void mc_wr_unlock(shard_t *sh)
{
    switch (g_lock) {
    case KV_LOCK_NONE:
        return;
    case KV_LOCK_MUX:
        portEXIT_CRITICAL(&sh->mux);
        return;
    case KV_LOCK_CAS:
    case KV_LOCK_RW:
        cas_spin_unlock(&sh->lk);
        return;
    case KV_LOCK_SEQ:
        __sync_synchronize();
        sh->seq++; /* even again */
        cas_spin_unlock(&sh->lk);
        return;
    }
}

static HOT void mc_rd_lock(shard_t *sh)
{
    if (g_lock != KV_LOCK_RW) {
        mc_wr_lock(sh); /* SEQ readers do not come through here (see kv_get_resp) */
        return;
    }
    lkstat_t *st = &lkstat[xPortGetCoreID()];
    st->acq++;
    uint32_t v = sh->lk;
    if (likely(!(v & RW_W) && cas(&sh->lk, v, v + 1))) return;
    st->contended++;
    uint32_t t0 = ccount();
    for (;;) {
        v = sh->lk;
        if (!(v & RW_W) && cas(&sh->lk, v, v + 1)) break;
    }
    st->spin_cycles += ccount() - t0;
}

static HOT void mc_rd_unlock(shard_t *sh)
{
    if (g_lock != KV_LOCK_RW) {
        mc_wr_unlock(sh);
        return;
    }
    __sync_synchronize();
    for (;;) {
        uint32_t v = sh->lk;
        if (cas(&sh->lk, v, v - 1)) return;
    }
}
static uint8_t *pages[MAX_PAGES]; /* page 0 unused so that ref != 0 */
static uint16_t npages;
static void *blocks[16];
static int nblocks;
static int evict_on = 1;
static uint32_t index_bytes;

static void default_mode(void);

const char *kv_engine_name(void)
{
    static char name[40];
    static const char *ln[] = {"nolock", "mux", "cas", "rw", "seqlock"};
    default_mode();
    snprintf(name, sizeof name, "opt-slab-oa-%dsh-%s", g_nshards, ln[g_lock]);
    return name;
}

/* Server defaults (Phases 1-2): one core owns the table -> no lock; two cores -> 8 shards with
 * a seqlock (lock-free GETs, CAS-locked writers), which beat portMUX 0.96x -> 1.69x quiet and
 * kept 1.40x of one core under WiFi-like load. */
static void default_mode(void)
{
    if (g_mode_set) return;
#if CONFIG_KV_FORCE_LOCK
    g_nshards = 1, g_lock = KV_LOCK_MUX; /* Entry 2 ablation: cost of a portMUX lock */
#elif CONFIG_KV_SERVER_BASIC
    g_nshards = 1, g_lock = KV_LOCK_CAS; /* task per client: many tasks on both cores */
#elif CONFIG_KV_CORES_SHARED
    g_nshards = 1, g_lock = KV_LOCK_SEQ;
#elif CONFIG_KV_CORES_SHARDED
    g_nshards = 8, g_lock = KV_LOCK_SEQ;
#else
    g_nshards = 1, g_lock = KV_LOCK_NONE;
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
    return &shards[(h >> 11) & (uint32_t)(g_nshards - 1)];
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
        shard_t *sh = &shards[pg % NSH];
        phdr_t *ph = page_hdr(pg);
        ph->cls = CLS_NONE;
        ph->shard = pg % NSH;
        ph->next = sh->free_pages;
        sh->free_pages = pg;
    }
}

int kv_init(size_t reserve)
{
    default_mode();
    const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;
    size_t free0 = heap_caps_get_free_size(caps);
    if (free0 < reserve + 2 * PAGE_SZ) return -1;
    size_t avail = free0 - reserve;
    uint32_t est_entries = avail / AVG_ENTRY_EST;
    uint32_t slots_total = est_entries * MAX_LOAD_DEN / MAX_LOAD_NUM + 16;
    index_bytes = 0;
    for (int s = 0; s < NSH; s++) {
        shard_t *sh = &shards[s];
        sh->nslots = slots_total / NSH;
        sh->max_count = sh->nslots * MAX_LOAD_NUM / MAX_LOAD_DEN;
        sh->idx = heap_caps_malloc(sh->nslots * sizeof(uint32_t), caps);
        if (!sh->idx) return -1;
        index_bytes += sh->nslots * sizeof(uint32_t);
#if NEED_LOCK
        portMUX_INITIALIZE(&sh->mux);
#endif
        sh->lk = 0;
        sh->seq = 0;
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
    for (int s = 0; s < NSH; s++) shard_reset(&shards[s]);
    distribute_pages();
    ESP_LOGI(TAG, "arena %u pages (%u KiB) in %d blocks, index %u slots (%u B), %d shard(s), heap left %u",
             npages, npages * 4, nblocks, (unsigned)(slots_total), (unsigned)index_bytes, NSH,
             (unsigned)heap_caps_get_free_size(caps));
    return npages ? 0 : -1;
}

void kv_deinit(void)
{
    for (int s = 0; s < NSH; s++) {
        heap_caps_free(shards[s].idx);
        shards[s].idx = NULL;
    }
    for (int i = 0; i < nblocks; i++) heap_caps_free(blocks[i]);
    nblocks = 0;
    npages = 0;
}

void kv_flush(void)
{
    for (int s = 0; s < NSH; s++) {
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

static inline HOT int put_nil(obuf_t *o)
{
    if (!ob_room(o, 5)) return KV_ENOROOM;
    memcpy(o->buf + o->len, "$-1\r\n", 5);
    o->len += 5;
    return KV_NOTFOUND;
}

static inline HOT int put_val(obuf_t *o, const uint8_t *v, uint32_t vl)
{
    if (!ob_room(o, 1 + 5 + 2 + vl + 2)) return KV_ENOROOM;
    uint8_t *w = o->buf + o->len;
    *w++ = '$';
    w += utoa10((char *)w, vl);
    *w++ = '\r';
    *w++ = '\n';
    memcpy(w, v, vl);
    w += vl;
    *w++ = '\r';
    *w++ = '\n';
    o->len = w - o->buf;
    return KV_OK;
}

/* GET body, caller holds the read (or write) lock. may_write: allowed to remove expired entries */
static HOT int get_locked(shard_t *sh, const uint8_t *k, size_t kl, uint32_t h, uint32_t now, obuf_t *o,
                          int may_write)
{
    uint32_t pos;
    ehdr_t *e = lookup(sh, k, kl, h, &pos);
    if (e && kv_expired(e->expire, now)) {
        if (may_write) {
            entry_remove(sh, sh->idx[pos] & REF_MASK);
            sh->expired++;
        }
        e = NULL;
    }
    if (!e) return put_nil(o);
    int rc = put_val(o, eval(e), e->vlen);
    if (rc == KV_OK) e->flags |= F_REF; /* concurrent readers only ever set this bit */
    return rc;
}

#define OPT_RETRY (-100)
/* Optimistic (seqlock) read: runs concurrently with a writer, so every field read from the
 * table may be torn. Everything is bounds-checked; the caller validates the sequence number
 * afterwards and discards the result on any change. Never writes to the table (so no CLOCK
 * reference bit for optimistic hits: documented limitation). */
static HOT int get_optimistic(shard_t *sh, const uint8_t *k, size_t kl, uint32_t h, uint32_t now, obuf_t *o)
{
    uint32_t i = home(sh, h), tag = TAG_OF(h), n = sh->nslots;
    for (uint32_t probes = 0; probes < n; probes++) {
        uint32_t s = sh->idx[i];
        if (s == 0) return put_nil(o);
        if ((s >> REF_BITS) == tag) {
            uint32_t ref = s & REF_MASK, pg = ref >> 9;
            if (pg == 0 || pg > npages) return OPT_RETRY;
            ehdr_t *e = ref2e(ref);
            uint32_t ekl = e->klen, vl = e->vlen, fl = e->flags, c = page_hdr(pg)->cls;
            if (c >= NCLS || sizeof(ehdr_t) + ekl + vl > cls_size[c]) return OPT_RETRY; /* torn or large */
            if (ekl == kl && memcmp(ekey(e), k, kl) == 0) {
                if (!(fl & F_USED)) return OPT_RETRY;
                if (kv_expired(e->expire, now)) return put_nil(o);
                return put_val(o, ekey(e) + kl, vl);
            }
        }
        if (++i == n) i = 0;
    }
    return OPT_RETRY;
}

HOT int kv_get_resp(const uint8_t *k, size_t kl, obuf_t *o)
{
    uint32_t h = hash32(k, kl), now = kv_now_ms();
    shard_t *sh = shard_of(h);
    int rc;
    if (g_lock == KV_LOCK_SEQ) {
        lkstat_t *st = &lkstat[xPortGetCoreID()];
        size_t len0 = o->len;
        st->acq++;
        for (int tries = 0; tries < 16; tries++) {
            uint32_t s1;
            if ((s1 = sh->seq) & 1) { /* a writer is inside: wait for it */
                uint32_t t0 = ccount();
                while ((s1 = sh->seq) & 1) {
                }
                st->spin_cycles += ccount() - t0;
                st->contended++;
            }
            __sync_synchronize();
            rc = get_optimistic(sh, k, kl, h, now, o);
            __sync_synchronize();
            if (sh->seq == s1) {
                if (rc != OPT_RETRY) return rc;
                break; /* consistent but not servable optimistically (large value): lock */
            }
            o->len = len0; /* a writer interleaved: discard and retry */
            st->retries++;
        }
        o->len = len0;
        mc_wr_lock(sh);
        rc = get_locked(sh, k, kl, h, now, o, 1);
        mc_wr_unlock(sh);
        return rc;
    }
    RD_LOCK(sh);
    rc = get_locked(sh, k, kl, h, now, o, READER_MAY_WRITE);
    RD_UNLOCK(sh);
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
    for (int s = 0; s < NSH; s++) {
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
    for (int s = 0; s < NSH; s++) {
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
    st->shards = NSH;
}

/* ------------------------------------------------------------ harness API */
void kv_mc_set_mode(int nshards, int lock_mode)
{
    int n = 1;
    while (n < nshards && n < MAXSHARDS) n <<= 1; /* round up to a power of two */
    g_nshards = n;
    g_lock = lock_mode;
    g_mode_set = 1;
}

int kv_shard_index(const uint8_t *k, size_t kl) { return (int)(shard_of(hash32(k, kl)) - shards); }

void kv_mc_stats(kv_lockstat_t out[2])
{
    for (int c = 0; c < 2; c++) {
        out[c].acq = lkstat[c].acq;
        out[c].contended = lkstat[c].contended;
        out[c].retries = lkstat[c].retries;
        out[c].spin_cycles = lkstat[c].spin_cycles;
    }
}

void kv_mc_reset_stats(void) { memset(lkstat, 0, sizeof lkstat); }
