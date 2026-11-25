/* On-device engine benchmark (v2). Workload mirrors micropython/mr_engine_bench.py:
 * 16 B keys ("key:%012d" sequential, or random hex), 32 B values, N = 500 keys.
 *
 * Per scenario: SET, GET, DEL over all N keys, REPS times; throughput from wall time,
 * and every operation of the first SAMPLE_REPS reps timed individually with the CPU
 * cycle counter -> p50/p95/p99 latency in ns (includes any preemption: intended).
 *
 * Scenarios (WiFi cannot run on this board, see wifisim.h):
 *   quiet           bench on core 1, nothing else running
 *   quiet_core0     bench on core 0, nothing else running
 *   sim_other_core  bench on core 1, WiFi-like load on core 0   (realistic deployment)
 *   sim_same_core   bench on core 0, WiFi-like load on core 0   (worst case)
 * Then one fill run (quiet): bytes/key and max keys.
 * Prints one "RESULT {json}" line per scenario. */
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
#include "cmd.h"
#include "sysmon.h"
#include "wifisim.h"

#define N 500
#define REPS 20
#define SAMPLE_REPS 4
#define NS (N * SAMPLE_REPS)
#define KEYLEN 16
#define VALLEN 32
#define CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)

typedef struct {
    uint32_t ops[3];       /* set, get, del per second */
    uint32_t pct[3][5];    /* [op][p50,p95,p99,p99.9,max] ns */
} res_t;

static uint8_t keys[N][KEYLEN + 1];
static uint32_t (*samp)[NS]; /* heap, only while timing: must not shrink the store */
static uint8_t val[VALLEN];

static void mk_seq(uint8_t *k, int i) { snprintf((char *)k, KEYLEN + 1, "key:%012d", i); }
static void mk_rand(uint8_t *k)
{
    static const char hx[] = "0123456789abcdef";
    uint32_t a = esp_random(), b = esp_random();
    for (int j = 0; j < 8; j++) {
        k[j] = hx[(a >> (4 * j)) & 15];
        k[8 + j] = hx[(b >> (4 * j)) & 15];
    }
    k[KEYLEN] = 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void percentiles(uint32_t *s, uint32_t out[5])
{
    qsort(s, NS, sizeof *s, cmp_u32);
    const int idx[5] = {NS / 2, NS * 95 / 100, NS * 99 / 100, NS * 999 / 1000, NS - 1};
    for (int i = 0; i < 5; i++) out[i] = (uint32_t)((uint64_t)s[idx[i]] * 1000 / 240); /* cycles @240MHz -> ns */
}

static void run_ops(res_t *r)
{
    static uint8_t ob[64];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    int64_t t[3] = {0};
    for (int rep = 0; rep < REPS; rep++) {
        int sm = rep < SAMPLE_REPS;
        uint32_t *ss = &samp[0][rep * N], *sg = &samp[1][rep * N], *sd = &samp[2][rep * N];
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < N; i++) {
            uint32_t c = ccount();
            kv_set(keys[i], KEYLEN, val, VALLEN, 0, 0);
            if (sm) ss[i] = ccount() - c;
        }
        int64_t t1 = esp_timer_get_time();
        for (int i = 0; i < N; i++) {
            o.len = 0;
            uint32_t c = ccount();
            kv_get_resp(keys[i], KEYLEN, &o);
            if (sm) sg[i] = ccount() - c;
        }
        int64_t t2 = esp_timer_get_time();
        for (int i = 0; i < N; i++) {
            uint32_t c = ccount();
            kv_del(keys[i], KEYLEN);
            if (sm) sd[i] = ccount() - c;
        }
        int64_t t3 = esp_timer_get_time();
        t[0] += t1 - t0;
        t[1] += t2 - t1;
        t[2] += t3 - t2;
    }
    for (int k = 0; k < 3; k++) {
        r->ops[k] = (uint32_t)((int64_t)N * REPS * 1000000 / (t[k] ? t[k] : 1));
        percentiles(samp[k], r->pct[k]);
    }
}

static void print_res(const char *scen, int bcore, int score, int duty, const res_t *sq, const res_t *rn)
{
    printf("RESULT {\"impl\":\"kvcache\",\"engine\":\"%s\",\"flags\":\"%s\",\"scenario\":\"%s\","
           "\"bench_core\":%d,\"sim_core\":%d,\"sim_duty_pct\":%d,"
           "\"set_ops\":%" PRIu32 ",\"get_ops\":%" PRIu32 ",\"del_ops\":%" PRIu32 ","
           "\"set_p50_ns\":%" PRIu32 ",\"set_p95_ns\":%" PRIu32 ",\"set_p99_ns\":%" PRIu32 ","
           "\"get_p50_ns\":%" PRIu32 ",\"get_p95_ns\":%" PRIu32 ",\"get_p99_ns\":%" PRIu32 ","
           "\"del_p50_ns\":%" PRIu32 ",\"del_p95_ns\":%" PRIu32 ",\"del_p99_ns\":%" PRIu32 ","
           "\"set_p999_ns\":%" PRIu32 ",\"set_max_ns\":%" PRIu32 ",\"get_p999_ns\":%" PRIu32
           ",\"get_max_ns\":%" PRIu32 ",\"del_p999_ns\":%" PRIu32 ",\"del_max_ns\":%" PRIu32 ","
           "\"rand_set_ops\":%" PRIu32 ",\"rand_get_ops\":%" PRIu32 ",\"rand_del_ops\":%" PRIu32 ","
           "\"rand_get_p99_ns\":%" PRIu32 "}\n",
           kv_engine_name(), variant_flags(), scen, bcore, score, duty, sq->ops[0], sq->ops[1], sq->ops[2],
           sq->pct[0][0], sq->pct[0][1], sq->pct[0][2], sq->pct[1][0], sq->pct[1][1], sq->pct[1][2], sq->pct[2][0],
           sq->pct[2][1], sq->pct[2][2], sq->pct[0][3], sq->pct[0][4], sq->pct[1][3], sq->pct[1][4], sq->pct[2][3],
           sq->pct[2][4], rn->ops[0], rn->ops[1], rn->ops[2], rn->pct[1][2]);
}

