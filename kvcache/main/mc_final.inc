/* ===================================================== Part F: final comparison (phase 3)
 * Included into mc_bench.c. No network: tasks call the engine directly.
 * Designs (best variant of each family, plus alternatives):
 *   single_core       one task on core 1, no lock (reference)
 *   shared_seqlock    2 tasks (core 0, core 1), one table, seqlock
 *   shared_rw         2 tasks, one table, readers-writer lock
 *   sharded8_seqlock  2 tasks, 8 shards, seqlock (any task may touch any key)
 *   sharded8_rw       2 tasks, 8 shards, readers-writer lock
 *   owner             2 tasks, 8 shards, each task owns the shards of its parity, no lock
 *                     (requests arrive already routed: the ideal case for ownership)
 *   dispatch_1worker  receiver on core 0 -> lock-free ring -> 1 worker on core 1 (owns all)
 *   dispatch_2owners  receiver on core 0 -> 2 lock-free rings -> 2 shard-owning workers
 *   dispatch_pool     receiver -> one FreeRTOS queue (batches of 16) -> 2 workers, shared table
 *                     with seqlock (any worker takes any batch)
 * Every design: F_REPS repetitions of F_RUN_MS per workload (90 % and 50 % GET, uniform keys),
 * then one correctness run per workload with checked values (see f_get_check). */
#define F_REPS 5
#define F_RUN_MS 3000
#define F_BATCH 16
enum { F_1C, F_DIRECT, F_OWNER, F_DISP1, F_DISP2, F_DISPPOOL };
typedef struct {
    const char *name;
    int kind, nshards, lock;
} fd_t;
static const fd_t FD[] = {
    {"single_core", F_1C, 1, KV_LOCK_NONE},
    {"shared_seqlock", F_DIRECT, 1, KV_LOCK_SEQ},
    {"shared_rw", F_DIRECT, 1, KV_LOCK_RW},
    {"sharded8_seqlock", F_DIRECT, 8, KV_LOCK_SEQ},
    {"sharded8_rw", F_DIRECT, 8, KV_LOCK_RW},
    {"owner", F_OWNER, 8, KV_LOCK_NONE},
    {"dispatch_1worker", F_DISP1, 1, KV_LOCK_NONE},
    {"dispatch_2owners", F_DISP2, 8, KV_LOCK_NONE},
    {"dispatch_pool", F_DISPPOOL, 1, KV_LOCK_SEQ},
};

/* ---- correctness: every value = [key u16][version u32][checksum u32][22 B fill] ---- */
static int f_check;
static uint32_t ver_next[K];          /* touched only by the key's single version-assigner */
static volatile uint32_t ver_pub[K];  /* highest version that may be visible (set BEFORE the SET) */
static uint32_t last_seen[2][K];
typedef struct {
    uint32_t gets, sets, misses, torn, future, backwards;
} fchk_t;
static fchk_t fchk[2];
static const fd_t *f_cur;
static QueueHandle_t f_q;

static uint32_t f_csum(const uint8_t *v)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < VALLEN; i++)
        if (i < 6 || i >= 10) h = (h ^ v[i]) * 16777619u;
    return h;
}

static void mkval(uint8_t *v, int k, uint32_t ver)
{
    v[0] = k, v[1] = k >> 8;
    memcpy(v + 2, &ver, 4);
    for (int i = 10; i < VALLEN; i++) v[i] = (uint8_t)(k * 31 + ver * 7 + i);
    uint32_t c = f_csum(v);
    memcpy(v + 6, &c, 4);
}

static int chkval(const uint8_t *v, int k, uint32_t *ver)
{
    uint32_t c;
    memcpy(&c, v + 6, 4);
    if ((v[0] | v[1] << 8) != k || c != f_csum(v)) return -1;
    memcpy(ver, v + 2, 4);
    return 0;
}

static void ver_pub_max(int k, uint32_t v) /* atomic max: two pool workers may race */
{
    uint32_t cur;
    do {
        cur = ver_pub[k];
        if (cur >= v) return;
    } while (!esp_cpu_compare_and_set(&ver_pub[k], cur, v));
}

