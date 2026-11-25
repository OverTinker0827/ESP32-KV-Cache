#pragma once
#include "common.h"

enum { KV_OK = 0, KV_NOTFOUND = 1, KV_NOTSET = 2, KV_ENOMEM = -1, KV_ETOOBIG = -2, KV_ENOROOM = -3 };
enum { KV_NX = 1, KV_XX = 2, KV_KEEPTTL = 4 };

typedef struct {
    uint32_t keys;
    uint32_t mem_used;   /* bytes holding live entries (incl. per-entry overhead) */
    uint32_t mem_total;  /* bytes committed to the store (arena+index, or heap used) */
    uint32_t evictions;
    uint32_t expired;
    uint32_t index_slots;
    uint32_t shards;
} kv_stats_t;

/* reserve_bytes: heap to leave untouched for the rest of the system (opt engine). */
int kv_init(size_t reserve_bytes);
void kv_deinit(void);
void kv_flush(void);

/* ttl_ms == 0: no expiry. */
int kv_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, uint32_t ttl_ms, int flags);
/* Appends "$<n>\r\n<val>\r\n" or "$-1\r\n" to o. KV_ENOROOM if it doesn't fit (o untouched). */
int kv_get_resp(const uint8_t *k, size_t kl, obuf_t *o);
int kv_del(const uint8_t *k, size_t kl);    /* 1 if deleted */
int kv_exists(const uint8_t *k, size_t kl); /* 1 if present */
int64_t kv_pttl(const uint8_t *k, size_t kl); /* -2 missing, -1 no expiry, else ms */
int kv_expire(const uint8_t *k, size_t kl, uint32_t ttl_ms); /* 1 if set */
void kv_active_expire(int budget);
void kv_set_evict(int on); /* 1 = allkeys-lru (CLOCK), 0 = noeviction */
/* Phase 3 engine (kv_opt.c v2 only) */
void kv_set_prefix(int on); /* prefix compression; change only on an empty store */
int kv_prefix_count(void);
int kv_pages_live(void);
/* adaptive reserve: release arena pages when free heap < low, grow when > high; returns delta */
int kv_mem_adjust(size_t low, size_t high);
/* streamed SET for values larger than the receive buffer (chained values only) */
typedef struct {
    uint8_t key[256];
    uint32_t h, gen, ref, conts[17];
    size_t vl, written, first, voff;
    int nconts, fl, active;
    uint8_t kl, plen, pid;
} kv_stream_t;
int kv_stream_begin(kv_stream_t *st, const uint8_t *k, size_t kl, size_t vl); /* KV_ETOOBIG: use SET */
size_t kv_stream_write(kv_stream_t *st, const uint8_t *p, size_t n);
int kv_stream_commit(kv_stream_t *st);
void kv_stream_abort(kv_stream_t *st);
/* bytes [off, off+cap) of the full RESP GET reply; *total = reply length (0 = key missing) */
size_t kv_get_range(const uint8_t *k, size_t kl, size_t off, uint8_t *buf, size_t cap, size_t *total);
int kv_get_evict(void);
void kv_stats(kv_stats_t *s);
const char *kv_engine_name(void);

/* ---- multicore harness (CONFIG_KV_MC_BENCH, opt engine): runtime shards + lock discipline ---- */
enum {
    KV_LOCK_NONE = 0, /* single owner, no lock */
    KV_LOCK_MUX,      /* FreeRTOS portMUX spinlock (disables interrupts) */
    KV_LOCK_CAS,      /* plain compare-and-swap spinlock (S32C1I) */
    KV_LOCK_RW,       /* readers-writer spinlock: GETs in parallel, writers exclusive */
    KV_LOCK_SEQ,      /* seqlock: GETs read optimistically without a lock, writers CAS-locked */
};
typedef struct {
    uint32_t acq, contended, retries;
    uint64_t spin_cycles;
} kv_lockstat_t;
void kv_mc_set_mode(int nshards, int lock_mode); /* call before kv_init */
int kv_shard_index(const uint8_t *k, size_t kl);
void kv_mc_stats(kv_lockstat_t out[2]); /* per core */
void kv_mc_reset_stats(void);

/* tick-based clock: 1 ms resolution, wrap-safe comparisons */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline uint32_t kv_now_ms(void) { return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }
static inline int kv_expired(uint32_t deadline, uint32_t now) { return deadline && (int32_t)(now - deadline) >= 0; }
