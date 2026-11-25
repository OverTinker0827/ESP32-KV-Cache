/* Phase 3 memory benchmark (CONFIG_KV_P3_BENCH), same code for every engine.
 * A. capacity sweep: fill until the store refuses (noeviction), per value size and key style
 *    -> max keys, store bytes per key. v2 engine: with and without prefix compression.
 * B. mixed sizes (16-512 B values): payload bytes / store bytes (fragmentation + overhead).
 * C. (v2 engine) adaptive reserve vs fixed reserves, under a simulated packet-buffer load that
 *    bursts to ~64 KB: failed buffer allocations (= dropped packets), cache capacity, hit rate.
 * Prints "RESULT {json}". */
#include "sdkconfig.h"
#if CONFIG_KV_P3_BENCH
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "kv.h"

#define CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)
static uint8_t vbuf[1024];

static int mk(char *k, int style, int i)
{
    if (style == 0) return snprintf(k, 40, "key:%012d", i);           /* 16 B */
    return snprintf(k, 40, "sensor:temp:room%02d:%05d", i % 20, i / 20); /* 24 B, 20 prefixes */
}

static const char *ENGINE_TAG(void)
{
#if CONFIG_KV_ENGINE_BASIC
    return "basic";
#elif CONFIG_KV_ENGINE_OPT1
    return "opt1";
#elif CONFIG_KV_ENGINE_OPT0
    return "opt0";
#else
    return "opt2";
#endif
}

static uint32_t store_bytes(size_t heap_before)
{
#if CONFIG_KV_ENGINE_BASIC
    return heap_before - heap_caps_get_free_size(CAPS); /* everything the store took from the heap */
#else
    kv_stats_t st;
    kv_stats(&st);
    return st.mem_total; /* arena pages + index */
#endif
}

static void fill(int style, int vsz, int prefix)
{
    char k[40];
    kv_flush();
#if CONFIG_KV_ENGINE_OPT
    kv_set_prefix(prefix);
#endif
    size_t h0 = heap_caps_get_free_size(CAPS);
    memset(vbuf, 'v', sizeof vbuf);
    int i = 0;
    for (;; i++) {
        int kl = mk(k, style, i);
        if (kv_set((uint8_t *)k, kl, vbuf, vsz, 0, 0) != KV_OK) break;
        if (heap_caps_get_free_size(CAPS) < 8192) break; /* basic engine: keep the system alive */
    }
    uint32_t sb = store_bytes(h0);
    int kl = style ? 24 : 16;
    printf("RESULT {\"part\":\"sweep\",\"engine\":\"%s\",\"keys\":\"%s\",\"key_len\":%d,\"val_len\":%d,\"prefix\":%d,"
           "\"max_keys\":%d,\"store_bytes\":%" PRIu32 ",\"bytes_per_key\":%.1f,\"payload_eff\":%.3f}\n",
           ENGINE_TAG(), style ? "iot" : "seq", kl, vsz, prefix, i, sb, i ? (double)sb / i : 0.0,
           sb ? (double)i * (kl + vsz) / sb : 0.0);
}

static void mixed(int prefix)
{
    char k[40];
    kv_flush();
#if CONFIG_KV_ENGINE_OPT
    kv_set_prefix(prefix);
#endif
    size_t h0 = heap_caps_get_free_size(CAPS);
    uint64_t payload = 0;
    uint32_t x = 777;
    int i = 0, stored = 0, rejected = 0, consec = 0;
    /* a rejected SET does not stop a cache: keep inserting until 200 rejections in a row
       (v1 of this test stopped at the first one, measuring the first full size class) */
    for (;; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        int vsz = 16 + x % 497;
        int kl = mk(k, 1, i);
        if (kv_set((uint8_t *)k, kl, vbuf, vsz, 0, 0) == KV_OK) {
            payload += kl + vsz;
            stored++;
            consec = 0;
        } else {
            rejected++;
            if (++consec == 200) break;
        }
        if (heap_caps_get_free_size(CAPS) < 8192) break;
    }
    uint32_t sb = store_bytes(h0);
    printf("RESULT {\"part\":\"mixed\",\"engine\":\"%s\",\"prefix\":%d,\"max_keys\":%d,\"rejected\":%d,"
           "\"store_bytes\":%" PRIu32 ",\"payload_bytes\":%" PRIu64 ",\"payload_eff\":%.3f}\n",
           ENGINE_TAG(), prefix, stored, rejected, sb, payload, sb ? (double)payload / sb : 0.0);
}

/* ------------------------------------------------- C. adaptive reserve under buffer bursts */
#if CONFIG_KV_ENGINE_OPT
#define NBUF 64
#define BUFSZ 1600
static volatile int pr_run, pr_go;
static volatile uint32_t pr_attempts, pr_fails;
static volatile int pr_target;
static SemaphoreHandle_t pr_done;

/* stands in for WiFi/lwIP packet buffers: holds a baseline of 6 buffers (~10 KB) and every
   1.5 s bursts to 40 (~64 KB) for 300 ms, allocating at most 8 buffers per ms (a fast ramp) */
