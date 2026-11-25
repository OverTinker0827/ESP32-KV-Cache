#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_attr.h"

/* HOT: code executed per request. With KV_HOT_IRAM it lives in IRAM so it never
 * stalls on a flash-cache miss (the cache is shared with WiFi/lwIP code that
 * thrashes it under load). */
#if CONFIG_KV_HOT_IRAM
#define HOT IRAM_ATTR
#define KV_IRAM_ON 1
#else
#define HOT
#define KV_IRAM_ON 0
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static inline uint32_t ccount(void)
{
    uint32_t c;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
    return c;
}

/* ---------- output buffer the command layer writes RESP replies into ---------- */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    int overflow; /* set if a reply did not fit (caller flushes and retries) */
} obuf_t;

static inline int ob_room(obuf_t *o, size_t n) { return o->len + n <= o->cap; }

/* ---------- server statistics (shared by all variants) ---------- */
typedef struct {
    volatile uint32_t cmds;
    volatile uint32_t conns;
    volatile uint32_t batches;      /* recv() calls that yielded >=1 command */
    volatile uint64_t exec_cycles;  /* CPU cycles spent inside command execution */
    volatile uint32_t svc_hist[24]; /* log2 histogram, server residence per batch in us */
} srv_stats_t;
extern srv_stats_t g_stats;

static inline void hist_add(volatile uint32_t *h, uint32_t us)
{
    int b = us ? 32 - __builtin_clz(us) : 0;
    if (b > 23) b = 23;
    h[b]++;
}
