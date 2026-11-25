/* Optimized engine v2 (Phase 3: memory), designed around the ESP32's constraints.
 * The Entry 5 version is frozen as kv_opt1.c for before/after comparisons.
 *
 * Memory
 *  - The store is a set of 4 KiB pages, each allocated on its own after WiFi/lwIP are up, so
 *    pages can later be returned to (or taken from) the heap one at a time (adaptive reserve).
 *    No malloc/free on the request path.
 *  - Pages are assigned on demand to slab classes (4-byte granular up to 64 B).
 *  - Entry layout (v2, compact):
 *      [hdr 4 B: vlen u16 | klen u8 | flags u8] [expire u32, only if F_TTL] [prefix id u8, only if
 *      F_PFX] [key, or its suffix if F_PFX] [next u32, only if F_CHAIN] [value, or its first part]
 *    v1 always spent 8 B of header (incl. expiry) per entry.
 *  - Values that do not fit the largest chunk are a chain of 4 KiB chunks INSIDE the arena
 *    (v1 put them in the heap reserve and failed with -OOM when it was fragmented).
 *  - Optional prefix compression: keys "a:b:<suffix>" store a 1-byte id of the shared prefix
 *    (up to 24 B) instead of the prefix bytes. Dictionary is append-only, 63 entries.
 *  - Index: open addressing, linear probing, 4 B per slot: [ tag:11 | ref:21 ],
 *    ref = page:11 | chunk-offset/4:10 (0 = empty); fastrange(hash, nslots); backward-shift delete.
 *  - In-place overwrite when the new entry fits the same slab class.
 * Eviction: CLOCK per slab class; page stealing from the largest class (slab calcification).
 * Expiry: lazy on access + incremental active sweep.
 * Concurrency: runtime shard count (power of two, <= 8) and lock discipline (none / portMUX / CAS /
 *  RW / seqlock), shared by the server and the multicore harness. */
#include <stdio.h>
#include "kv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "kv_opt";

#include "esp_cpu.h"
#define MAXSHARDS 8 /* power of two; shard = bits 11.. of the hash (independent of tag and home slot) */
/* Single-owner server builds fix shard count and lock mode at compile time so every lock check
 * compiles away; Entry 5 made them runtime everywhere and that cost ~12 % SET/s in the engine
 * benchmark (measured in Entry 6). The harness, the basic server and two-loop builds stay runtime. */
#if (CONFIG_KV_CORES_SINGLE || CONFIG_KV_CORES_SINGLE_CORE0) && !CONFIG_KV_MC_BENCH && !CONFIG_KV_SERVER_BASIC &&        !CONFIG_KV_FORCE_LOCK
#define FIXED_SINGLE 1
static const int g_nshards = 1;
static const int g_lock = KV_LOCK_NONE;
#else
#define FIXED_SINGLE 0
static int g_nshards = 1;
static int g_lock = KV_LOCK_NONE;
#endif
static int g_mode_set; /* 1 once kv_mc_set_mode() chose explicitly (harness) */
static int g_prefix;   /* prefix compression on/off (runtime, for measurement) */
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
#define MAX_PAGES 256 /* 1 MiB; ref supports 2047 (PSRAM builds) */
#define REF_BITS 21
#define REF_OFF_BITS 10 /* chunk offset / 4 */
#define REF_MASK ((1u << REF_BITS) - 1)
#define TAG_OF(h) ((h) & 0x7FFu)
#define SLOT(tag, ref) (((uint32_t)(tag) << REF_BITS) | (ref))
#define MAX_KEY 255
#define MAX_VAL 65535
#define AVG_ENTRY_EST 44 /* bytes/entry used to size the index */
#define MAX_LOAD_NUM 9   /* index max load 0.9 */
#define MAX_LOAD_DEN 10

typedef struct {
    uint16_t vlen; /* total value length (chained values included) */
    uint8_t klen;  /* stored key bytes (the suffix when F_PFX) */
    uint8_t flags;
} ehdr_t;
#define F_USED 1
#define F_REF 2   /* CLOCK reference bit */
#define F_TTL 4   /* a 4-byte expiry deadline follows the header */
#define F_PFX 8   /* a 1-byte prefix id precedes the key; klen = suffix length */
#define F_CHAIN 16 /* value continues in a chain of continuation chunks */
#define F_CONT 32  /* this chunk is a continuation chunk (not an entry) */

/* continuation chunk: [len u16 | pad u8 | flags u8 = F_CONT][next u32][head u32][data ...] */
typedef struct {
    uint16_t len;
    uint8_t pad;
    uint8_t flags;
    uint32_t next;
    uint32_t head;
} chdr_t;

typedef struct {
    uint8_t cls;
    uint8_t shard;
    uint16_t used;
    uint16_t next; /* next page in class list / free list */
    uint16_t nchunks;
} phdr_t;

/* <= 64 B: 4-byte steps; <= 256 B: 8/16-byte steps; above: "page-fit" sizes 4088/n (rounded down
 * to 4), so every page divides into whole chunks (v2-first-run classes 640/768/1016 left a 532 B
 * entry 6 per page; 584 = 4088/7 fits 7). */
static const uint16_t cls_size[] = {8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  48,  52,   56,   60,  64,
                                    72,  80,  88,  96,  104, 112, 120, 128, 144, 160, 176, 192,  208,  224, 240,
                                    252, 272, 292, 312, 340, 368, 408, 452, 508, 584, 680, 816, 1020, 1360, 2044, 4088};
#define NCLS (sizeof(cls_size) / sizeof(cls_size[0]))
#define CLS_MAX (NCLS - 1)
#define CLS_NONE 0xFF
#define CONT_DATA (4088 - sizeof(chdr_t))