static void pressure_task(void *arg)
{
    static void *bufs[NBUF];
    int held = 0;
    int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    while (!pr_go) vTaskDelay(1);
    while (pr_run) {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        pr_target = (ms % 1500) < 300 ? 40 : 6;
        for (int n = 0; n < 8 && held < pr_target; n++) {
            pr_attempts++;
            void *b = heap_caps_malloc(BUFSZ, CAPS);
            if (!b) {
                pr_fails++;
                break;
            }
            memset(b, 0x5a, 64);
            bufs[held++] = b;
        }
        while (held > pr_target) heap_caps_free(bufs[--held]);
        vTaskDelayUntil(&last, 1);
    }
    while (held) heap_caps_free(bufs[--held]);
    xSemaphoreGive(pr_done);
    vTaskDelete(NULL);
}

typedef struct {
    const char *name;
    size_t init_reserve, low, high; /* low = 0: fixed reserve (no adjustment) */
} amode_t;

static void adaptive_run(const amode_t *m, int run_ms)
{
    char k[40];
    kv_deinit();
    kv_set_prefix(0);
    if (kv_init(m->init_reserve) != 0) {
        printf("RESULT {\"error\":\"init %s\"}\n", m->name);
        return;
    }
    kv_set_evict(1); /* a cache: make room by evicting */
    pr_attempts = pr_fails = 0;
    pr_run = 1;
    pr_go = 0;
    xTaskCreatePinnedToCore(pressure_task, "press", 3072, NULL, 22, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    pr_go = 1;
    /* cache-aside workload: working set of 6000 keys (more than fits), GET, on miss SET */
    const int W = 6000;
    uint32_t gets = 0, hits = 0, adj_calls = 0;
    uint64_t pages_acc = 0;
    int pages_min = 1 << 30, pages_max = 0, released = 0, grown = 0;
    size_t heap_min = SIZE_MAX;
    uint8_t ob[64];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    int64_t t_end = esp_timer_get_time() + (int64_t)run_ms * 1000, next_adj = 0;
    while (esp_timer_get_time() < t_end) {
        int i = esp_random() % W;
        int kl = mk(k, 0, i);
        o.len = 0;
        gets++;
        if (kv_get_resp((uint8_t *)k, kl, &o) == KV_OK) hits++;
        else kv_set((uint8_t *)k, kl, vbuf, 32, 0, 0);
        int64_t now = esp_timer_get_time();
        if (now >= next_adj) { /* controller tick every 2 ms (event-loop idle tick in the server) */
            next_adj = now + 2000;
            if (m->low) {
                int d = kv_mem_adjust(m->low, m->high);
                if (d < 0) released -= d;
                else grown += d;
            }
            adj_calls++;
            int p = kv_pages_live();
            pages_acc += p;
            if (p < pages_min) pages_min = p;
            if (p > pages_max) pages_max = p;
            size_t f = heap_caps_get_free_size(CAPS);
            if (f < heap_min) heap_min = f;
        }
    }
    pr_run = 0;
    xSemaphoreTake(pr_done, portMAX_DELAY);
    printf("RESULT {\"part\":\"adaptive\",\"mode\":\"%s\",\"init_reserve\":%u,\"low\":%u,\"high\":%u,"
           "\"buf_attempts\":%" PRIu32 ",\"buf_fails\":%" PRIu32 ",\"hit_rate\":%.3f,\"gets\":%" PRIu32
           ",\"pages_avg\":%.1f,\"pages_min\":%d,\"pages_max\":%d,\"pages_released\":%d,\"pages_grown\":%d,"
           "\"heap_min\":%u}\n",
           m->name, (unsigned)m->init_reserve, (unsigned)m->low, (unsigned)m->high, pr_attempts, pr_fails,
           gets ? (double)hits / gets : 0.0, gets, adj_calls ? (double)pages_acc / adj_calls : 0.0, pages_min,
           pages_max, released, grown, (unsigned)heap_min);
}
#endif

void p3_bench_run(size_t reserve)
{
    static const int vs[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    if (kv_init(reserve) != 0) {
        printf("RESULT {\"error\":\"kv_init\"}\n");
        return;
    }
    kv_set_evict(0);
    for (int style = 0; style < 2; style++)
        for (int v = 0; v < 8; v++) {
            fill(style, vs[v], 0);
#if CONFIG_KV_ENGINE_OPT
            fill(style, vs[v], 1);
#endif
        }
    mixed(0);
#if CONFIG_KV_ENGINE_OPT
    mixed(1);
    pr_done = xSemaphoreCreateBinary();
    static const amode_t AM[] = {{"fixed_40k", 40 * 1024, 0, 0},
                                 {"fixed_80k", 80 * 1024, 0, 0},
                                 {"fixed_100k", 100 * 1024, 0, 0},
                                 {"adaptive_20k_48k", 40 * 1024, 20 * 1024, 48 * 1024},
                                 {"adaptive_40k_64k", 40 * 1024, 40 * 1024, 64 * 1024},
                                 {"adaptive_56k_80k", 40 * 1024, 56 * 1024, 80 * 1024}};
    for (int i = 0; i < 6; i++) adaptive_run(&AM[i], 6000);
#endif
    kv_deinit();
}
#endif
