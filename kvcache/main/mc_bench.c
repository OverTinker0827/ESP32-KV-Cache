/* Phase 1 multicore harness (CONFIG_KV_MC_BENCH, optimized engine, WiFi off).
 *
 * Part A, direct designs: 1 or 2 tasks pinned to cores run GET/SET back to back against
 *   the engine in a given lock discipline (none / portMUX / CAS / RW / seqlock), with one
 *   table or 2 hash-routed shards, or "owned" shards (each core only gets keys of its own
 *   shard, no lock: the shared-nothing upper bound).
 * Part B, queue designs: a receiver task on core 0 (stands in for the network stack)
 *   hands requests to 1 or 2 worker tasks through a FreeRTOS queue or a lock-free SPSC
 *   ring, one request or a batch of 16 per handoff; 2 workers either share one locked
 *   table or own a shard each (receiver routes by key hash).
 * Part C, statistics counters: per-core vs one shared atomic vs one shared racy counter.
 *
 * Workloads: 1024 random 16 B keys, 32 B values, 90/10 or 50/50 GET/SET, uniform or
 * Zipf(0.99). Every run lasts RUN_MS. Prints "RESULT {json}" lines. */
#include "sdkconfig.h"
#if CONFIG_KV_MC_BENCH
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "kv.h"
#include "cmd.h"

#define K 1024
#define STREAM 4096 /* ops per task stream, replayed cyclically */
#define RUN_MS 400
#define NSAMP 2048
#define KEYLEN 16
#define VALLEN 32
#define BATCH 16
#define RING 256
#define WPRIO 5

static uint8_t keys[K][KEYLEN];
static uint8_t key_shard[K];
static float zcdf[K];
static uint16_t *stream[2]; /* bit15 = SET, low bits = key index */
static uint32_t *samp[2];
static uint32_t nsamp[2];
static uint8_t val[VALLEN];
static volatile int go, stop;
static volatile uint32_t ops_done[2];
static SemaphoreHandle_t done_sem;
static size_t g_reserve;

typedef struct {
    const char *name;
    int get_pct;
    int zipf;
} wl_t;
static const wl_t WL[] = {{"g90_uniform", 90, 0}, {"g50_uniform", 50, 0}, {"g90_zipf", 90, 1}, {"g50_zipf", 50, 1}};

/* ------------------------------------------------------------------ helpers */
static uint32_t rnd(void) { return esp_random(); }

static void build_keys(void)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < K; i++)
        for (int j = 0; j < KEYLEN; j++) keys[i][j] = hx[rnd() & 15];
    /* Zipf(theta=0.99) CDF over ranks; rank r -> key r (keys are random, so no shard bias) */
    double sum = 0;
    for (int i = 0; i < K; i++) sum += 1.0 / pow(i + 1, 0.99);
    double acc = 0;
    for (int i = 0; i < K; i++) {
        acc += 1.0 / pow(i + 1, 0.99) / sum;
        zcdf[i] = (float)acc;
    }
}

static int zipf_key(void)
{
    float u = (rnd() >> 8) * (1.0f / 16777216.0f);
    int lo = 0, hi = K - 1;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (zcdf[m] < u) lo = m + 1;
        else hi = m;
    }
    return lo;
}

/* stream for task t; shard_filter >= 0 keeps only keys of that shard ("owned" designs) */
static void build_stream(int t, const wl_t *w, int shard_filter)
{
    for (int i = 0; i < STREAM;) {
        int k = w->zipf ? zipf_key() : (int)(rnd() % K);
        if (shard_filter >= 0 && key_shard[k] != shard_filter) continue;
        int set = (int)(rnd() % 100) >= w->get_pct;
        stream[t][i++] = (uint16_t)(k | (set ? 0x8000 : 0));
    }
}