typedef struct {
    uint32_t *idx;
    uint32_t nslots, count, max_count;
    uint32_t free_head[NCLS];
    uint16_t cls_pages[NCLS]; /* head of class page list */
    uint16_t npages_cls[NCLS];
    uint16_t hand_page[NCLS], hand_chunk[NCLS];
    uint16_t free_pages, nfree_pages;
    uint32_t used_bytes, evictions, expired, exp_cursor;
    portMUX_TYPE mux;
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

static uint8_t *pages[MAX_PAGES]; /* page 0 unused so that ref != 0; NULL = released */
static uint16_t npages;           /* highest page index in use */
static uint32_t store_gen;        /* bumped by kv_init/kv_flush: invalidates streams in flight */
static uint16_t live_pages;
static int evict_on = 1;
static uint32_t index_bytes;

static void default_mode(void);

const char *kv_engine_name(void)
{
    static char name[48];
    static const char *ln[] = {"nolock", "mux", "cas", "rw", "seqlock"};
    default_mode();
    snprintf(name, sizeof name, "opt2-slab-oa-%dsh-%s%s", g_nshards, ln[g_lock], g_prefix ? "-pfx" : "");
    return name;
}

/* Server defaults (Phases 1-2): one core owns the table -> no lock; two cores -> 8 shards with
 * a seqlock (lock-free GETs, CAS-locked writers). */
static void default_mode(void)
{
    if (g_mode_set) return;
#if CONFIG_KV_PREFIX_COMPRESS
    g_prefix = 1;
#endif
#if FIXED_SINGLE
    return;
#elif CONFIG_KV_FORCE_LOCK
    g_nshards = 1, g_lock = KV_LOCK_MUX; /* Entry 2 ablation: cost of a portMUX lock */
#elif CONFIG_KV_SERVER_BASIC
    g_nshards = 1, g_lock = KV_LOCK_CAS; /* task per client: many tasks on both cores */
#elif CONFIG_KV_CORES_SHARED
    g_nshards = 1, g_lock = KV_LOCK_SEQ;
#elif CONFIG_KV_CORES_SHARDED
    g_nshards = 8, g_lock = KV_LOCK_SEQ;
#elif CONFIG_KV_CORES_OWNER
    g_nshards = 8, g_lock = KV_LOCK_NONE; /* single owner per shard: routing replaces locking */
#elif CONFIG_KV_CORES_DISPATCH
    g_nshards = 1, g_lock = KV_LOCK_NONE; /* the dispatch worker owns everything */
#else
    g_nshards = 1, g_lock = KV_LOCK_NONE;
#endif
}

/* ---------------------------------------------------------------- hashing */
#define FNV0 2166136261u
static inline HOT uint32_t fnv_run(uint32_t h, const uint8_t *p, size_t n)
{
    while (n >= 4) {
        h = (h ^ p[0]) * 16777619u;
        h = (h ^ p[1]) * 16777619u;
        h = (h ^ p[2]) * 16777619u;
        h = (h ^ p[3]) * 16777619u;
        p += 4;
        n -= 4;
    }
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static inline HOT uint32_t fmix(uint32_t h) /* murmur3 fmix32: full avalanche */
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static inline HOT uint32_t hash32(const uint8_t *p, size_t n) { return fmix(fnv_run(FNV0, p, n)); }

/* expiry deadline: exactly now + ttl (v1 used "| 1" to avoid 0, which added up to 1 ms) */
static inline uint32_t deadline(uint32_t now, uint32_t ttl)
{
    uint32_t d = now + ttl;
    return d ? d : 1;
}

static inline HOT uint32_t home(const shard_t *sh, uint32_t h)
{
    return (uint32_t)(((uint64_t)h * sh->nslots) >> 32);
}

static inline HOT shard_t *shard_of(uint32_t h) { return &shards[(h >> 11) & (uint32_t)(g_nshards - 1)]; }

/* ------------------------------------------------------- prefix dictionary (3c) */
#define NPFX 64 /* ids 1..63 */
#define PFX_MIN 4
#define PFX_MAX 24
static uint8_t pfx_len[NPFX];
static uint8_t pfx_bytes[NPFX][PFX_MAX];
static uint8_t pfx_map[128]; /* fmix(prefix FNV state) & 127 -> id, linear probing */
static volatile int npfx = 1;
static portMUX_TYPE pfx_mux = portMUX_INITIALIZER_UNLOCKED;

/* the request's key, digested once per operation */
typedef struct {
    const uint8_t *k;
    uint32_t h;  /* hash of the full key */
    uint8_t kl;  /* full length */
    uint8_t plen; /* prefix length if pid != 0 */
    uint8_t pid; /* dictionary id of its prefix, 0 = none */
    uint8_t cand; /* prefix length that could be learned (SET) */
    uint32_t ph; /* fmix of the prefix FNV state (dictionary lookup) */
} key_t;

static inline int pfx_find(uint32_t ph, const uint8_t *k, int plen)
{
    for (int i = ph & 127, n = 0; n < 128; n++, i = (i + 1) & 127) {
        int id = pfx_map[i];
        if (!id) return 0;
        if (pfx_len[id] == plen && memcmp(pfx_bytes[id], k, plen) == 0) return id;
    }
    return 0;
}

static int pfx_add(uint32_t ph, const uint8_t *k, int plen)
{
    int id = 0;
    portENTER_CRITICAL(&pfx_mux); /* writers only; readers never block (entries are immutable) */
    id = pfx_find(ph, k, plen);
    if (!id && npfx < NPFX) {
        id = npfx;
        pfx_len[id] = plen;
        memcpy(pfx_bytes[id], k, plen);
        int i = ph & 127;
        while (pfx_map[i]) i = (i + 1) & 127;
        __sync_synchronize(); /* publish bytes before the map entry */
        pfx_map[i] = id;
        npfx = id + 1;
    }
    portEXIT_CRITICAL(&pfx_mux);
    return id;
}

/* slow path: prefix compression on (kept out of line so the common path stays small) */
static HOT __attribute__((noinline)) void mkkey_prefix(key_t *q, const uint8_t *k, size_t kl, int learn)
{
    /* prefix = up to and including the last ':' within the first PFX_MAX bytes, suffix non-empty */
    int plen = 0;
    int lim = (int)kl - 1 < PFX_MAX ? (int)kl - 1 : PFX_MAX; /* suffix must stay non-empty */
    for (int i = lim - 1; i >= PFX_MIN - 1; i--)
        if (k[i] == ':') {
            plen = i + 1;
            break;
        }
    uint32_t hp = fnv_run(FNV0, k, plen);
    q->h = fmix(fnv_run(hp, k + plen, kl - plen));
    if (!plen) return;
    q->ph = fmix(hp);
    q->cand = plen;
    int id = pfx_find(q->ph, k, plen);
    if (!id && learn) id = pfx_add(q->ph, k, plen);
    if (id) {
        q->pid = id;
        q->plen = plen;
    }
}

/* digest the request key once per operation; inlined fast path when prefixes are off */
static inline __attribute__((always_inline)) void mkkey(key_t *q, const uint8_t *k, size_t kl, int learn)
{
    q->k = k;
    q->kl = kl;
    q->pid = q->plen = q->cand = 0;
    if (likely(!g_prefix)) q->h = hash32(k, kl);
    else mkkey_prefix(q, k, kl, learn);
}

/* --------------------------------------------------------------- pages/refs */
static inline HOT void *ref2p(uint32_t ref) { return pages[ref >> REF_OFF_BITS] + ((ref & 1023) << 2); }
static inline HOT ehdr_t *ref2e(uint32_t ref) { return (ehdr_t *)ref2p(ref); }
static inline phdr_t *page_hdr(uint16_t pg) { return (phdr_t *)pages[pg]; }
static inline uint32_t mkref(uint16_t pg, uint32_t off) { return ((uint32_t)pg << REF_OFF_BITS) | (off >> 2); }
static inline HOT uint32_t *free_next(void *chunk) { return (uint32_t *)((uint8_t *)chunk + 4); }

static inline HOT uint8_t *e_after(ehdr_t *e) { return (uint8_t *)(e + 1) + ((e->flags & F_TTL) ? 4 : 0); }
static inline HOT uint32_t e_expire(ehdr_t *e) { return (e->flags & F_TTL) ? *(uint32_t *)(e + 1) : 0; }
/* expired? reads the clock only for entries that carry a deadline (v1 read it on every op) */
static inline HOT int e_is_expired(ehdr_t *e)
{
    return unlikely(e->flags & F_TTL) && kv_expired(e_expire(e), kv_now_ms());
}
static inline HOT uint8_t e_pid(ehdr_t *e) { return (e->flags & F_PFX) ? *e_after(e) : 0; }
static inline HOT uint8_t *e_key(ehdr_t *e) { return e_after(e) + ((e->flags & F_PFX) ? 1 : 0); }
static inline HOT uint8_t *e_tail(ehdr_t *e) { return e_key(e) + e->klen; } /* next (chain) or value */
static inline HOT uint8_t *e_val(ehdr_t *e) { return e_tail(e) + ((e->flags & F_CHAIN) ? 4 : 0); }
static inline uint32_t e_next(ehdr_t *e)
{
    uint32_t n;
    memcpy(&n, e_tail(e), 4);
    return n;
}

/* bytes before the value: header + optional expiry + optional prefix id + key + optional next */
static inline size_t e_prefix_bytes(int fl, size_t kl)
{
    return sizeof(ehdr_t) + ((fl & F_TTL) ? 4 : 0) + ((fl & F_PFX) ? 1 : 0) + kl + ((fl & F_CHAIN) ? 4 : 0);
}

/* full-key hash of a stored entry (for backward-shift delete) */
static uint32_t e_hash(ehdr_t *e)
{
    if (e->flags & F_PFX) {
        int id = e_pid(e);
        return fmix(fnv_run(fnv_run(FNV0, pfx_bytes[id], pfx_len[id]), e_key(e), e->klen));
    }
    return hash32(e_key(e), e->klen);
}

static inline HOT int key_eq(ehdr_t *e, const key_t *q)
{
    if (e->flags & F_PFX)
        return q->pid && e_pid(e) == q->pid && e->klen == q->kl - q->plen &&
               memcmp(e_key(e), q->k + q->plen, e->klen) == 0;
    return e->klen == q->kl && memcmp(e_key(e), q->k, q->kl) == 0;
}

/* class for an entry of `total` bytes: table lookup up to 256 B (v2-first-run scanned 40 classes on
 * every SET: ~20 % slower SETs), binary search above */
static uint8_t cls_lut[256 / 4 + 1];
static void cls_lut_init(void)
{
    for (int t = 0, c = 0; t <= 256 / 4; t++) {
        while (cls_size[c] < t * 4) c++;
        cls_lut[t] = c;
    }
}
static inline HOT int cls_for(size_t total)
{
    if (likely(total <= 256)) return cls_lut[(total + 3) >> 2];
    if (total > cls_size[CLS_MAX]) return -1;
    int lo = 0, hi = CLS_MAX;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (cls_size[m] < total) lo = m + 1;
        else hi = m;
    }
    return lo;
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
    for (int i = ph->nchunks - 1; i >= 0; i--) {
        uint32_t off = PAGE_HDR + i * cls_size[cls];
        ehdr_t *e = (ehdr_t *)(pages[pg] + off);
        e->flags = 0;
        *free_next(e) = sh->free_head[cls];
        sh->free_head[cls] = mkref(pg, off);
    }
}

/* ------------------------------------------------------------------ index */
static HOT ehdr_t *lookup(shard_t *sh, const key_t *q, uint32_t *pos)
{
    uint32_t i = home(sh, q->h), tag = TAG_OF(q->h);
    uint32_t *idx = sh->idx, n = sh->nslots;
    for (;;) {
        uint32_t s = idx[i];
        if (s == 0) {
            *pos = i;
            return NULL;
        }
        if ((s >> REF_BITS) == tag) {
            ehdr_t *e = ref2e(s & REF_MASK);
            if (key_eq(e, q)) {
                *pos = i;
                return e;
            }
        }
        if (++i == n) i = 0;
    }
}

static void slot_delete(shard_t *sh, uint32_t i)
{
    uint32_t *idx = sh->idx, n = sh->nslots, j = i;
    for (;;) {
        if (++j == n) j = 0;
        uint32_t s = idx[j];
        if (s == 0) break;
        uint32_t hp = home(sh, e_hash(ref2e(s & REF_MASK)));
        int stay = (i <= j) ? (hp > i && hp <= j) : (hp > i || hp <= j);
        if (!stay) {
            idx[i] = s;
            i = j;
        }
    }
    idx[i] = 0;
    sh->count--;
}

static void chunk_put(shard_t *sh, uint32_t ref)
{
    phdr_t *ph = page_hdr(ref >> REF_OFF_BITS);
    sh->used_bytes -= cls_size[ph->cls];
    ph->used--;
    void *c = ref2p(ref);
    ((ehdr_t *)c)->flags = 0;
    *free_next(c) = sh->free_head[ph->cls];
    sh->free_head[ph->cls] = ref;
}

/* free an entry's chunk and, for chained values, its continuation chunks */
static void chunk_free(shard_t *sh, uint32_t ref)
{
    ehdr_t *e = ref2e(ref);
    if (e->flags & F_CHAIN) {
        uint32_t c = e_next(e);
        while (c) {
            uint32_t nx = ((chdr_t *)ref2p(c))->next;
            chunk_put(sh, c);
            c = nx;
        }
    }
    chunk_put(sh, ref);
}

static void entry_remove(shard_t *sh, uint32_t ref)
{
    uint32_t i = home(sh, e_hash(ref2e(ref)));
    while ((sh->idx[i] & REF_MASK) != ref) {
        if (++i == sh->nslots) i = 0;
    }
    slot_delete(sh, i);
    chunk_free(sh, ref);
}

/* CLOCK sweep inside one class. Returns 1 if an entry was evicted. */
static int evict_in_class(shard_t *sh, int cls)
{
    uint32_t now = kv_now_ms();
    if (!sh->cls_pages[cls]) return 0;
    uint16_t pg = sh->hand_page[cls];
    uint16_t ci = sh->hand_chunk[cls];
    if (!pg || !pages[pg] || page_hdr(pg)->cls != cls) {
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
        if (!(e->flags & F_USED)) continue; /* free or continuation chunk */
        int exp = kv_expired(e_expire(e), now);
        if ((e->flags & F_REF) && !exp) {
            e->flags &= ~F_REF; /* second chance */
            continue;
        }
        if (exp) sh->expired++;
        else sh->evictions++;
        entry_remove(sh, mkref(pg, off));
        sh->hand_page[cls] = pg;
        sh->hand_chunk[cls] = ci;
        return 1;
    }
    return 0;
}

/* Empty one page of class `victim`: evict its entries (and the owners of any continuation
 * chunks in it), unlink its free chunks, detach it from the class. */
static void page_evacuate(shard_t *sh, int victim, uint16_t pg)
{
    phdr_t *ph = page_hdr(pg);
    uint16_t sz = cls_size[victim];
    for (int i = 0; i < ph->nchunks; i++) {
        uint32_t off = PAGE_HDR + i * sz;
        ehdr_t *e = (ehdr_t *)(pages[pg] + off);
        if (e->flags & F_USED) {
            sh->evictions++;
            entry_remove(sh, mkref(pg, off));
        } else if (e->flags & F_CONT) {
            sh->evictions++;
            entry_remove(sh, ((chdr_t *)e)->head); /* frees this chunk as part of the chain */
        }
    }
    uint32_t *pp = &sh->free_head[victim];
    while (*pp) {
        if ((*pp >> REF_OFF_BITS) == pg) *pp = *free_next(ref2p(*pp));
        else pp = free_next(ref2p(*pp));
    }
    uint16_t *lp = &sh->cls_pages[victim];
    while (*lp != pg) lp = &page_hdr(*lp)->next;
    *lp = ph->next;
    sh->npages_cls[victim]--;
    if (sh->hand_page[victim] == pg) sh->hand_page[victim] = 0;
    ph->cls = CLS_NONE;
}

/* Steal a page from the class holding the most pages (never `want_cls`, so a chain under
 * construction in class CLS_MAX is never touched while CLS_MAX allocates). */
#define PG_PINNED 0x80 /* in phdr.shard: page holds a value being streamed in, never evacuate */
static inline int page_pinned(uint16_t pg) { return page_hdr(pg)->shard & PG_PINNED; }

static uint16_t steal_page(shard_t *sh, int want_cls)
{
    int victim = -1;
    uint16_t vpg = 0;
    for (int c = 0; c < (int)NCLS; c++) {
        if (c == want_cls || sh->npages_cls[c] == 0 || (victim >= 0 && sh->npages_cls[c] <= sh->npages_cls[victim]))
            continue;
        uint16_t pg = sh->cls_pages[c];
        while (pg && page_pinned(pg)) pg = page_hdr(pg)->next;
        if (pg) {
            victim = c;
            vpg = pg;
        }
    }
    if (victim < 0) return 0;
    page_evacuate(sh, victim, vpg);
    return vpg;
}

/* A page of another class whose chunks are all free can be reassigned without evicting
 * anything; without this, empty pages stayed stuck in their old class (slab calcification),
 * e.g. a 64 KB value failed in noeviction mode with plenty of empty pages around. */
static uint16_t reclaim_empty_page(shard_t *sh, int want_cls)
{
    for (int c = 0; c < (int)NCLS; c++) {
        if (c == want_cls) continue;
        for (uint16_t pg = sh->cls_pages[c]; pg; pg = page_hdr(pg)->next)
            if (page_hdr(pg)->used == 0) {
                page_evacuate(sh, c, pg);
                return pg;
            }
    }
    return 0;
}

static uint32_t chunk_alloc(shard_t *sh, int cls)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t ref = sh->free_head[cls];
        if (ref) {
            sh->free_head[cls] = *free_next(ref2p(ref));
            page_hdr(ref >> REF_OFF_BITS)->used++;
            sh->used_bytes += cls_size[cls];
            return ref;
        }
        if (sh->free_pages) {
            uint16_t pg = sh->free_pages;
            sh->free_pages = page_hdr(pg)->next;
            sh->nfree_pages--;
            page_assign(sh, pg, cls);
            continue;
        }
        uint16_t epg = reclaim_empty_page(sh, cls);
        if (epg) {
            page_assign(sh, epg, cls);
            continue;
        }
        if (!evict_on) return 0;
        if (evict_in_class(sh, cls)) continue;
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
    sh->free_pages = sh->nfree_pages = 0;
    sh->used_bytes = sh->evictions = sh->expired = sh->exp_cursor = 0;
}