static inline void f_get_check(int t, int k, obuf_t *o)
{
    o->len = 0;
    kv_get_resp(keys[k], KEYLEN, o);
    fchk[t].gets++;
    if (o->len == 5 && o->buf[1] == '-') {
        fchk[t].misses++;
        return;
    }
    uint32_t ver;
    if (o->len != 5 + VALLEN + 2 || memcmp(o->buf, "$32\r\n", 5) != 0 || chkval(o->buf + 5, k, &ver)) {
        fchk[t].torn++; /* wrong length, wrong key or bad checksum: a torn or corrupted read */
        return;
    }
    if (ver > ver_pub[k]) fchk[t].future++;     /* a version nobody published: corruption */
    if (ver < last_seen[t][k]) fchk[t].backwards++; /* went back in time for this reader */
    last_seen[t][k] = ver;
}

static inline void f_set(int t, int k, uint32_t ver, uint8_t *vb)
{
    mkval(vb, k, ver);
    ver_pub_max(k, ver); /* publish intent first: a reader may see the value right after kv_set */
    kv_set(keys[k], KEYLEN, vb, VALLEN, 0, 0);
    fchk[t].sets++;
}

/* writer rule for checked runs: key k is written only by the task owning its shard parity */
static inline int f_writer(int k) { return (f_cur->nshards > 1 ? key_shard[k] : k) & 1; }

