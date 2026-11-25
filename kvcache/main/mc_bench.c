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
#include "wifisim.h"

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
static const char *g_scen = "quiet"; /* printed in every RESULT: quiet | wifisim_core0 */

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

/* p99.9 of one task's samples (raw unit); 0 if none */
static uint32_t p999_one(int t)
{
    static uint32_t tmp[NSAMP];
    uint32_t n = nsamp[t];
    if (!n) return 0;
    memcpy(tmp, samp[t], n * sizeof(uint32_t));
    qsort(tmp, n, sizeof *tmp, cmp_u32);
    return tmp[n * 999 / 1000];
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
    /* per core: task t runs on core t for 2-task designs, single task on core 1 */
    uint32_t pc0 = d->ntasks == 1 ? 0 : p999_one(0), pc1 = d->ntasks == 1 ? p999_one(0) : p999_one(1);
    uint32_t p[3];
    pcts(p);
    char lk[200];
    report_locks(lk, sizeof lk);
    kv_lockstat_t ls[2];
    kv_mc_stats(ls);
    double spin_pct = 100.0 * (double)(ls[0].spin_cycles + ls[1].spin_cycles) /
                      ((double)win * 240.0 * d->ntasks);
    printf("RESULT {\"part\":\"%s\",\"scenario\":\"%s\",\"design\":\"%s\",\"workload\":\"%s\",\"ntasks\":%d,\"nshards\":%d,"
           "\"ops_per_s\":%" PRIu64 ",\"core0_ops_per_s\":%" PRIu64 ",\"core1_ops_per_s\":%" PRIu64 ","
           "\"p50_ns\":%" PRIu32 ",\"p99_ns\":%" PRIu32 ",\"p999_ns\":%" PRIu32 ",\"p999_core0_ns\":%" PRIu32 ",\"p999_core1_ns\":%" PRIu32 ",%s,\"spin_pct\":%.2f",
           part, g_scen, label ? label : d->name, w->name, d->ntasks, d->nshards, (uint64_t)tot * 1000000 / win,
           (uint64_t)(d->ntasks == 1 ? 0 : ops_done[0]) * 1000000 / win,
           (uint64_t)(d->ntasks == 1 ? ops_done[0] : ops_done[1]) * 1000000 / win, p[0] * 1000 / 240,
           p[1] * 1000 / 240, p[2] * 1000 / 240, pc0 * 1000 / 240, pc1 * 1000 / 240, lk, spin_pct);
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
    printf("RESULT {\"part\":\"queue\",\"scenario\":\"%s\",\"design\":\"%s\",\"workload\":\"%s\",\"workers\":%d,\"batch\":%d,"
           "\"handoff\":\"%s\",\"ops_per_s\":%" PRIu64 ",\"w0_ops_per_s\":%" PRIu64 ",\"w1_ops_per_s\":%" PRIu64
           ",\"lat_p50_us\":%" PRIu32 ",\"lat_p99_us\":%" PRIu32 ",\"lat_p999_us\":%" PRIu32
           ",\"cpu0_pct\":%.1f,\"cpu1_pct\":%.1f}\n",
           g_scen, q->name, w->name, q->workers, q->batch, q->ring ? "spsc_ring" : "freertos_queue",
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

/* ======================================================= Part D: Phase 2 balancing
 * Receiver on core 0 routes one skewed request stream into two SPSC rings; worker 0 on
 * core 0 (shares the core with the receiver, like the network stack), worker 1 on core 1.
 * Table split into P2_NSH shards; owner[shard] says which worker executes it.
 *   static_hash    owner = shard % 2, never changes, no lock (Phase 1's best design)
 *   roundrobin_rw  every request alternates workers; RW lock per shard (pure work sharing)
 *   spill_rw       to the owner unless its ring holds SPILL_T more requests than the other's
 *                  (teammate's adaptive load balancing); RW lock per shard
 *   migrate        no lock: every EPOCH requests the receiver compares the two rings' average
 *                  backlog and moves one shard from the busier worker to the other. Lock-free
 *                  hand-over: the shard's new requests are held in a side buffer until the old
 *                  owner has drained everything routed to it before the decision, so requests
 *                  for one key never overtake each other.
 *   hotrep_rw      static owners + the top-NHOT keys of each epoch are replicated: GETs of a hot
 *                  key go to the less loaded worker; the non-owner serves them from its replica
 *                  (seqlock, written only by the owner on SET / first GET). RW lock per shard
 *                  for the rare fallback (replica not yet filled / slot reused). */
#define P2_NSH 8
#define EPOCH 2048
#define SPILL_T 32
#define NHOT 8
#define SIDE 192
#define F_REPLICA 1

enum { P2_STATIC, P2_RR, P2_SPILL, P2_MIGRATE, P2_HOTREP };
typedef struct {
    const char *name;
    int mode, lock, hyst;
} p2d_t;
static const p2d_t P2D[] = {{"static_hash", P2_STATIC, KV_LOCK_NONE, 0},
                            {"roundrobin_rw", P2_RR, KV_LOCK_RW, 0},
                            {"spill_rw", P2_SPILL, KV_LOCK_RW, 0},
                            {"migrate", P2_MIGRATE, KV_LOCK_NONE, 0},
                            {"migrate_hyst", P2_MIGRATE, KV_LOCK_NONE, 1},
                            {"hotrep_rw", P2_HOTREP, KV_LOCK_RW, 0}};

typedef struct {
    const char *name;
    int get_pct;
    float theta; /* 0 = uniform */
    int hotshard; /* 1: the hottest ranks are all keys of shard 0 (owned by worker 0, the receiver's core) */
    int reps;     /* random hot-key placement: repeat with a fresh placement; every design of a
                     repetition runs on the same placement and the same request stream */
} p2w_t;
static const p2w_t P2W[] = {{"uniform", 90, 0, 0, 1},
                            {"zipf099", 90, 0.99f, 0, 3},
                            {"zipf12", 90, 1.2f, 0, 3},
                            {"hotshard_zipf12", 90, 1.2f, 1, 1},
                            {"hotshard_zipf12_g50", 50, 1.2f, 1, 1}};
static int p2_rep;

typedef struct {
    volatile uint32_t seq;
    volatile uint16_t key;
    volatile uint8_t valid;
    uint8_t v[VALLEN];
} replica_t;

static uint16_t rank2key[K];
static uint8_t owner[P2_NSH];
static volatile int8_t hotslot[K]; /* -1 = not hot */
static replica_t rep[2][NHOT];      /* rep[w] is read by worker w, written by the other worker (owner) */
static const p2d_t *p2cur;
static volatile uint32_t p2_spills, p2_migrations, p2_rep_hits, p2_rep_fallbacks, p2_side_stalls;

static void build_zipf_theta(float theta)
{
    if (theta <= 0) return;
    double sum = 0, acc = 0;
    for (int i = 0; i < K; i++) sum += 1.0 / pow(i + 1, theta);
    for (int i = 0; i < K; i++) {
        acc += 1.0 / pow(i + 1, theta) / sum;
        zcdf[i] = (float)acc;
    }
}

static void build_rank_map(int hotshard)
{
    if (!hotshard) { /* fresh random placement of ranks onto keys (Fisher-Yates) */
        for (int i = 0; i < K; i++) rank2key[i] = i;
        for (int i = K - 1; i > 0; i--) {
            int j = rnd() % (i + 1);
            uint16_t t = rank2key[i];
            rank2key[i] = rank2key[j];
            rank2key[j] = t;
        }
        return;
    }
    int r = 0;
    for (int k = 0; k < K; k++)
        if (key_shard[k] == 0) rank2key[r++] = k; /* hottest ranks -> shard 0 */
    for (int k = 0; k < K; k++)
        if (key_shard[k] != 0) rank2key[r++] = k;
}

static void build_p2_stream(const p2w_t *w)
{
    for (int i = 0; i < STREAM; i++) {
        int k = w->theta > 0 ? rank2key[zipf_key()] : (int)(rnd() % K);
        int set = (int)(rnd() % 100) >= w->get_pct;
        stream[0][i] = (uint16_t)(k | (set ? 0x8000 : 0));
    }
}

static inline uint32_t backlog(int w) { return rings[w]->head - rings[w]->tail; }

/* blocking push (receiver is the only producer of each ring) */
static inline void p2_push(int w, uint16_t op, uint16_t flags, uint32_t t_us)
{
    ring_t *r = rings[w];
    while (r->head - r->tail >= RING) {
        if (stop) return;
        taskYIELD();
    }
    uint32_t h = r->head;
    r->buf[h & (RING - 1)] = (req_t){.op = op, .pad = flags, .t_us = t_us};
    __sync_synchronize();
    r->head = h + 1;
}

static void hot_epoch(uint16_t *kcnt)
{
    /* top-NHOT keys of this epoch become the hot set; slots are reassigned freely: readers
       verify the key stored in the replica, so a reused slot is detected */
    /* v2: one pass over the counters keeping a sorted top-NHOT (v1 rescanned all K counters
       NHOT times per epoch and made the receiver the bottleneck: 0.66x on uniform traffic) */
    static uint16_t prev[NHOT];
    static int nprev;
    uint16_t top[NHOT], tc[NHOT];
    int n = 0;
    for (int k = 0; k < K; k++) {
        uint16_t c = kcnt[k];
        if (c < 2 || (n == NHOT && c <= tc[NHOT - 1])) continue; /* hot = seen at least twice */
        int j = n < NHOT ? n++ : NHOT - 1;
        while (j > 0 && tc[j - 1] < c) {
            tc[j] = tc[j - 1];
            top[j] = top[j - 1];
            j--;
        }
        tc[j] = c;
        top[j] = k;
    }
    for (int j = 0; j < nprev; j++) hotslot[prev[j]] = -1;
    for (int j = 0; j < n; j++) {
        hotslot[top[j]] = j;
        prev[j] = top[j];
    }
    nprev = n;
    memset(kcnt, 0, K * sizeof(uint16_t));
}

static void p2_receiver(void *arg)
{
    const p2d_t *d = p2cur;
    uint16_t *s = stream[0];
    uint32_t i = 0;
    static uint16_t kcnt[K];
    uint32_t bl_acc[2] = {0}, ep = 0, ecnt[P2_NSH] = {0};
    int persist = 0, cool = 0;
    /* migration state */
    int pend = -1, pend_from = 0, pend_to = 0;
    uint32_t barrier = 0;
    uint16_t side[SIDE];
    uint32_t side_t[SIDE];
    int nside = 0;
    memset(kcnt, 0, sizeof kcnt);
/* hand the shard to its new owner: everything routed before the decision has been executed */
#define FINISH_MIGRATION()                                                                                             \
    do {                                                                                                               \
        owner[pend] = pend_to;                                                                                         \
        for (int j = 0; j < nside; j++) p2_push(pend_to, side[j], 0, side_t[j]);                                       \
        nside = 0;                                                                                                     \
        pend = -1;                                                                                                     \
        p2_migrations++;                                                                                               \
    } while (0)
#define DRAINED() ((int32_t)(rings[pend_from]->tail - barrier) >= 0)
    while (!go) {
    }
    while (!stop) {
        if (pend >= 0 && DRAINED()) FINISH_MIGRATION();
        uint16_t op = s[i & (STREAM - 1)];
        int k = op & 0x7FFF, sh = key_shard[k], isset = op & 0x8000;
        uint32_t t_us = (i & 15) == 0 ? now_us() : 0;
        i++;
        int w = owner[sh];
        uint16_t flags = 0;
        switch (d->mode) {
        case P2_RR:
            w = i & 1;
            break;
        case P2_SPILL:
            if (backlog(w) > backlog(w ^ 1) + SPILL_T) {
                w ^= 1;
                p2_spills++;
            }
            break;
        case P2_HOTREP: {
            int hs = hotslot[k];
            if (!isset && hs >= 0) {
                int other = w ^ 1;
                replica_t *rp = &rep[other][hs];
                if (rp->valid && rp->key == k && backlog(other) < backlog(w)) {
                    w = other;
                    flags = F_REPLICA;
                }
            }
            kcnt[k]++;
            break;
        }
        case P2_MIGRATE:
            if (sh == pend) { /* shard in hand-over: hold its requests */
                if (nside < SIDE) {
                    side[nside] = op;
                    side_t[nside++] = t_us;
                    continue;
                }
                /* side buffer full: wait for the old owner to drain, hand over, then route
                   this request to the new owner normally (order preserved: side buffer first) */
                p2_side_stalls++;
                while (!DRAINED() && !stop) taskYIELD();
                if (stop) continue;
                FINISH_MIGRATION();
                w = owner[sh];
            }
            break;
        default:
            break;
        }
        p2_push(w, op, flags, t_us);
        bl_acc[0] += backlog(0);
        bl_acc[1] += backlog(1);
        ecnt[sh]++;
        if (++ep == EPOCH) {
            if (d->mode == P2_MIGRATE && pend < 0) {
                int heavy = bl_acc[0] > bl_acc[1] ? 0 : 1, light = heavy ^ 1;
                /* v1: act on any imbalance > 8 queued requests (thrashed: 55-72 moves in 0.4 s).
                   hyst: imbalance > 32 for 2 epochs in a row, then 4 epochs of cool-down. */
                int imb = bl_acc[heavy] / EPOCH > bl_acc[light] / EPOCH + (d->hyst ? 32 : 8);
                if (cool > 0) cool--;
                persist = imb ? persist + 1 : 0;
                if (imb && (!d->hyst || (persist >= 2 && cool == 0))) {
                    /* move the heavy worker's least-used shard that still has traffic, so a
                       hot shard is never bounced back and forth (it stays where it is) */
                    int owned = 0, pick = -1;
                    for (int x = 0; x < P2_NSH; x++)
                        if (owner[x] == heavy) {
                            owned++;
                            if (ecnt[x] && (pick < 0 || ecnt[x] < ecnt[pick])) pick = x;
                        }
                    if (pick >= 0 && owned > 1) {
                        pend = pick;
                        pend_from = heavy;
                        pend_to = light;
                        barrier = rings[heavy]->head;
                        persist = 0;
                        cool = d->hyst ? 4 : 0;
                    }
                }
            }
            memset(ecnt, 0, sizeof ecnt);
            if (d->mode == P2_HOTREP) hot_epoch(kcnt);
            bl_acc[0] = bl_acc[1] = 0;
            ep = 0;
        }
    }
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

static inline void rep_write(replica_t *r, uint16_t k, const uint8_t *v)
{
    r->seq++;
    __sync_synchronize();
    r->key = k;
    memcpy(r->v, v, VALLEN);
    r->valid = 1;
    __sync_synchronize();
    r->seq++;
}

/* returns 1 if served from the replica */
static inline int rep_read(replica_t *r, uint16_t k, uint8_t *out)
{
    for (int t = 0; t < 4; t++) {
        uint32_t s1 = r->seq;
        if (s1 & 1) continue;
        __sync_synchronize();
        int ok = r->valid && r->key == k;
        if (ok) memcpy(out, r->v, VALLEN);
        __sync_synchronize();
        if (r->seq == s1) return ok;
    }
    return 0;
}

static void p2_worker(void *arg)
{
    int w = (int)(intptr_t)arg;
    ring_t *r = rings[w];
    int hotrep = p2cur->mode == P2_HOTREP;
    uint8_t ob[64], rv[VALLEN];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    uint32_t n = 0, ns = 0;
    while (!go) {
    }
    while (!stop) {
        uint32_t t = r->tail, h = r->head;
        if (t == h) {
            taskYIELD();
            continue;
        }
        __sync_synchronize();
        for (; t != h; t++) {
            req_t *it = &r->buf[t & (RING - 1)];
            uint16_t op = it->op;
            int k = op & 0x7FFF;
            if (it->pad & F_REPLICA) {
                int hs = hotslot[k];
                if (hs >= 0 && rep_read(&rep[w][hs], k, rv)) {
                    p2_rep_hits++;
                } else {
                    exec_op(op, &o); /* fallback: engine read under the shard's RW lock */
                    p2_rep_fallbacks++;
                }
            } else {
                exec_op(op, &o);
                if (hotrep) { /* owner keeps the other worker's replica of a hot key current */
                    int hs = hotslot[k];
                    if (hs >= 0) {
                        replica_t *rp = &rep[w ^ 1][hs];
                        if (op & 0x8000) rep_write(rp, k, val);
                        else if (!(rp->valid && rp->key == k) && o.len >= 5 + VALLEN)
                            rep_write(rp, k, o.buf + 5); /* "$32\r\n" + value */
                    }
                }
            }
            if (it->t_us && ns < NSAMP) samp[w][ns++] = now_us() - it->t_us;
            n++;
        }
        __sync_synchronize();
        r->tail = t;
    }
    ops_done[w] = n;
    nsamp[w] = ns;
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

static void run_p2(const p2d_t *d, const p2w_t *wl)
{
    if (setup_store(P2_NSH, d->lock) != 0) {
        printf("RESULT {\"error\":\"setup %s\"}\n", d->name);
        return;
    }
    for (int x = 0; x < P2_NSH; x++) owner[x] = x & 1;
    for (int k = 0; k < K; k++) hotslot[k] = -1;
    memset(rep, 0, sizeof rep);
    p2_spills = p2_migrations = p2_rep_hits = p2_rep_fallbacks = p2_side_stalls = 0;
    /* offered load per worker under the static owner map (what balancing has to fix) */
    uint32_t off[2] = {0};
    for (int j = 0; j < STREAM; j++) off[owner[key_shard[stream[0][j] & 0x7FFF]]]++;
    p2cur = d;
    for (int i = 0; i < 2; i++) rings[i] = heap_caps_calloc(1, sizeof(ring_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ops_done[0] = ops_done[1] = 0;
    nsamp[0] = nsamp[1] = 0;
    xTaskCreatePinnedToCore(p2_receiver, "p2r", 4096, NULL, WPRIO, NULL, 0);
    for (int i = 0; i < 2; i++) xTaskCreatePinnedToCore(p2_worker, "p2w", 4096, (void *)(intptr_t)i, WPRIO, NULL, i);
    uint64_t i0[2], i1[2];
    int64_t win = run_window(3, i0, i1);
    uint32_t tot = ops_done[0] + ops_done[1];
    kv_lockstat_t ls[2];
    kv_mc_stats(ls);
    uint64_t acq = ls[0].acq + ls[1].acq, con = ls[0].contended + ls[1].contended;
    char own[P2_NSH + 1];
    for (int x = 0; x < P2_NSH; x++) own[x] = '0' + owner[x];
    own[P2_NSH] = 0;
    printf("RESULT {\"part\":\"balance\",\"scenario\":\"%s\",\"design\":\"%s\",\"workload\":\"%s\",\"rep\":%d,\"get_pct\":%d,"
           "\"ops_per_s\":%" PRIu64 ",\"w0_ops_per_s\":%" PRIu64 ",\"w1_ops_per_s\":%" PRIu64
           ",\"offered_w0_pct\":%.1f,\"spills\":%" PRIu32 ",\"migrations\":%" PRIu32 ",\"final_owners\":\"%s\","
           "\"rep_hits\":%" PRIu32 ",\"rep_fallbacks\":%" PRIu32 ",\"side_stalls\":%" PRIu32
           ",\"lk_contended_pct\":%.2f}\n",
           g_scen, d->name, wl->name, p2_rep, wl->get_pct, (uint64_t)tot * 1000000 / win,
           (uint64_t)ops_done[0] * 1000000 / win, (uint64_t)ops_done[1] * 1000000 / win,
           100.0 * off[0] / STREAM, p2_spills, p2_migrations, own, p2_rep_hits, p2_rep_fallbacks, p2_side_stalls,
           acq ? 100.0 * con / acq : 0.0);
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 2; i++) {
        heap_caps_free(rings[i]);
        rings[i] = NULL;
    }
}

/* ============================================ Part E: Phase 1 designs under WiFi-like load */
static void run_under_wifisim(void)
{
    static const int dsel[] = {0, 4, 5, 7, 9, 10}; /* 1c_nolock, shared_cas, sharded_cas, sharded_rw,
                                                      sharded_seqlock, owned_nolock */
    wifisim_cfg_t c = WIFISIM_DEFAULT(0);
    for (int pass = 0; pass < 2; pass++) {
        g_scen = pass ? "wifisim_core0" : "quiet";
        if (pass) {
            wifisim_start(&c);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        for (size_t wi = 0; wi < 2; wi++) /* g90_uniform, g50_uniform */
            for (size_t j = 0; j < sizeof dsel / sizeof dsel[0]; j++)
                run_direct(&DD[dsel[j]], &WL[wi], "direct", NULL);
        run_queue(&QD[6], &WL[0]); /* ring_2w_routed */
        if (pass) wifisim_stop();
    }
    g_scen = "quiet";
}

/* ================================================================== driver */
static void mc_phase1(void) __attribute__((unused));
static void mc_phase2(void) __attribute__((unused));

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
#if CONFIG_KV_MC_PHASE == 2
    mc_phase2();
#else
    mc_phase1();
#endif
    kv_deinit();
}

static void mc_phase2(void)
{
    printf("MC phase 2: %d balancing designs x %d workloads, then Phase 1 designs under wifisim\n",
           (int)(sizeof P2D / sizeof P2D[0]), (int)(sizeof P2W / sizeof P2W[0]));
    setup_store(P2_NSH, KV_LOCK_NONE); /* key -> shard map for 8 shards (same for every design) */
    for (size_t wi = 0; wi < sizeof P2W / sizeof P2W[0]; wi++)
        for (p2_rep = 0; p2_rep < P2W[wi].reps; p2_rep++) {
            build_zipf_theta(P2W[wi].theta);
            build_rank_map(P2W[wi].hotshard);
            build_p2_stream(&P2W[wi]);
            for (size_t di = 0; di < sizeof P2D / sizeof P2D[0]; di++) run_p2(&P2D[di], &P2W[wi]);
        }
    build_zipf_theta(0.99f); /* Phase 1 workloads use Zipf 0.99 */
    run_under_wifisim();
}

static void mc_phase1(void)
{
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
}
#endif /* CONFIG_KV_MC_BENCH */