static void fill_test(void)
{
    uint8_t k[KEYLEN + 1];
    kv_flush();
    size_t heap_a = heap_caps_get_free_size(CAPS);
    for (int i = 0; i < N; i++) {
        mk_seq(k, i);
        kv_set(k, KEYLEN, val, VALLEN, 0, 0);
    }
    size_t heap_b = heap_caps_get_free_size(CAPS);
    kv_stats_t st;
    kv_stats(&st);
    /* opt engine: arena preallocated -> its own accounting (chunk + index share);
       basic engine: real heap consumption including allocator overhead */
    uint32_t bpk = (heap_a > heap_b) ? (uint32_t)((heap_a - heap_b) / N) : st.mem_used / N;
    int i = N;
    for (;; i++) {
        mk_seq(k, i);
        if (kv_set(k, KEYLEN, val, VALLEN, 0, 0) != KV_OK) break;
        if (heap_caps_get_free_size(CAPS) < 8192) break; /* basic engine: keep the system alive */
    }
    kv_stats(&st);
    printf("RESULT {\"impl\":\"kvcache\",\"engine\":\"%s\",\"flags\":\"%s\",\"scenario\":\"fill\","
           "\"bytes_per_key\":%" PRIu32 ",\"max_keys\":%d,\"store_bytes\":%" PRIu32 "}\n",
           kv_engine_name(), variant_flags(), bpk, i, st.mem_total);
}

typedef struct {
    const char *name;
    int bench_core, sim_core;
} scen_t;

static const scen_t SCEN[] = {
    {"quiet", 1, -1}, {"quiet_core0", 0, -1}, {"sim_other_core", 1, 0}, {"sim_same_core", 0, 0}};
static volatile int cur;
static SemaphoreHandle_t step_done;

static void bench_task(void *arg)
{
    const scen_t *s = &SCEN[cur];
    res_t sq, rn;
    kv_flush();
    for (int i = 0; i < N; i++) mk_seq(keys[i], i);
    run_ops(&sq);
    for (int i = 0; i < N; i++) mk_rand(keys[i]);
    run_ops(&rn);
    print_res(s->name, s->bench_core, s->sim_core, s->sim_core >= 0 ? wifisim_measured_duty_pct() : 0, &sq, &rn);
    xSemaphoreGive(step_done);
    vTaskDelete(NULL);
}

void engine_bench_run(size_t reserve)
{
    memset(val, 'v', VALLEN);
    step_done = xSemaphoreCreateBinary();
    /* timing phase: sample buffers first, then the store (500 keys need little memory) */
    samp = malloc(sizeof(uint32_t) * 3 * NS);
    if (!samp) {
        printf("RESULT {\"error\":\"samples\"}\n");
        return;
    }
    if (kv_init(reserve) != 0) {
        printf("RESULT {\"error\":\"kv_init\"}\n");
        return;
    }
    kv_set_evict(0);
    for (cur = 0; cur < (int)(sizeof SCEN / sizeof SCEN[0]); cur++) {
        const scen_t *s = &SCEN[cur];
        if (s->sim_core >= 0) {
            wifisim_cfg_t c = WIFISIM_DEFAULT(s->sim_core);
            wifisim_start(&c);
            vTaskDelay(pdMS_TO_TICKS(50)); /* let it reach steady state */
        }
        /* priority 5: below the simulator (22), like the cache below the WiFi task */
        xTaskCreatePinnedToCore(bench_task, "kvbench", 6144, NULL, 5, NULL, s->bench_core);
        xSemaphoreTake(step_done, portMAX_DELAY);
        if (s->sim_core >= 0) wifisim_stop();
    }
    /* capacity phase: drop the samples and re-carve the store from the full heap,
       exactly as in entry 1, then fill from the main task */
    kv_deinit();
    free(samp);
    samp = NULL;
    if (kv_init(reserve) != 0) {
        printf("RESULT {\"error\":\"kv_init2\"}\n");
        return;
    }
    kv_set_evict(0);
    fill_test();
    kv_deinit();
}
