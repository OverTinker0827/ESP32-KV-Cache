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
int kv_get_evict(void);
void kv_stats(kv_stats_t *s);
const char *kv_engine_name(void);

/* tick-based clock: 1 ms resolution, wrap-safe comparisons */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline uint32_t kv_now_ms(void) { return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }
static inline int kv_expired(uint32_t deadline, uint32_t now) { return deadline && (int32_t)(now - deadline) >= 0; }