static void f_direct_task(void *arg)
{
    int t = (int)(intptr_t)arg, solo = f_cur->kind == F_1C;
    uint16_t *s = stream[t];
    uint8_t ob[64], vb[VALLEN];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    uint32_t n = 0, i = 0, ns = 0;
    while (!go) {
    }
    while (!stop) {
        uint16_t op = s[i++ & (STREAM - 1)];
        int k = op & 0x7FFF;
        int sm = !f_check && (i & 15) == 0 && ns < NSAMP;
        uint32_t c0 = sm ? ccount() : 0;
        if (!f_check) {
            if (op & 0x8000) kv_set(keys[k], KEYLEN, val, VALLEN, 0, 0);
            else {
                o.len = 0;
                kv_get_resp(keys[k], KEYLEN, &o);
            }
        } else if ((op & 0x8000) && (solo || f_writer(k) == t)) {
            f_set(t, k, ++ver_next[k], vb);
        } else {
            f_get_check(t, k, &o);
        }
        if (sm) samp[t][ns++] = ccount() - c0;
        n++;
    }
    ops_done[t] = n;
    nsamp[t] = ns;
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

typedef struct {
    uint16_t op[F_BATCH];
    uint32_t ver[F_BATCH];
    uint32_t n;
} fbatch_t;

static void f_receiver(void *arg)
{
    const fd_t *d = f_cur;
    uint16_t *s = stream[0];
    uint32_t i = 0;
    fbatch_t b = {0};
    while (!go) {
    }
    while (!stop) {
        uint16_t op = s[i++ & (STREAM - 1)];
        int k = op & 0x7FFF;
        uint32_t ver = (f_check && (op & 0x8000)) ? ++ver_next[k] : 0; /* single assigner */
        if (d->kind == F_DISPPOOL) {
            b.op[b.n] = op;
            b.ver[b.n++] = ver;
            if (b.n == F_BATCH) {
                while (xQueueSend(f_q, &b, pdMS_TO_TICKS(5)) != pdTRUE && !stop) {
                }
                b.n = 0;
            }
            continue;
        }
        int w = d->kind == F_DISP2 ? (key_shard[k] & 1) : 0;
        ring_t *r = rings[w];
        while (r->head - r->tail >= RING && !stop) taskYIELD();
        uint32_t h = r->head;
        r->buf[h & (RING - 1)] = (req_t){.op = op, .t_us = ver};
        __sync_synchronize();
        r->head = h + 1;
    }
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

static inline void f_exec(int w, uint16_t op, uint32_t ver, obuf_t *o, uint8_t *vb)
{
    int k = op & 0x7FFF;
    if (!f_check) {
        if (op & 0x8000) kv_set(keys[k], KEYLEN, val, VALLEN, 0, 0);
        else {
            o->len = 0;
            kv_get_resp(keys[k], KEYLEN, o);
        }
    } else if (op & 0x8000) {
        f_set(w, k, ver, vb);
    } else {
        f_get_check(w, k, o);
    }
}

static void f_worker(void *arg)
{
    int w = (int)(intptr_t)arg;
    const fd_t *d = f_cur;
    uint8_t ob[64], vb[VALLEN];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    uint32_t n = 0;
    ring_t *r = d->kind == F_DISPPOOL ? NULL : rings[w];
    while (!go) {
    }
    while (!stop) {
        if (!r) {
            fbatch_t b;
            if (xQueueReceive(f_q, &b, pdMS_TO_TICKS(5)) != pdTRUE) continue;
            for (uint32_t j = 0; j < b.n; j++) f_exec(w, b.op[j], b.ver[j], &o, vb);
            n += b.n;
            continue;
        }
        uint32_t t = r->tail, h = r->head;
        if (t == h) {
            taskYIELD();
            continue;
        }
        __sync_synchronize();
        for (; t != h; t++) {
            req_t *it = &r->buf[t & (RING - 1)];
            f_exec(w, it->op, it->t_us, &o, vb);
            n++;
        }
        __sync_synchronize();
        r->tail = t;
    }
    ops_done[w] = n;
    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

/* after the run: every key must hold exactly the last published version */
static uint32_t f_final_check(uint32_t *unreadable)
{
    uint8_t ob[64];
    obuf_t o = {.buf = ob, .cap = sizeof ob};
    uint32_t bad = 0;
    *unreadable = 0;
    for (int k = 0; k < K; k++) {
        o.len = 0;
        kv_get_resp(keys[k], KEYLEN, &o);
        uint32_t ver;
        if (o.len != 5 + VALLEN + 2 || chkval(o.buf + 5, k, &ver)) {
            (*unreadable)++;
            continue;
        }
        if (ver != ver_pub[k]) bad++;
    }
    return bad;
}

static void f_run(const fd_t *d, const wl_t *wl, int rep)
{
    if (setup_store(d->nshards, d->lock) != 0) {
        printf("RESULT {\"error\":\"setup %s\"}\n", d->name);
        return;
    }
    f_cur = d;
    if (f_check) { /* preload checked values, version 0 */
        uint8_t vb[VALLEN];
        for (int k = 0; k < K; k++) {
            mkval(vb, k, 0);
            kv_set(keys[k], KEYLEN, vb, VALLEN, 0, 0);
            ver_next[k] = 0;
            ver_pub[k] = 0;
            last_seen[0][k] = last_seen[1][k] = 0;
        }
        memset(fchk, 0, sizeof fchk);
    }
    int disp = d->kind >= F_DISP1;
    int nw = d->kind == F_1C || d->kind == F_DISP1 ? 1 : 2;
    if (disp) build_stream(0, wl, -1);
    else
        for (int t = 0; t < nw; t++) build_stream(t, wl, d->kind == F_OWNER ? t : -1);
    ops_done[0] = ops_done[1] = 0;
    nsamp[0] = nsamp[1] = 0;
    f_q = NULL;
    if (disp) {
        if (d->kind == F_DISPPOOL) f_q = xQueueCreate(8, sizeof(fbatch_t));
        else
            for (int i = 0; i < 2; i++) rings[i] = heap_caps_calloc(1, sizeof(ring_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        xTaskCreatePinnedToCore(f_receiver, "frx", 4096, NULL, WPRIO, NULL, 0);
        if (d->kind == F_DISP1) xTaskCreatePinnedToCore(f_worker, "fw", 4096, (void *)0, WPRIO, NULL, 1);
        else
            for (int i = 0; i < 2; i++) xTaskCreatePinnedToCore(f_worker, "fw", 4096, (void *)(intptr_t)i, WPRIO, NULL, i);
    } else {
        for (int t = 0; t < nw; t++)
            xTaskCreatePinnedToCore(f_direct_task, "fd", 4096, (void *)(intptr_t)t, WPRIO, NULL, nw == 1 ? 1 : t);
    }
    uint64_t i0[2], i1[2];
    int64_t win = run_window(nw + disp, i0, i1);
    uint32_t tot = ops_done[0] + ops_done[1];
    if (nw == 1) nsamp[1] = 0;
    uint32_t p[3];
    pcts(p);
    if (!f_check) {
        printf("RESULT {\"part\":\"final\",\"design\":\"%s\",\"workload\":\"%s\",\"rep\":%d,\"run_ms\":%d,"
               "\"ops_per_s\":%" PRIu64 ",\"core0_ops_per_s\":%" PRIu64 ",\"core1_ops_per_s\":%" PRIu64
               ",\"p50_ns\":%" PRIu32 ",\"p99_ns\":%" PRIu32 ",\"p999_ns\":%" PRIu32 ",\"cpu0_pct\":%.1f,\"cpu1_pct\":%.1f}\n",
               d->name, wl->name, rep, RUN_MS, (uint64_t)tot * 1000000 / win, (uint64_t)ops_done[0] * 1000000 / win,
               (uint64_t)ops_done[1] * 1000000 / win, disp ? 0 : p[0] * 1000 / 240, disp ? 0 : p[1] * 1000 / 240,
               disp ? 0 : p[2] * 1000 / 240, 100.0 * (1.0 - (double)(i1[0] - i0[0]) / win),
               100.0 * (1.0 - (double)(i1[1] - i0[1]) / win));
    } else {
        uint32_t unreadable, final_bad = f_final_check(&unreadable);
        fchk_t a = {0};
        for (int t = 0; t < 2; t++) {
            a.gets += fchk[t].gets, a.sets += fchk[t].sets, a.misses += fchk[t].misses;
            a.torn += fchk[t].torn, a.future += fchk[t].future, a.backwards += fchk[t].backwards;
        }
        int ok = !a.torn && !a.future && !a.backwards && !a.misses && !final_bad && !unreadable;
        printf("RESULT {\"part\":\"final_check\",\"design\":\"%s\",\"workload\":\"%s\",\"run_ms\":%d,\"ops\":%" PRIu32
               ",\"gets_checked\":%" PRIu32 ",\"sets\":%" PRIu32 ",\"torn\":%" PRIu32 ",\"future\":%" PRIu32
               ",\"backwards\":%" PRIu32 ",\"misses\":%" PRIu32 ",\"final_mismatch\":%" PRIu32
               ",\"final_unreadable\":%" PRIu32 ",\"pass\":%d}\n",
               d->name, wl->name, RUN_MS, tot, a.gets, a.sets, a.torn, a.future, a.backwards, a.misses, final_bad,
               unreadable, ok);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (f_q) vQueueDelete(f_q);
    f_q = NULL;
    for (int i = 0; i < 2; i++) {
        if (rings[i]) heap_caps_free(rings[i]);
        rings[i] = NULL;
    }
}

static void mc_phase3(void)
{
    g_run_ms = F_RUN_MS;
    const int nd = sizeof FD / sizeof FD[0];
    printf("MC final: %d designs x 2 workloads x %d reps x %d ms, then correctness runs\n", nd, F_REPS, F_RUN_MS);
    for (int wi = 0; wi < 2; wi++) /* g90_uniform, g50_uniform */
        for (int rep = 0; rep < F_REPS; rep++)
            for (int di = 0; di < nd; di++) f_run(&FD[di], &WL[wi], rep);
    f_check = 1;
    for (int wi = 0; wi < 2; wi++)
        for (int di = 0; di < nd; di++) f_run(&FD[di], &WL[wi], 0);
    f_check = 0;
}
