#include "sdkconfig.h"
#include "route.h"
#if KV_ROUTED
#include <stdint.h>
#include <unistd.h>
#include "esp_vfs_eventfd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { R_SET, R_GET, R_DEL, R_EXISTS, R_PTTL, R_EXPIRE, R_FLUSH, R_STATS };

typedef struct {
    uint8_t op;
    const uint8_t *k, *v;
    size_t kl, vl;
    uint32_t ttl;
    int flags;
    obuf_t *o;
    kv_stats_t *st;
    int64_t r64;
    int r;
    volatile int done;
} rreq_t;

#define RQ 64
typedef struct {
    volatile uint32_t head;
    uint32_t pad0[7];
    volatile uint32_t tail;
    uint32_t pad1[7];
    rreq_t *volatile slot[RQ];
} rring_t;

/* inbound ring per destination: [0], [1] = owner loops; dispatch uses [1] = the worker */
static rring_t rings[2];
static int wake_fd[2] = {-1, -1};

static void exec_req(rreq_t *q)
{
    switch (q->op) {
    case R_SET: q->r = kv_set(q->k, q->kl, q->v, q->vl, q->ttl, q->flags); break;
    case R_GET: q->r = kv_get_resp(q->k, q->kl, q->o); break;
    case R_DEL: q->r = kv_del(q->k, q->kl); break;
    case R_EXISTS: q->r = kv_exists(q->k, q->kl); break;
    case R_PTTL: q->r64 = kv_pttl(q->k, q->kl); break;
    case R_EXPIRE: q->r = kv_expire(q->k, q->kl, q->ttl); break;
    case R_FLUSH: kv_flush(); break;
    case R_STATS: kv_stats(q->st); break;
    }
    __sync_synchronize();
    q->done = 1;
}

static int drain(int dest)
{
    rring_t *r = &rings[dest];
    int n = 0;
    while (r->tail != r->head) {
        __sync_synchronize();
        rreq_t *q = r->slot[r->tail % RQ];
        exec_req(q);
        r->tail++;
        n++;
    }
    return n;
}

#if CONFIG_KV_CORES_OWNER
static inline int me(void) { return xPortGetCoreID(); } /* loop l runs pinned on core l */
static inline int owner_of(const uint8_t *k, size_t kl) { return kv_shard_index(k, kl) & 1; }
#endif

/* run q on dest and wait; single producer per ring (the other loop / the receiver) */
static void forward(int dest, rreq_t *q)
{
    rring_t *r = &rings[dest];
    q->done = 0;
    while (r->head - r->tail >= RQ) {
#if CONFIG_KV_CORES_OWNER
        route_service(me());
#endif
    }
    r->slot[r->head % RQ] = q;
    __sync_synchronize();
    r->head++;
#if CONFIG_KV_CORES_OWNER
    uint64_t one = 1;
    write(wake_fd[dest], &one, sizeof one); /* owner may be asleep in select() */
    while (!q->done) route_service(me()); /* never deadlock: serve what is forwarded to us */
#else
    while (!q->done) {
    }
#endif
    __sync_synchronize();
}

/* local or forwarded execution of one engine call */
static void run(rreq_t *q, int keyed)
{
#if CONFIG_KV_CORES_OWNER
    if (!keyed || owner_of(q->k, q->kl) == me()) {
        exec_req(q); /* global ops (FLUSH, STATS) run locally: not synchronised with the other loop */
        return;
    }
    forward(owner_of(q->k, q->kl), q);
#else
    (void)keyed;
    forward(1, q); /* dispatch: everything runs on the worker */
#endif
}

int r_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, uint32_t ttl_ms, int flags)
{
    rreq_t q = {.op = R_SET, .k = k, .kl = kl, .v = v, .vl = vl, .ttl = ttl_ms, .flags = flags};
    run(&q, 1);
    return q.r;
}
int r_get_resp(const uint8_t *k, size_t kl, obuf_t *o)
{
    rreq_t q = {.op = R_GET, .k = k, .kl = kl, .o = o};
    run(&q, 1);
    return q.r;
}
int r_del(const uint8_t *k, size_t kl)
{
    rreq_t q = {.op = R_DEL, .k = k, .kl = kl};
    run(&q, 1);
    return q.r;
}
int r_exists(const uint8_t *k, size_t kl)
{
    rreq_t q = {.op = R_EXISTS, .k = k, .kl = kl};
    run(&q, 1);
    return q.r;
}
int64_t r_pttl(const uint8_t *k, size_t kl)
{
    rreq_t q = {.op = R_PTTL, .k = k, .kl = kl};
    run(&q, 1);
    return q.r64;
}
int r_expire(const uint8_t *k, size_t kl, uint32_t ttl_ms)
{
    rreq_t q = {.op = R_EXPIRE, .k = k, .kl = kl, .ttl = ttl_ms};
    run(&q, 1);
    return q.r;
}
void r_flush(void)
{
    rreq_t q = {.op = R_FLUSH};
    run(&q, 0);
}
void r_stats(kv_stats_t *s)
{
    rreq_t q = {.op = R_STATS, .st = s};
    run(&q, 0);
}

#if CONFIG_KV_CORES_DISPATCH
/* the worker owns all data: it only executes forwarded calls, plus the TTL sweep when idle */
static void worker_task(void *arg)
{
    uint32_t idle = 0;
    for (;;) {
        if (drain(1)) {
            idle = 0;
            continue;
        }
        if (++idle == 20000) { /* ~quiet for a while: incremental TTL sweep, then yield */
            kv_active_expire(64);
            idle = 0;
            vTaskDelay(1);
        }
    }
}
#endif

void route_start(void)
{
#if CONFIG_KV_CORES_OWNER
    esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&cfg));
    for (int i = 0; i < 2; i++) wake_fd[i] = eventfd(0, 0);
#else
    xTaskCreatePinnedToCore(worker_task, "kvworker", 4096, NULL, 10, NULL, 1);
#endif
}

void route_service(int loop) { drain(loop); }
int route_wake_fd(int loop) { return wake_fd[loop]; }
void route_wake_clear(int loop)
{
    uint64_t v;
    if (wake_fd[loop] >= 0) read(wake_fd[loop], &v, sizeof v);
}
#endif