static void page_to_free_list(uint16_t pg, int s)
{
    shard_t *sh = &shards[s];
    phdr_t *ph = page_hdr(pg);
    ph->cls = CLS_NONE;
    ph->shard = s;
    ph->next = sh->free_pages;
    sh->free_pages = pg;
    sh->nfree_pages++;
}

static void distribute_pages(void)
{
    for (uint16_t pg = npages; pg >= 1; pg--)
        if (pages[pg]) page_to_free_list(pg, pg % NSH);
}

#define CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)

/* allocate one page from the heap if that leaves at least `reserve` free; returns its index */
static uint16_t page_new(size_t reserve)
{
    if (heap_caps_get_free_size(CAPS) < reserve + PAGE_SZ + 64) return 0;
    uint16_t pg = 0;
    for (uint16_t i = 1; i <= npages; i++)
        if (!pages[i]) {
            pg = i;
            break;
        }
    if (!pg) {
        if (npages >= MAX_PAGES - 1) return 0;
        pg = npages + 1;
    }
    uint8_t *p = heap_caps_malloc(PAGE_SZ, CAPS);
    if (!p) return 0;
    pages[pg] = p;
    if (pg > npages) npages = pg;
    live_pages++;
    return pg;
}

int kv_init(size_t reserve)
{
    default_mode();
    cls_lut_init();
    size_t free0 = heap_caps_get_free_size(CAPS);
    if (free0 < reserve + 2 * PAGE_SZ) return -1;
    size_t avail = free0 - reserve;
    uint32_t est_entries = avail / AVG_ENTRY_EST;
    uint32_t slots_total = est_entries * MAX_LOAD_DEN / MAX_LOAD_NUM + 16;
    index_bytes = 0;
    for (int s = 0; s < NSH; s++) {
        shard_t *sh = &shards[s];
        sh->nslots = slots_total / NSH;
        sh->max_count = sh->nslots * MAX_LOAD_NUM / MAX_LOAD_DEN;
        sh->idx = heap_caps_malloc(sh->nslots * sizeof(uint32_t), CAPS);
        if (!sh->idx) return -1;
        index_bytes += sh->nslots * sizeof(uint32_t);
        portMUX_INITIALIZE(&sh->mux);
        sh->lk = 0;
        sh->seq = 0;
    }
    /* one page at a time until only `reserve` is left (pages can be released later) */
    npages = live_pages = 0;
    memset(pages, 0, sizeof pages);
    while (page_new(reserve)) {
    }
    for (int s = 0; s < NSH; s++) shard_reset(&shards[s]);
    distribute_pages();
    store_gen++;
    ESP_LOGI(TAG, "arena %u pages (%u KiB), index %u slots (%u B), %d shard(s), heap left %u", live_pages,
             live_pages * 4, (unsigned)slots_total, (unsigned)index_bytes, NSH,
             (unsigned)heap_caps_get_free_size(CAPS));
    return live_pages ? 0 : -1;
}

