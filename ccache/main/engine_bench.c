/* On-device engine benchmark, mirrors micropython/mr_engine_bench.py exactly:
 * 16 B keys "key:%012d", 32 B values, N = 500 keys, then fill until full.
 * Runs before WiFi starts; prints one "RESULT {json}" line on the console. */
#include <stdio.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "kv.h"
#include "cmd.h"

#define N 500
#define REPS 20
#define KEYLEN 16
#define VALLEN 32
#define CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)

static void mk_seq(uint8_t *k, int i) { snprintf((char *)k, KEYLEN + 1, "key:%012d", i); }
static void mk_rand(uint8_t *k, int i)
{
    static const char hx[] = "0123456789abcdef";
    uint32_t a = esp_random(), b = esp_random();
    for (int j = 0; j < 8; j++) {
        k[j] = hx[(a >> (4 * j)) & 15];
        k[8 + j] = hx[(b >> (4 * j)) & 15];
    }
}

typedef struct {
    uint32_t set, get, del;
} ops_t;

static ops_t run_ops(uint8_t (*keys)[KEYLEN + 1])
{
    static uint8_t val[VALLEN];
    static uint8_t ob[64];
    memset(val, 'v', VALLEN);
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    int64_t ts = 0, tg = 0, td = 0;
    for (int r = 0; r < REPS; r++) {
        int64_t t = esp_timer_get_time();
        for (int i = 0; i < N; i++) kv_set(keys[i], KEYLEN, val, VALLEN, 0, 0);
        ts += esp_timer_get_time() - t;
        t = esp_timer_get_time();
        for (int i = 0; i < N; i++) {
            o.len = 0;
            kv_get_resp(keys[i], KEYLEN, &o);
        }
        tg += esp_timer_get_time() - t;
        t = esp_timer_get_time();
        for (int i = 0; i < N; i++) kv_del(keys[i], KEYLEN);
        td += esp_timer_get_time() - t;
    }
    ops_t r = {(uint32_t)((int64_t)N * REPS * 1000000 / ts), (uint32_t)((int64_t)N * REPS * 1000000 / tg),
               (uint32_t)((int64_t)N * REPS * 1000000 / td)};
    return r;
}

void engine_bench_run(size_t reserve)
{
    static uint8_t keys[N][KEYLEN + 1];
    static uint8_t val[VALLEN];
    memset(val, 'v', VALLEN);
    size_t heap0 = heap_caps_get_free_size(CAPS);
    if (kv_init(reserve) != 0) {
        printf("RESULT {\"error\":\"kv_init\"}\n");
        return;
    }
    kv_set_evict(0);

    for (int i = 0; i < N; i++) mk_seq(keys[i], i);
    ops_t seq = run_ops(keys);
    for (int i = 0; i < N; i++) mk_rand(keys[i], i);
    ops_t rnd = run_ops(keys);

    /* bytes per key: store N fresh keys */
    kv_flush();
    size_t heap_a = heap_caps_get_free_size(CAPS);
    uint8_t k[KEYLEN + 1];
    for (int i = 0; i < N; i++) {
        mk_seq(k, i);
        kv_set(k, KEYLEN, val, VALLEN, 0, 0);
    }
    size_t heap_b = heap_caps_get_free_size(CAPS);
    kv_stats_t st;
    kv_stats(&st);
    /* opt engine: arena is preallocated, so use its own accounting (slab chunk + index share);
       basic engine: real heap consumption including allocator overhead */
    uint32_t bpk = (heap_a > heap_b) ? (uint32_t)((heap_a - heap_b) / N) : st.mem_used / N;

    /* max keys until the store refuses (noeviction) */
    int i = N;
    int64_t tf = esp_timer_get_time();
    for (;; i++) {
        mk_seq(k, i);
        if (kv_set(k, KEYLEN, val, VALLEN, 0, 0) != KV_OK) break;
        if (heap_caps_get_free_size(CAPS) < 8192) break; /* basic engine: keep the system alive */
    }
    tf = esp_timer_get_time() - tf;
    kv_stats(&st);
    printf("RESULT {\"impl\":\"ccache\",\"engine\":\"%s\",\"key_len\":%d,\"val_len\":%d,"
           "\"set_ops\":%" PRIu32 ",\"get_ops\":%" PRIu32 ",\"del_ops\":%" PRIu32 ","
           "\"rand_set_ops\":%" PRIu32 ",\"rand_get_ops\":%" PRIu32 ",\"rand_del_ops\":%" PRIu32 ","
           "\"bytes_per_key\":%" PRIu32 ",\"max_keys\":%d,\"fill_ops\":%u,\"store_bytes\":%" PRIu32
           ",\"heap_before\":%u,\"reserve\":%u}\n",
           kv_engine_name(), KEYLEN, VALLEN, seq.set, seq.get, seq.del, rnd.set, rnd.get, rnd.del, bpk, i,
           (unsigned)((int64_t)(i - N) * 1000000 / (tf ? tf : 1)), st.mem_total, (unsigned)heap0,
           (unsigned)reserve);
    kv_deinit();
}