static int setup_store(int nshards, int lock)
{
    kv_deinit();
    kv_mc_set_mode(nshards, lock);
    if (kv_init(g_reserve) != 0) return -1;
    kv_set_evict(0);
    for (int i = 0; i < K; i++) {
        kv_set(keys[i], KEYLEN, val, VALLEN, 0, 0);
        key_shard[i] = kv_shard_index(keys[i], KEYLEN);
    }
    kv_mc_reset_stats();
    return 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* merge both tasks' samples, return p50/p99/p99.9 of the raw unit */
static void pcts(uint32_t out[3])
{
    static uint32_t all[2 * NSAMP];
    uint32_t n = 0;
    for (int t = 0; t < 2; t++)
        for (uint32_t i = 0; i < nsamp[t]; i++) all[n++] = samp[t][i];
    if (!n) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    qsort(all, n, sizeof *all, cmp_u32);
    out[0] = all[n / 2];
    out[1] = all[n * 99 / 100];
    out[2] = all[n * 999 / 1000];
}

static void cpu_snapshot(uint64_t idle[2], int64_t *t)
{
    idle[0] = ulTaskGetIdleRunTimeCounterForCore(0);
    idle[1] = ulTaskGetIdleRunTimeCounterForCore(1);
    *t = esp_timer_get_time();
}

/* run the already-started tasks for RUN_MS, stop them, wait for ntasks completions */
static int64_t run_window(int ntasks, uint64_t idle0[2], uint64_t idle1[2])
{
    int64_t t0, t1;
    vTaskDelay(pdMS_TO_TICKS(20)); /* tasks reach the start line */
    cpu_snapshot(idle0, &t0);
    go = 1;
    vTaskDelay(pdMS_TO_TICKS(RUN_MS));
    stop = 1;
    cpu_snapshot(idle1, &t1);
    for (int i = 0; i < ntasks; i++) xSemaphoreTake(done_sem, portMAX_DELAY);
    go = stop = 0;
    return t1 - t0;
}

/* ============================================================ Part A: direct */
typedef struct {
    const char *name;
    int ntasks, nshards, lock, owned;
} dd_t;

static const dd_t DD[] = {
    {"1c_nolock", 1, 1, KV_LOCK_NONE, 0},         {"1c_mux", 1, 1, KV_LOCK_MUX, 0},
    {"1c_cas", 1, 1, KV_LOCK_CAS, 0},             {"2c_shared_mux", 2, 1, KV_LOCK_MUX, 0},
    {"2c_shared_cas", 2, 1, KV_LOCK_CAS, 0},      {"2c_sharded_cas", 2, 2, KV_LOCK_CAS, 0},
    {"2c_shared_rw", 2, 1, KV_LOCK_RW, 0},        {"2c_sharded_rw", 2, 2, KV_LOCK_RW, 0},
    {"2c_shared_seqlock", 2, 1, KV_LOCK_SEQ, 0},  {"2c_sharded_seqlock", 2, 2, KV_LOCK_SEQ, 0},
    {"2c_owned_nolock", 2, 2, KV_LOCK_NONE, 1},
};

static volatile uint32_t shared_ctr; /* Part C */
static volatile int ctr_mode;        /* 0 local, 1 shared atomic, 2 shared racy */

static void IRAM_ATTR atomic_inc(volatile uint32_t *p)
{
    uint32_t v;
    do {
        v = *p;
    } while (!esp_cpu_compare_and_set(p, v, v + 1));
}

static void direct_task(void *arg)
{
    int t = (int)(intptr_t)arg;
    uint16_t *s = stream[t];
    uint8_t ob[64];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    uint32_t n = 0, i = 0, ns = 0;
    int cm = ctr_mode;
    while (!go) {
    }
    while (!stop) {
        uint16_t op = s[i++ & (STREAM - 1)];
        const uint8_t *k = keys[op & 0x7FFF];
        int sm = (i & 15) == 0 && ns < NSAMP;
        uint32_t c0 = sm ? ccount() : 0;
        if (op & 0x8000) kv_set(k, KEYLEN, val, VALLEN, 0, 0);
        else {
            o.len = 0;
            kv_get_resp(k, KEYLEN, &o);
        }
        if (sm) samp[t][ns++] = ccount() - c0;
        if (cm == 1) atomic_inc(&shared_ctr);
        else if (cm == 2) shared_ctr++;
        n++;
    }
    ops_done[t] = n;
    nsamp[t] = ns;
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

static void report_locks(char *buf, size_t cap)
{
    kv_lockstat_t ls[2];
    kv_mc_stats(ls);
    uint64_t acq = ls[0].acq + ls[1].acq, con = ls[0].contended + ls[1].contended;
    uint64_t spin = ls[0].spin_cycles + ls[1].spin_cycles, ret = ls[0].retries + ls[1].retries;
    snprintf(buf, cap,
             "\"lk_acq\":%" PRIu64 ",\"lk_contended_pct\":%.2f,\"lk_spin_cyc_per_contended\":%" PRIu64
             ",\"seq_retries\":%" PRIu64,
             acq, acq ? 100.0 * con / acq : 0.0, con ? spin / con : 0, ret);
}

static void run_direct(const dd_t *d, const wl_t *w, const char *part, const char *label)
{
    if (setup_store(d->nshards, d->lock) != 0) {
        printf("RESULT {\"error\":\"setup %s\"}\n", d->name);
        return;
    }
    for (int t = 0; t < d->ntasks; t++) build_stream(t, w, d->owned ? t : -1);
    shared_ctr = 0;
    /* single-task designs run on core 1; two tasks: task t on core t */
    for (int t = 0; t < d->ntasks; t++)
        xTaskCreatePinnedToCore(direct_task, "mcd", 4096, (void *)(intptr_t)t, WPRIO, NULL,
                                d->ntasks == 1 ? 1 : t);
    uint64_t i0[2], i1[2];
    int64_t win = run_window(d->ntasks, i0, i1);
    uint32_t tot = 0;
    for (int t = 0; t < d->ntasks; t++) tot += ops_done[t];
    if (d->ntasks == 1) nsamp[1] = 0;
    uint32_t p[3];
    pcts(p);
    char lk[200];
    report_locks(lk, sizeof lk);
    kv_lockstat_t ls[2];
    kv_mc_stats(ls);
    double spin_pct = 100.0 * (double)(ls[0].spin_cycles + ls[1].spin_cycles) /
                      ((double)win * 240.0 * d->ntasks);
    printf("RESULT {\"part\":\"%s\",\"design\":\"%s\",\"workload\":\"%s\",\"ntasks\":%d,\"nshards\":%d,"
           "\"ops_per_s\":%" PRIu64 ",\"core0_ops_per_s\":%" PRIu64 ",\"core1_ops_per_s\":%" PRIu64 ","
           "\"p50_ns\":%" PRIu32 ",\"p99_ns\":%" PRIu32 ",\"p999_ns\":%" PRIu32 ",%s,\"spin_pct\":%.2f",
           part, label ? label : d->name, w->name, d->ntasks, d->nshards, (uint64_t)tot * 1000000 / win,
           (uint64_t)(d->ntasks == 1 ? 0 : ops_done[0]) * 1000000 / win,
           (uint64_t)(d->ntasks == 1 ? ops_done[0] : ops_done[1]) * 1000000 / win, p[0] * 1000 / 240,
           p[1] * 1000 / 240, p[2] * 1000 / 240, lk, spin_pct);
    if (ctr_mode) printf(",\"counter_mode\":%d,\"counter_value\":%" PRIu32 ",\"lost_updates\":%" PRIu32, ctr_mode,
                         shared_ctr, tot - shared_ctr);
    printf("}\n");
}

/* ============================================================ Part B: queues */
typedef struct {
    uint16_t op;
    uint16_t pad;
    uint32_t t_us; /* enqueue time for sampled requests, 0 otherwise */
} req_t;

typedef struct {
    uint16_t op[BATCH];
    uint32_t n;
    uint32_t t_us;
} breq_t;

typedef struct {
    volatile uint32_t head;
    uint32_t pad0[7];
    volatile uint32_t tail;
    uint32_t pad1[7];
    req_t buf[RING];
} ring_t;

typedef struct {
    const char *name;
    int workers;  /* 1 or 2 */
    int routed;   /* 2 workers: 1 = own shard each (no lock), 0 = shared table (CAS) */
    int ring;     /* 0 FreeRTOS queue, 1 SPSC ring */
    int batch;    /* requests per handoff: 1 or BATCH */
} qd_t;

static const qd_t QD[] = {
    {"q_1w", 1, 0, 0, 1},          {"q_2w_shared", 2, 0, 0, 1},      {"q_2w_routed", 2, 1, 0, 1},
    {"q_1w_batch16", 1, 0, 0, 16}, {"q_2w_routed_batch16", 2, 1, 0, 16},
    {"ring_1w", 1, 0, 1, 1},       {"ring_2w_routed", 2, 1, 1, 1},   {"ring_2w_routed_batch16", 2, 1, 1, 16},
};

static const qd_t *cur_q;
static QueueHandle_t qh[2];
static ring_t *rings[2];

static inline uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

static inline void exec_op(uint16_t op, obuf_t *o)
{
    const uint8_t *k = keys[op & 0x7FFF];
    if (op & 0x8000) kv_set(k, KEYLEN, val, VALLEN, 0, 0);
    else {
        o->len = 0;
        kv_get_resp(k, KEYLEN, o);
    }
}

static inline int ring_push(ring_t *r, const req_t *it)
{
    uint32_t h = r->head;
    if (h - r->tail == RING) return 0;
    r->buf[h & (RING - 1)] = *it;
    __sync_synchronize();
    r->head = h + 1;
    return 1;
}

static void producer_task(void *arg)
{
    const qd_t *q = cur_q;
    uint16_t *s = stream[0];
    uint32_t i = 0;
    while (!go) {
    }
    if (!q->ring) {
        if (q->batch == 1) {
            while (!stop) {
                req_t r = {.op = s[i & (STREAM - 1)], .t_us = (i & 15) == 0 ? now_us() : 0};
                i++;
                int w = q->workers == 2 && q->routed ? key_shard[r.op & 0x7FFF] : 0;
                xQueueSend(qh[w], &r, pdMS_TO_TICKS(5));
            }
        } else {
            breq_t b[2] = {0};
            while (!stop) {
                uint16_t op = s[i++ & (STREAM - 1)];
                int w = q->workers == 2 && q->routed ? key_shard[op & 0x7FFF] : 0;
                if (b[w].n == 0) b[w].t_us = now_us();
                b[w].op[b[w].n++] = op;
                if (b[w].n == BATCH) {
                    xQueueSend(qh[w], &b[w], pdMS_TO_TICKS(5));
                    b[w].n = 0;
                }
            }
        }
    } else {
        /* SPSC rings: one per worker. Batch mode publishes `head` once per BATCH requests. */
        uint32_t pend[2] = {0};
        while (!stop) {
            uint16_t op = s[i & (STREAM - 1)];
            int w = q->workers == 2 ? key_shard[op & 0x7FFF] : 0;
            ring_t *r = rings[w];
            uint32_t h = r->head + pend[w];
            if (h - r->tail >= RING) { /* full: publish what we have, let workers run */
                if (pend[w]) {
                    __sync_synchronize();
                    r->head = h;
                    pend[w] = 0;
                }
                taskYIELD();
                continue;
            }
            r->buf[h & (RING - 1)] = (req_t){.op = op, .t_us = (i & 15) == 0 ? now_us() : 0};
            i++;
            if (q->batch == 1 || ++pend[w] == BATCH) {
                __sync_synchronize();
                r->head = h + 1;
                pend[w] = 0;
            }
        }
    }
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

static void worker_task(void *arg)
{
    int w = (int)(intptr_t)arg;
    const qd_t *q = cur_q;
    QueueHandle_t mq = qh[q->workers == 2 && q->routed ? w : 0];
    ring_t *r = q->ring ? rings[w] : NULL;
    uint8_t ob[64];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    uint32_t n = 0, ns = 0;
    while (!go) {
    }
    while (!stop) {
        if (r) {
            uint32_t t = r->tail, h = r->head;
            if (t == h) {
                taskYIELD();
                continue;
            }
            __sync_synchronize();
            for (; t != h; t++) {
                req_t *it = &r->buf[t & (RING - 1)];
                exec_op(it->op, &o);
                if (it->t_us && ns < NSAMP) samp[w][ns++] = now_us() - it->t_us;
                n++;
            }
            __sync_synchronize();
            r->tail = t;
        } else if (q->batch == 1) {
            req_t it;
            if (xQueueReceive(mq, &it, pdMS_TO_TICKS(5)) != pdTRUE) continue;
            exec_op(it.op, &o);
            if (it.t_us && ns < NSAMP) samp[w][ns++] = now_us() - it.t_us;
            n++;
        } else {
            breq_t b;
            if (xQueueReceive(mq, &b, pdMS_TO_TICKS(5)) != pdTRUE) continue;
            for (uint32_t j = 0; j < b.n; j++) exec_op(b.op[j], &o);
            if (ns < NSAMP) samp[w][ns++] = now_us() - b.t_us; /* batch completion latency */
            n += b.n;
        }
    }
    ops_done[w] = n;
    nsamp[w] = ns;
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

static void run_queue(const qd_t *q, const wl_t *w)
{
    int nsh = q->workers == 2 ? 2 : 1;
    int lock = (q->workers == 2 && !q->routed) ? KV_LOCK_CAS : KV_LOCK_NONE;
    if (setup_store(nsh, lock) != 0) {
        printf("RESULT {\"error\":\"setup %s\"}\n", q->name);
        return;
    }
    build_stream(0, w, -1);
    cur_q = q;
    size_t isz = q->batch == 1 ? sizeof(req_t) : sizeof(breq_t);
    for (int i = 0; i < 2; i++) {
        qh[i] = q->ring ? NULL : xQueueCreate(q->batch == 1 ? 64 : 8, isz);
        if (q->ring) {
            rings[i] = heap_caps_calloc(1, sizeof(ring_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    ops_done[0] = ops_done[1] = 0;
    nsamp[0] = nsamp[1] = 0;
    /* receiver on core 0 (where lwIP runs); worker 0 on core 1 when alone, else on core w */
    xTaskCreatePinnedToCore(producer_task, "mcp", 4096, NULL, WPRIO, NULL, 0);
    for (int i = 0; i < q->workers; i++)
        xTaskCreatePinnedToCore(worker_task, "mcw", 4096, (void *)(intptr_t)i, WPRIO, NULL,
                                q->workers == 1 ? 1 : i);
    uint64_t i0[2], i1[2];
    int64_t win = run_window(1 + q->workers, i0, i1);
    uint32_t tot = ops_done[0] + ops_done[1];
    uint32_t p[3];
    pcts(p);
    double c0 = 100.0 * (1.0 - (double)(i1[0] - i0[0]) / win), c1 = 100.0 * (1.0 - (double)(i1[1] - i0[1]) / win);
    printf("RESULT {\"part\":\"queue\",\"design\":\"%s\",\"workload\":\"%s\",\"workers\":%d,\"batch\":%d,"
           "\"handoff\":\"%s\",\"ops_per_s\":%" PRIu64 ",\"w0_ops_per_s\":%" PRIu64 ",\"w1_ops_per_s\":%" PRIu64
           ",\"lat_p50_us\":%" PRIu32 ",\"lat_p99_us\":%" PRIu32 ",\"lat_p999_us\":%" PRIu32
           ",\"cpu0_pct\":%.1f,\"cpu1_pct\":%.1f}\n",
           q->name, w->name, q->workers, q->batch, q->ring ? "spsc_ring" : "freertos_queue",
           (uint64_t)tot * 1000000 / win, (uint64_t)ops_done[0] * 1000000 / win,
           (uint64_t)ops_done[1] * 1000000 / win, p[0], p[1], p[2], c0, c1);
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 2; i++) {
        if (qh[i]) vQueueDelete(qh[i]);
        qh[i] = NULL;
        if (rings[i]) heap_caps_free(rings[i]);
        rings[i] = NULL;
    }
}

/* ================================================================== driver */
void mc_bench_run(size_t reserve)
{
    g_reserve = reserve;
    memset(val, 'v', VALLEN);
    done_sem = xSemaphoreCreateCounting(4, 0);
    for (int t = 0; t < 2; t++) {
        stream[t] = malloc(STREAM * sizeof(uint16_t));
        samp[t] = malloc(NSAMP * sizeof(uint32_t));
    }
    build_keys();
    /* the controller must preempt the busy worker tasks to end each window */
    vTaskPrioritySet(NULL, 20);
    printf("MC start: %d direct designs x %d workloads, %d queue designs\n", (int)(sizeof DD / sizeof DD[0]),
           (int)(sizeof WL / sizeof WL[0]), (int)(sizeof QD / sizeof QD[0]));

    for (size_t wi = 0; wi < sizeof WL / sizeof WL[0]; wi++)
        for (size_t di = 0; di < sizeof DD / sizeof DD[0]; di++) run_direct(&DD[di], &WL[wi], "direct", NULL);

    for (size_t wi = 0; wi < sizeof WL / sizeof WL[0]; wi += 2) /* g90_uniform, g90_zipf */
        for (size_t qi = 0; qi < sizeof QD / sizeof QD[0]; qi++) run_queue(&QD[qi], &WL[wi]);

    /* Part C: statistics counters on the owned (lock-free) design */
    static const char *cname[] = {"counters_per_core", "counters_shared_atomic", "counters_shared_racy"};
    for (int m = 0; m < 3; m++) {
        ctr_mode = m;
        run_direct(&DD[sizeof DD / sizeof DD[0] - 1], &WL[0], "counters", cname[m]);
    }
    ctr_mode = 0;
    kv_deinit();
}
#endif /* CONFIG_KV_MC_BENCH */