void kv_deinit(void)
{
    for (int s = 0; s < NSH; s++) {
        heap_caps_free(shards[s].idx);
        shards[s].idx = NULL;
    }
    for (int i = 1; i <= npages; i++) {
        heap_caps_free(pages[i]);
        pages[i] = NULL;
    }
    npages = live_pages = 0;
}

void kv_flush(void)
{
    store_gen++;
    for (int s = 0; s < NSH; s++) {
        shard_t *sh = &shards[s];
        SH_LOCK(sh);
        shard_reset(sh);
        SH_UNLOCK(sh);
    }
    distribute_pages();
}

/* write a complete entry (and its chain) into chunk `ref`; conts[] are pre-allocated chunks */
static HOT __attribute__((noinline)) void write_entry_chain(uint32_t ref, int fl, uint32_t expire, const key_t *q,
                                                           const uint8_t *v, size_t vl, const uint32_t *conts,
                                                           int nconts)
{
    ehdr_t *e = ref2e(ref);
    int pfx = fl & F_PFX;
    size_t kl = pfx ? q->kl - q->plen : q->kl;
    e->vlen = vl;
    e->klen = kl;
    uint8_t *p = (uint8_t *)(e + 1);
    if (fl & F_TTL) {
        memcpy(p, &expire, 4);
        p += 4;
    }
    if (pfx) *p++ = q->pid;
    memcpy(p, pfx ? q->k + q->plen : q->k, kl);
    p += kl;
    size_t first = vl;
    if (fl & F_CHAIN) {
        memcpy(p, &conts[0], 4);
        p += 4;
        first = cls_size[CLS_MAX] - (p - (uint8_t *)e);
    }
    memcpy(p, v, first);
    v += first;
    vl -= first;
    for (int i = 0; i < nconts; i++) {
        chdr_t *c = ref2p(conts[i]);
        size_t n = vl < CONT_DATA ? vl : CONT_DATA;
        c->len = n;
        c->next = i + 1 < nconts ? conts[i + 1] : 0;
        c->head = ref;
        c->flags = F_CONT;
        memcpy(c + 1, v, n);
        v += n;
        vl -= n;
    }
    __sync_synchronize();
    e->flags = F_USED | F_REF | fl; /* publish last */
}

/* single-chunk entries (the common case) are written inline; chains go out of line */
static inline __attribute__((always_inline)) void write_entry(uint32_t ref, int fl, uint32_t expire, const key_t *q,
                                                              const uint8_t *v, size_t vl, const uint32_t *conts,
                                                              int nconts)
{
    if (unlikely(fl & F_CHAIN)) {
        write_entry_chain(ref, fl, expire, q, v, vl, conts, nconts);
        return;
    }
    ehdr_t *e = ref2e(ref);
    size_t kl = (fl & F_PFX) ? q->kl - q->plen : q->kl;
    e->vlen = vl;
    e->klen = kl;
    uint8_t *p = (uint8_t *)(e + 1);
    if (fl & F_TTL) {
        memcpy(p, &expire, 4);
        p += 4;
    }
    if (fl & F_PFX) *p++ = q->pid;
    memcpy(p, (fl & F_PFX) ? q->k + q->plen : q->k, kl);
    memcpy(p + kl, v, vl);
    __sync_synchronize();
    e->flags = F_USED | F_REF | fl; /* publish last */
}

/* SET body; caller holds the shard's write lock. ttl_abs != 0: use this absolute deadline */
static inline __attribute__((always_inline)) int set_locked(shard_t *sh, const key_t *qp, const uint8_t *v, size_t vl,
                                                            uint32_t ttl_ms, uint32_t ttl_abs, int flags)
{
#define q (*qp)
    size_t kl = q.kl;
    uint32_t pos;
    int rc = KV_OK;
    ehdr_t *e = lookup(sh, &q, &pos);
    if (e && e_is_expired(e)) {
        entry_remove(sh, sh->idx[pos] & REF_MASK);
        sh->expired++;
        e = NULL;
    }
    if (((flags & KV_NX) && e) || ((flags & KV_XX) && !e)) {
        rc = KV_NOTSET;
        goto out;
    }
    uint32_t expire = ttl_abs ? ttl_abs : ttl_ms ? deadline(kv_now_ms(), ttl_ms) : ((flags & KV_KEEPTTL) && e ? e_expire(e) : 0);
    int fl = (expire ? F_TTL : 0) | (q.pid ? F_PFX : 0);
    size_t skl = q.pid ? kl - q.plen : kl;
    int cls = cls_for(e_prefix_bytes(fl, skl) + vl);
    int nconts = 0;
    if (cls < 0) { /* chain: head chunk of the largest class + continuation chunks */
        fl |= F_CHAIN | F_TTL; /* chains always carry the expiry field: EXPIRE is then a plain write */
        cls = CLS_MAX;
        size_t first = cls_size[CLS_MAX] - e_prefix_bytes(fl, skl);
        nconts = (vl - first + CONT_DATA - 1) / CONT_DATA;
    }

    /* fast path: rewrite in place when the new entry fits the same slab class */
    if (e && !(fl & F_CHAIN) && !(e->flags & F_CHAIN) &&
        page_hdr((sh->idx[pos] & REF_MASK) >> REF_OFF_BITS)->cls == cls) {
        write_entry(sh->idx[pos] & REF_MASK, fl, expire, &q, v, vl, NULL, 0);
        goto out;
    }
    if (!e && sh->count >= sh->max_count) { /* index full -> evict first */
        int c2 = cls;
        if (!evict_on || !evict_in_class(sh, cls)) {
            for (c2 = 0; c2 < (int)NCLS && !(evict_on && evict_in_class(sh, c2)); c2++) {
            }
        }
        if (c2 == (int)NCLS) {
            rc = KV_ENOMEM;
            goto out;
        }
    }
    uint32_t ref = chunk_alloc(sh, cls);
    uint32_t conts[17];
    int got = 0;
    if (ref) {
        ((ehdr_t *)ref2e(ref))->flags = F_CONT; /* not an entry yet; never evicted while building */
        ((chdr_t *)ref2p(ref))->head = 0;
        for (; got < nconts; got++) {
            conts[got] = chunk_alloc(sh, CLS_MAX);
            if (!conts[got]) break;
            ((chdr_t *)ref2p(conts[got]))->flags = F_CONT;
            ((chdr_t *)ref2p(conts[got]))->head = 0;
        }
    }
    if (!ref || got < nconts) {
        for (int i = 0; i < got; i++) chunk_put(sh, conts[i]);
        if (ref) chunk_put(sh, ref);
        rc = KV_ENOMEM;
        goto out;
    }
    write_entry(ref, fl, expire, &q, v, vl, conts, nconts);
    /* chunk_alloc may have evicted/shifted entries: look the key up again */
    e = lookup(sh, &q, &pos);
    if (e && (sh->idx[pos] & REF_MASK) != ref) {
        uint32_t old = sh->idx[pos] & REF_MASK;
        sh->idx[pos] = SLOT(TAG_OF(q.h), ref);
        chunk_free(sh, old);
    } else if (!e) {
        sh->idx[pos] = SLOT(TAG_OF(q.h), ref);
        sh->count++;
    }
out:
    return rc;
#undef q
}

HOT int kv_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, uint32_t ttl_ms, int flags)
{
    if (unlikely(kl > MAX_KEY || vl > MAX_VAL)) return KV_ETOOBIG;
    key_t q;
    mkkey(&q, k, kl, 1);
    shard_t *sh = shard_of(q.h);
    SH_LOCK(sh);
    int rc = set_locked(sh, &q, v, vl, ttl_ms, 0, flags);
    SH_UNLOCK(sh);
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

/* "$<len>\r\n<value>\r\n"; chained values are gathered from their continuation chunks */
static inline HOT int put_entry(obuf_t *o, ehdr_t *e)
{
    uint32_t vl = e->vlen;
    if (!ob_room(o, 1 + 5 + 2 + vl + 2)) return KV_ENOROOM;
    uint8_t *w = o->buf + o->len;
    *w++ = '$';
    w += utoa10((char *)w, vl);
    *w++ = '\r';
    *w++ = '\n';
    if (likely(!(e->flags & F_CHAIN))) {
        memcpy(w, e_val(e), vl);
        w += vl;
    } else {
        size_t first = cls_size[CLS_MAX] - (e_val(e) - (uint8_t *)e);
        memcpy(w, e_val(e), first);
        w += first;
        for (uint32_t c = e_next(e); c; c = ((chdr_t *)ref2p(c))->next) {
            chdr_t *ch = ref2p(c);
            memcpy(w, ch + 1, ch->len);
            w += ch->len;
        }
    }
    *w++ = '\r';
    *w++ = '\n';
    o->len = w - o->buf;
    return KV_OK;
}

static inline __attribute__((always_inline)) int get_locked(shard_t *sh, const key_t *q, obuf_t *o, int may_write)
{
    uint32_t pos;
    ehdr_t *e = lookup(sh, q, &pos);
    if (e && e_is_expired(e)) {
        if (may_write) {
            entry_remove(sh, sh->idx[pos] & REF_MASK);
            sh->expired++;
        }
        e = NULL;
    }
    if (!e) return put_nil(o);
    int rc = put_entry(o, e);
    if (rc == KV_OK) e->flags |= F_REF; /* concurrent readers only ever set this bit */
    return rc;
}

#define OPT_RETRY (-100)
/* Optimistic (seqlock) read: runs concurrently with a writer, so every field may be torn.
 * Everything is bounds-checked; the caller validates the sequence number afterwards and
 * discards the result on any change. Chained values go to the locked path. */
static HOT int get_optimistic(shard_t *sh, const key_t *q, obuf_t *o)
{
    uint32_t i = home(sh, q->h), tag = TAG_OF(q->h), n = sh->nslots;
    for (uint32_t probes = 0; probes < n; probes++) {
        uint32_t s = sh->idx[i];
        if (s == 0) return put_nil(o);
        if ((s >> REF_BITS) == tag) {
            uint32_t ref = s & REF_MASK, pg = ref >> REF_OFF_BITS;
            if (pg == 0 || pg > npages || !pages[pg]) return OPT_RETRY;
            ehdr_t *e = ref2e(ref);
            uint32_t fl = e->flags, ekl = e->klen, vl = e->vlen, c = page_hdr(pg)->cls;
            if (!(fl & F_USED) || (fl & (F_CHAIN | F_CONT)) || c >= NCLS) return OPT_RETRY;
            if (e_prefix_bytes(fl, ekl) + vl > cls_size[c]) return OPT_RETRY; /* torn */
            if ((fl & F_PFX) && (e_pid(e) == 0 || e_pid(e) >= NPFX)) return OPT_RETRY;
            if (key_eq(e, q)) {
                if (e_is_expired(e)) return put_nil(o);
                return put_entry(o, e);
            }
        }
        if (++i == n) i = 0;
    }
    return OPT_RETRY;
}

HOT int kv_get_resp(const uint8_t *k, size_t kl, obuf_t *o)
{
    if (unlikely(kl > MAX_KEY)) return put_nil(o);
    key_t q;
    mkkey(&q, k, kl, 0);
    shard_t *sh = shard_of(q.h);
    int rc;
    if (g_lock == KV_LOCK_SEQ) {
        lkstat_t *st = &lkstat[xPortGetCoreID()];
        size_t len0 = o->len;
        st->acq++;
        for (int tries = 0; tries < 16; tries++) {
            uint32_t s1;
            if ((s1 = sh->seq) & 1) {
                uint32_t t0 = ccount();
                while ((s1 = sh->seq) & 1) {
                }
                st->spin_cycles += ccount() - t0;
                st->contended++;
            }
            __sync_synchronize();
            rc = get_optimistic(sh, &q, o);
            __sync_synchronize();
            if (sh->seq == s1) {
                if (rc != OPT_RETRY) return rc;
                break; /* consistent but not servable optimistically (chained value): lock */
            }
            o->len = len0;
            st->retries++;
        }
        o->len = len0;
        mc_wr_lock(sh);
        rc = get_locked(sh, &q, o, 1);
        mc_wr_unlock(sh);
        return rc;
    }
    RD_LOCK(sh);
    rc = get_locked(sh, &q, o, READER_MAY_WRITE);
    RD_UNLOCK(sh);
    return rc;
}

static ehdr_t *find_live(shard_t *sh, const key_t *q, uint32_t *pos)
{
    ehdr_t *e = lookup(sh, q, pos);
    if (e && e_is_expired(e)) {
        entry_remove(sh, sh->idx[*pos] & REF_MASK);
        sh->expired++;
        return NULL;
    }
    return e;
}

HOT int kv_del(const uint8_t *k, size_t kl)
{
    if (unlikely(kl > MAX_KEY)) return 0;
    key_t q;
    mkkey(&q, k, kl, 0);
    uint32_t pos;
    shard_t *sh = shard_of(q.h);
    SH_LOCK(sh);
    ehdr_t *e = find_live(sh, &q, &pos);
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
    if (unlikely(kl > MAX_KEY)) return 0;
    key_t q;
    mkkey(&q, k, kl, 0);
    uint32_t pos;
    shard_t *sh = shard_of(q.h);
    SH_LOCK(sh);
    int r = find_live(sh, &q, &pos) != NULL;
    SH_UNLOCK(sh);
    return r;
}

int64_t kv_pttl(const uint8_t *k, size_t kl)
{
    if (unlikely(kl > MAX_KEY)) return -2;
    key_t q;
    mkkey(&q, k, kl, 0);
    uint32_t pos;
    shard_t *sh = shard_of(q.h);
    int64_t r;
    SH_LOCK(sh);
    ehdr_t *e = find_live(sh, &q, &pos);
    if (!e) r = -2;
    else if (!e_expire(e)) r = -1;
    else r = (int32_t)(e_expire(e) - kv_now_ms());
    SH_UNLOCK(sh);
    return r;
}

/* EXPIRE: entries with an expiry field (incl. every chained entry) get a plain write; an entry
 * stored without one is rebuilt with the field, under the same lock (no lost concurrent update). */
int kv_expire(const uint8_t *k, size_t kl, uint32_t ttl_ms)
{
    if (kl > MAX_KEY) return 0;
    key_t q;
    mkkey(&q, k, kl, 0);
    uint32_t now = kv_now_ms(), pos, ex = deadline(now, ttl_ms);
    shard_t *sh = shard_of(q.h);
    int r = 0;
    SH_LOCK(sh);
    ehdr_t *e = find_live(sh, &q, &pos);
    if (e && (e->flags & F_TTL)) {
        memcpy(e + 1, &ex, 4);
        r = 1;
    } else if (e) { /* not chained here (chains always have F_TTL): value <= one chunk */
        uint32_t vl = e->vlen;
        uint8_t *tmp = heap_caps_malloc(vl ? vl : 1, MALLOC_CAP_8BIT);
        if (tmp) {
            memcpy(tmp, e_val(e), vl);
            r = set_locked(sh, &q, tmp, vl, 0, ex, KV_XX) == KV_OK;
            heap_caps_free(tmp);
        }
    }
    SH_UNLOCK(sh);
    return r;
}

void kv_active_expire(int budget) { kv_active_expire_mask(budget, 0xFF); }

/* sweep only the shards in `mask` (owner mode: a loop sweeps the shards it owns) */
void kv_active_expire_mask(int budget, uint32_t mask)
{
    uint32_t now = kv_now_ms();
    for (int s = 0; s < NSH; s++) {
        if (!(mask >> s & 1)) continue;
        shard_t *sh = &shards[s];
        SH_LOCK(sh);
        for (int b = 0; b < budget; b++) {
            uint32_t i = sh->exp_cursor;
            uint32_t sl = sh->idx[i];
            if (sl) {
                ehdr_t *e = ref2e(sl & REF_MASK);
                if (kv_expired(e_expire(e), now)) {
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
void kv_set_prefix(int on)
{
    g_prefix = on; /* only change on an empty store: prefixed entries need it on to be found */
}
int kv_prefix_count(void) { return npfx - 1; }
int kv_pages_live(void) { return live_pages; }

void kv_stats(kv_stats_t *st)
{
    memset(st, 0, sizeof *st);
    for (int s = 0; s < NSH; s++) {
        shard_t *sh = &shards[s];
        SH_LOCK(sh);
        st->keys += sh->count;
        st->mem_used += sh->used_bytes;
        st->evictions += sh->evictions;
        st->expired += sh->expired;
        st->index_slots += sh->nslots;
        SH_UNLOCK(sh);
    }
    st->mem_used += index_bytes * st->keys / (st->index_slots ? st->index_slots : 1);
    st->mem_total = (uint32_t)live_pages * PAGE_SZ + index_bytes;
    st->shards = NSH;
}

/* ------------------------------------------------------------ harness API */
void kv_mc_set_mode(int nshards, int lock_mode)
{
#if FIXED_SINGLE
    (void)nshards, (void)lock_mode; /* fixed at compile time in this build */
#else
    int n = 1;
    while (n < nshards && n < MAXSHARDS) n <<= 1; /* round up to a power of two */
    g_nshards = n;
    g_lock = lock_mode;
#endif
    g_mode_set = 1;
}

int kv_shard_index(const uint8_t *k, size_t kl)
{
    key_t q;
    mkkey(&q, k, kl, 0);
    return (int)(shard_of(q.h) - shards);
}

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

/* ------------------------------------------------- adaptive memory reserve (3d)
 * Called periodically (event-loop idle tick). The arena is a set of individually allocated
 * pages, so it can hand pages back to the heap when the rest of the system (WiFi/lwIP buffers)
 * needs memory, and take them again when the heap has room:
 *   free heap < low  : release the shortfall (<= MEMADJ_MAX pages): unused pages first, then pages that are
 *                      empty, then (last resort) evacuate the fullest class's first page
 *   free heap > high : grow by one page per call (keeping `high` free)
 * Returns the change in arena pages (negative = released). */
#define MEMADJ_MAX 16 /* v1 released at most 4 pages per call and fell behind a 12.8 KB/ms burst */

static void page_release(uint16_t pg)
{
    heap_caps_free(pages[pg]);
    pages[pg] = NULL;
    live_pages--;
}

/* take one page out of shard sh for release: free list, then empty, then evacuate */
static uint16_t page_take(shard_t *sh)
{
    if (sh->free_pages) {
        uint16_t pg = sh->free_pages;
        sh->free_pages = page_hdr(pg)->next;
        sh->nfree_pages--;
        return pg;
    }
    uint16_t pg = reclaim_empty_page(sh, -1);
    if (pg) return pg;
    if (!evict_on) return 0; /* noeviction: never drop data to make room */
    return steal_page(sh, -1);
}

int kv_mem_adjust(size_t low, size_t high)
{
    size_t freeb = heap_caps_get_free_size(CAPS);
    int delta = 0;
    if (freeb < low) {
        int need = (low - freeb + PAGE_SZ - 1) / PAGE_SZ; /* release the whole shortfall at once */
        for (int n = 0; n < need && n < MEMADJ_MAX && heap_caps_get_free_size(CAPS) < low; n++) {
            int released = 0;
            for (int s = 0; s < NSH && !released; s++) {
                shard_t *sh = &shards[(s + n) % NSH];
                SH_LOCK(sh);
                uint16_t pg = live_pages > 2 ? page_take(sh) : 0; /* always keep a minimal store */
                if (pg) {
                    page_release(pg);
                    released = 1;
                }
                SH_UNLOCK(sh);
            }
            if (!released) break;
            delta--;
        }
    } else if (freeb > high + PAGE_SZ) {
        uint16_t pg = page_new(high);
        if (pg) {
            int s = pg % NSH;
            SH_LOCK(&shards[s]);
            page_to_free_list(pg, s);
            SH_UNLOCK(&shards[s]);
            delta++;
        }
    }
    return delta;
}

/* ------------------------------------------------ ranged GET reply (streaming large values)
 * Copies bytes [off, off+cap) of the complete RESP reply ("$<n>\r\n" + value + "\r\n") into buf.
 * Lets the server send a value of any size through its normal 4 KB transmit buffer instead of
 * growing it (with a 40 KB heap reserve a 10 KB reply could not get a 16 KB buffer). Returns the
 * bytes copied; *total = full reply length, or 0 if the key is missing (nothing copied). */
size_t kv_get_range(const uint8_t *k, size_t kl, size_t off, uint8_t *buf, size_t cap, size_t *total)
{
    *total = 0;
    if (kl > MAX_KEY) return 0;
    key_t q;
    mkkey(&q, k, kl, 0);
    shard_t *sh = shard_of(q.h);
    uint32_t pos;
    size_t n = 0;
    SH_LOCK(sh); /* write lock: a consistent view even in seqlock/RW modes */
    ehdr_t *e = lookup(sh, &q, &pos);
    if (e && !e_is_expired(e)) {
        char hdr[12];
        int hn = 0;
        hdr[hn++] = '$';
        hn += utoa10(hdr + hn, e->vlen);
        hdr[hn++] = '\r';
        hdr[hn++] = '\n';
        size_t vl = e->vlen, tot = hn + vl + 2;
        *total = tot;
        /* walk the reply's three parts: header, value (inline part then chain), trailer */
        while (n < cap && off + n < tot) {
            size_t p = off + n;
            if (p < (size_t)hn) {
                buf[n++] = hdr[p];
                continue;
            }
            if (p >= hn + vl) {
                buf[n++] = (p == hn + vl) ? '\r' : '\n';
                continue;
            }
            size_t vo = p - hn; /* offset inside the value */
            const uint8_t *src;
            size_t avail;
            size_t first = (e->flags & F_CHAIN) ? cls_size[CLS_MAX] - (e_val(e) - (uint8_t *)e) : vl;
            if (vo < first) {
                src = e_val(e) + vo;
                avail = first - vo;
            } else {
                size_t co = vo - first;
                uint32_t c = e_next(e);
                chdr_t *ch = ref2p(c);
                while (co >= ch->len) {
                    co -= ch->len;
                    ch = ref2p(ch->next);
                }
                src = (const uint8_t *)(ch + 1) + co;
                avail = ch->len - co;
            }
            size_t take = avail < cap - n ? avail : cap - n;
            memcpy(buf + n, src, take);
            n += take;
        }
    }
    SH_UNLOCK(sh);
    return n;
}

#include "kv_stream.inc" /* streamed SET (needs the static helpers above) */
