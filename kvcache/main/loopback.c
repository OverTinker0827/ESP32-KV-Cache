/* End-to-end test of the REAL server without WiFi: client tasks on the board connect to
 * 127.0.0.1:6379 through lwIP's loopback interface, so requests go through sockets, the TCP
 * stack, the RESP parser, the event loop(s) and the engine exactly as network requests would.
 * Loopback runs TCP twice (client and server side) on the same chip, so absolute numbers are
 * a lower bound on what the server costs; they are for comparing server designs.
 *
 * 1. Functional suite: exact expected replies (PING, SET/GET, NX/XX, DEL, EXISTS, TTL/PTTL,
 *    expiry, pipelining, inline commands, 3 KB and 10 KB values, FLUSHALL, unknown command).
 * 2. Benchmark: C connections (client tasks pinned to core 0, where the network stack lives),
 *    each sending batches of P pipelined GETs or SETs for RUN_MS; with and without the
 *    WiFi-like load simulator on core 0. Prints "RESULT {json}" / "TEST ..." lines. */
#include "sdkconfig.h"
#if CONFIG_KV_LOOPBACK_TEST
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "common.h"
#include "kv.h"
#include "server.h"
#include "sysmon.h"
#include "wifisim.h"
#if CONFIG_LWIP_STATS
#include "lwip/stats.h"
#include "lwip/priv/tcp_priv.h"
#endif

static int g_lb_run_ms = 1000; /* final mode: 2000 */
#define RUN_MS g_lb_run_ms
static int g_rep;
#if CONFIG_LWIP_TCPIP_CORE_LOCKING
#define CORE_LOCKING_ON 1
#else
#define CORE_LOCKING_ON 0
#endif
#define NKEYS 1000
#define VALLEN 32
#define MAXP 16
#define NSAMP 1024

static int lb_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(KV_PORT)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0) {
        printf("LBDIAG socket() failed errno=%d\n", errno);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("LBDIAG connect() failed errno=%d\n", errno);
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static int send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        int w = send(fd, b, n, 0);
        if (w <= 0) return -1;
        b += w;
        n -= w;
    }
    return 0;
}

static size_t g_recv_got; /* diagnostics: bytes received by the last failing recv_n */
static int recv_n(int fd, uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv(fd, b + got, n - got, 0);
        if (r <= 0) {
            g_recv_got = got;
            return -1;
        }
        got += r;
    }
    return 0;
}

/* RESP-encode argv into out; returns length */
static int resp(char *out, int argc, const char **argv, const int *lens)
{
    int n = sprintf(out, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        int l = lens ? lens[i] : (int)strlen(argv[i]);
        n += sprintf(out + n, "$%d\r\n", l);
        memcpy(out + n, argv[i], l);
        n += l;
        out[n++] = '\r';
        out[n++] = '\n';
    }
    return n;
}

/* ------------------------------------------------------------- functional */
static int pass, fail;

static void check(const char *name, int fd, const char *req, int reqlen, const char *expect, int explen)
{
    static uint8_t got[12000];
    if (reqlen && send_all(fd, req, reqlen) < 0) {
        printf("TEST FAIL %s (send)\n", name);
        fail++;
        return;
    }
    if (recv_n(fd, got, explen) < 0) {
        printf("TEST FAIL %s (no/short reply)\n", name);
        fail++;
        return;
    }
    if (memcmp(got, expect, explen) != 0) {
        printf("TEST FAIL %s: got '%.*s'\n", name, explen > 60 ? 60 : explen, got);
        fail++;
        return;
    }
    pass++;
}

#define CMD(name, fd, expect, ...)                                                                                     \
    do {                                                                                                               \
        const char *av_[] = {__VA_ARGS__};                                                                             \
        char rq_[256];                                                                                                 \
        int rl_ = resp(rq_, sizeof av_ / sizeof av_[0], av_, NULL);                                                    \
        check(name, fd, rq_, rl_, expect, strlen(expect));                                                             \
    } while (0)

static void functional(void)
{
    int fd = lb_connect();
    if (fd < 0) {
        printf("TEST FAIL connect\n");
        fail++;
        return;
    }
    CMD("ping", fd, "+PONG\r\n", "PING");
    CMD("flush", fd, "+OK\r\n", "FLUSHALL");
    CMD("set", fd, "+OK\r\n", "SET", "a", "1");
    CMD("get", fd, "$1\r\n1\r\n", "GET", "a");
    CMD("get_missing", fd, "$-1\r\n", "GET", "nokey");
    CMD("set_nx_exists", fd, "$-1\r\n", "SET", "a", "22", "NX");
    CMD("set_xx_missing", fd, "$-1\r\n", "SET", "b", "x", "XX");
    CMD("set_nx_new", fd, "+OK\r\n", "SET", "b", "x", "NX");
    CMD("overwrite", fd, "+OK\r\n", "SET", "a", "hello");
    CMD("get_overwritten", fd, "$5\r\nhello\r\n", "GET", "a");
    CMD("exists2", fd, ":2\r\n", "EXISTS", "a", "b", "c");
    CMD("dbsize", fd, ":2\r\n", "DBSIZE");
    CMD("del", fd, ":2\r\n", "DEL", "a", "b", "c");
    CMD("exists0", fd, ":0\r\n", "EXISTS", "a");
    CMD("ttl_missing", fd, ":-2\r\n", "TTL", "a");
    CMD("set_noexp", fd, "+OK\r\n", "SET", "p", "v");
    CMD("ttl_noexp", fd, ":-1\r\n", "TTL", "p");
    CMD("set_ex", fd, "+OK\r\n", "SET", "e", "v", "EX", "10");
    CMD("ttl_ex", fd, ":10\r\n", "TTL", "e");
    CMD("expire_missing", fd, ":0\r\n", "EXPIRE", "nokey", "5");
    CMD("set_px", fd, "+OK\r\n", "SET", "t", "v", "PX", "100");
    vTaskDelay(pdMS_TO_TICKS(150));
    CMD("expired", fd, "$-1\r\n", "GET", "t");
    CMD("setex", fd, "+OK\r\n", "SETEX", "s", "100", "val");
    CMD("get_setex", fd, "$3\r\nval\r\n", "GET", "s");
    CMD("unknown", fd, "-ERR unknown command\r\n", "NOSUCHCMD");
    CMD("arity", fd, "-ERR wrong number of arguments\r\n", "GET");
    /* pipelining: three commands in one send, replies in order */
    {
        char rq[256];
        int n = 0;
        const char *a1[] = {"SET", "k1", "v1"}, *a2[] = {"GET", "k1"}, *a3[] = {"PING"};
        n += resp(rq + n, 3, a1, NULL);
        n += resp(rq + n, 2, a2, NULL);
        n += resp(rq + n, 1, a3, NULL);
        const char *ex = "+OK\r\n$2\r\nv1\r\n+PONG\r\n";
        check("pipeline3", fd, rq, n, ex, strlen(ex));
    }
#if !CONFIG_KV_SERVER_BASIC /* the basic port does not parse inline commands */
    check("inline_ping", fd, "PING\r\n", 6, "+PONG\r\n", 7);
#endif
    /* large values: 3 KB fits the 4 KB buffers; 10 KB forces rx and tx buffer growth */
    static char big[10240 + 64], rq[10240 + 64], ex[10240 + 64];
    for (int sz = 3000; sz <= 10000; sz += 7000) {
        for (int i = 0; i < sz; i++) big[i] = 'a' + i % 26;
        const char *av[] = {"SET", "big", big};
        int lens[] = {3, 3, sz};
        int n = resp(rq, 3, av, lens);
        check(sz == 3000 ? "set_3k" : "set_10k", fd, rq, n, "+OK\r\n", 5);
        const char *ag[] = {"GET", "big"};
        n = resp(rq, 2, ag, NULL);
        int e = sprintf(ex, "$%d\r\n", sz);
        memcpy(ex + e, big, sz);
        e += sz;
        ex[e++] = '\r';
        ex[e++] = '\n';
        check(sz == 3000 ? "get_3k" : "get_10k", fd, rq, n, ex, e);
    }
    CMD("flush_end", fd, "+OK\r\n", "FLUSHALL");
    CMD("dbsize_end", fd, ":0\r\n", "DBSIZE");
    close(fd);
    printf("TEST SUMMARY pass=%d fail=%d server=%s cores=%s engine=%s\n", pass, fail, variant_server_name(),
           variant_cores_name(), kv_engine_name());
}

/* -------------------------------------------------------------- benchmark */
typedef struct {
    int id, P, set;
    uint32_t ops;
    uint32_t ns;
    uint32_t *samp; /* batch round-trip us */
    int err;
} cli_t;

static volatile int go, stop;
static SemaphoreHandle_t done;

#define NBATCH 2 /* prebuilt batches per client: formatting keys per request would cost ~5 us each */

static void client_task(void *arg)
{
    cli_t *c = arg;
    static const char *vstr = "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv";
    uint8_t rb[MAXP * 48];
    int64_t tc0 = esp_timer_get_time();
    int fd = lb_connect();
    int connect_us = (int)(esp_timer_get_time() - tc0);
    char *rq = malloc(NBATCH * MAXP * 96);
    int blen[NBATCH];
    if (fd < 0 || !rq) {
        c->err = 1;
        if (fd >= 0) close(fd);
        free(rq);
        xSemaphoreGive(done);
        vTaskDelete(NULL);
    }
    uint32_t k = c->id * 7919, j = c->id;
    int rlen[NBATCH];
    for (int b = 0; b < NBATCH; b++) {
        int n = 0;
        rlen[b] = 0;
        char *base = rq + b * MAXP * 96;
        for (int i = 0; i < c->P; i++) {
            char key[20];
            snprintf(key, sizeof key, "key:%012" PRIu32, (k++ % NKEYS));
            int is_set = c->set == 1 || (c->set == 2 && (j++ % 10) == 0);
            const char *av[] = {is_set ? "SET" : "GET", key, vstr};
            n += resp(base + n, is_set ? 3 : 2, av, NULL);
            rlen[b] += is_set ? 5 : 5 + VALLEN + 2; /* "+OK\r\n" or "$32\r\n<32>\r\n" */
        }
        blen[b] = n;
    }
    uint32_t bi = 0;
    while (!go) vTaskDelay(1);
    while (!stop) {
        int b = bi++ % NBATCH;
        int64_t t0 = esp_timer_get_time();
        int sfail = send_all(fd, rq + b * MAXP * 96, blen[b]) < 0;
        if (sfail || recv_n(fd, rb, (size_t)rlen[b]) < 0) {
            printf("LBDIAG client %d io failed after %" PRIu32 " ops errno=%d in=%s got=%u/%d fd=%d connect_us=%d "
                   "fail_after_us=%d\n", c->id, c->ops, errno, sfail ? "send" : "recv", (unsigned)g_recv_got, rlen[b],
                   fd, connect_us, (int)(esp_timer_get_time() - t0));
            c->err = 2;
            break;
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (c->samp && c->ns < NSAMP) c->samp[c->ns++] = us;
        c->ops += c->P;
    }
    close(fd);
    free(rq);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void preload(void)
{
    int fd = lb_connect();
    char rq[128];
    uint8_t rb[8];
    for (int i = 0; i < NKEYS; i++) {
        char key[20];
        snprintf(key, sizeof key, "key:%012d", i);
        const char *av[] = {"SET", key, "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv"};
        int n = resp(rq, 3, av, NULL);
        send_all(fd, rq, n);
        /* every SET must be acknowledged: an earlier version ignored "-OOM" replies, so benchmark
           GETs later found missing keys ("$-1", 5 bytes), waited for 39 and timed out */
        if (recv_n(fd, rb, 5) < 0 || memcmp(rb, "+OK\r\n", 5) != 0) {
            printf("LBDIAG preload SET %d failed: '%.5s'\n", i, (const char *)rb);
            close(fd);
            return;
        }
    }
    close(fd);
}

static void bench(const char *scen, int conns, int P, int set)
{
    static cli_t cl[4];
    static uint32_t all[4 * NSAMP];
    for (int i = 0; i < conns; i++) {
        uint32_t *s = cl[i].samp;
        memset(&cl[i], 0, sizeof cl[i]);
        cl[i].samp = s ? s : malloc(NSAMP * sizeof(uint32_t));
        cl[i].id = i;
        cl[i].P = P;
        cl[i].set = set;
        xTaskCreatePinnedToCore(client_task, "lbc", 4096, &cl[i], 5, NULL, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(100)); /* connections established */
    size_t heap_run = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    memset((void *)g_stats, 0, sizeof g_stats);
    uint64_t up0, a0, b0, up1, a1, b1;
    sysmon_cpu(&up0, &a0, &b0);
    go = 1;
    vTaskDelay(pdMS_TO_TICKS(RUN_MS));
    stop = 1;
    sysmon_cpu(&up1, &a1, &b1);
    for (int i = 0; i < conns; i++) xSemaphoreTake(done, portMAX_DELAY);
    go = stop = 0;
    uint64_t win = up1 - up0;
    uint32_t ops = 0, n = 0;
    int err = 0;
    for (int i = 0; i < conns; i++) {
        ops += cl[i].ops;
        err |= cl[i].err;
        if (cl[i].samp) memcpy(all + n, cl[i].samp, cl[i].ns * sizeof(uint32_t));
        n += cl[i].ns;
    }
    qsort(all, n, sizeof *all, cmp_u32);
    uint32_t srv_cmds = 0, srv_batches = 0;
    uint64_t cyc = 0;
    for (int i = 0; i < NSTATS; i++) {
        srv_cmds += g_stats[i].cmds;
        srv_batches += g_stats[i].batches;
        cyc += g_stats[i].exec_cycles;
    }
    printf("RESULT {\"part\":\"loopback\",\"scenario\":\"%s\",\"server\":\"%s\",\"cores\":\"%s\",\"engine\":\"%s\","
           "\"cmd\":\"%s\",\"rep\":%d,\"run_ms\":%d,\"conns\":%d,\"P\":%d,\"ops_per_s\":%" PRIu64 ",\"batch_p50_us\":%" PRIu32
           ",\"batch_p99_us\":%" PRIu32 ",\"batch_p999_us\":%" PRIu32 ",\"cpu0_pct\":%.1f,\"cpu1_pct\":%.1f,"
           "\"server_cmds_per_batch\":%.2f,\"server_exec_us_per_cmd\":%.3f,\"errors\":%d,\"heap_free\":%u}\n",
           scen, variant_server_name(), variant_cores_name(), kv_engine_name(), set == 2 ? "MIX90" : set ? "SET" : "GET", g_rep, RUN_MS, conns, P,
           (uint64_t)ops * 1000000 / win, n ? all[n / 2] : 0, n ? all[n * 99 / 100] : 0,
           n ? all[n * 999 / 1000] : 0, 100.0 * (1.0 - (double)(a1 - a0) / win),
           100.0 * (1.0 - (double)(b1 - b0) / win), srv_batches ? (double)srv_cmds / srv_batches : 0.0,
           srv_cmds ? (double)cyc / srv_cmds / 240.0 : 0.0, err, (unsigned)heap_run);
#if CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
    static int dumped;
    extern volatile uint32_t srv_diag[6];
    extern volatile uint64_t srv_cyc[3];
    printf("SRVCYC select_us=%.0f recv_us=%.0f send_us=%.0f (totals since boot, wall time inside the call)\n",
           srv_cyc[0] / 240.0, srv_cyc[1] / 240.0, srv_cyc[2] / 240.0);
    printf("SRVDIAG iter=%" PRIu32 " sel_pos=%" PRIu32 " sel_zero=%" PRIu32 " sel_neg=%" PRIu32 " errno=%" PRIu32
           " accept_fail=%" PRIu32 "\n",
           srv_diag[0], srv_diag[1], srv_diag[2], srv_diag[3], srv_diag[5], srv_diag[4]);
    if (!dumped++) { /* diagnostic: where does the CPU go during the first run? (cumulative since boot) */
        static char buf[1600];
        vTaskGetRunTimeStats(buf);
        printf("TASKSTATS\n%s\nTASKSTATS END\n", buf);
    }
#endif
#if CONFIG_LWIP_STATS
    int n_act = 0, n_tw = 0;
    for (struct tcp_pcb *q = tcp_active_pcbs; q; q = q->next) n_act++; /* diagnostic, unlocked read */
    for (struct tcp_pcb *q = tcp_tw_pcbs; q; q = q->next) n_tw++;
    printf("LWIPDIAG tcp xmit=%u recv=%u drop=%u memerr=%u proterr=%u err=%u | pcbs active=%d time_wait=%d"
           " | link drop=%u\n",
           lwip_stats.tcp.xmit, lwip_stats.tcp.recv, lwip_stats.tcp.drop, lwip_stats.tcp.memerr,
           lwip_stats.tcp.proterr, lwip_stats.tcp.err, n_act, n_tw, lwip_stats.link.drop);
#endif
    /* wait until the server has closed every connection of this run (max 2 s) */
    extern volatile int srv_active[2];
    for (int w = 0; w < 200 && (srv_active[0] + srv_active[1]) > 0; w++) vTaskDelay(pdMS_TO_TICKS(10));
}

/* ---- baseline: raw TCP echo over loopback (no parser, no engine) on port 7000, server task
 *      pinned like the cache server (core 1). Separates lwIP/loopback cost from server cost. */
static void echo_server(void *arg)
{
    int port = (int)(intptr_t)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(lfd, (struct sockaddr *)&a, sizeof a);
    listen(lfd, 2);
    int fd = accept(lfd, NULL, NULL);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint8_t b[64];
    for (;;) {
        int r = recv(fd, b, sizeof b, 0);
        if (r <= 0) break;
        send_all(fd, b, r);
    }
    close(fd);
    close(lfd);
    vTaskDelete(NULL);
}

static void echo_baseline(int core)
{
    int port = 7000 + core;
    xTaskCreatePinnedToCore(echo_server, "echo", 4096, (void *)(intptr_t)port, 10, NULL, core);
    vTaskDelay(pdMS_TO_TICKS(50));
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(fd, (struct sockaddr *)&a, sizeof a);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint8_t m[16] = "0123456789abcdef", r[16];
    static uint32_t s[512];
    for (int i = 0; i < 512; i++) {
        int64_t t0 = esp_timer_get_time();
        send_all(fd, m, 16);
        recv_n(fd, r, 16);
        s[i] = (uint32_t)(esp_timer_get_time() - t0);
    }
    close(fd);
    qsort(s, 512, sizeof s[0], cmp_u32);
    printf("RESULT {\"part\":\"echo_baseline\",\"echo_core\":%d,\"core_locking\":%d,\"rtt_p50_us\":%" PRIu32
           ",\"rtt_p99_us\":%" PRIu32 ",\"rtt_min_us\":%" PRIu32 "}\n",
           core, CORE_LOCKING_ON, s[256], s[506], s[0]);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---- concurrent consistency test: 4 clients x 3 s, 8 pipelined requests, 50 % SET.
 * Key "chk:NNNN" is written only by client NNNN % 4, with increasing versions; every value is
 * "kkkk:vvvvvvvv:fffffffff:cccccccc" (key, version, fill, checksum), 32 bytes. Every GET checks
 * the key and checksum (torn/corrupted), that the version was published (writers publish BEFORE
 * sending) and that it never goes backwards for that reader; at the end every key must hold its
 * writer's last version. Run against the real server, so it covers every server model. */
#define CK 512
#define CC 4
static volatile uint32_t ck_pub[CK];
static uint32_t ck_seen[CC][CK];
typedef struct {
    int id;
    uint32_t gets, sets, torn, future, backwards, misses, errors;
} ckc_t;
static ckc_t ckc[CC];

static uint32_t ck_sum(const char *p, int n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ (uint8_t)*p++) * 16777619u;
    return h;
}

static void ck_val(char *v, int k, uint32_t ver)
{
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%04x:%08" PRIx32 ":%09" PRIu32, k, ver, (uint32_t)(k * 2654435761u ^ ver) % 1000000000u);
    memcpy(v, tmp, 23);
    snprintf(v + 23, 10, ":%08" PRIx32, ck_sum(v, 23));
}

/* parse one GET reply at p (n bytes available); returns bytes consumed or -1 */
static int ck_reply(ckc_t *c, int k, const uint8_t *p, int n)
{
    if (n >= 5 && memcmp(p, "$-1\r\n", 5) == 0) {
        c->misses++;
        return 5;
    }
    if (n < 39 || memcmp(p, "$32\r\n", 5) != 0) return -1;
    const char *v = (const char *)p + 5;
    unsigned kk;
    uint32_t ver, sum;
    if (sscanf(v, "%4x:%8" SCNx32, &kk, &ver) != 2 || sscanf(v + 24, "%8" SCNx32, &sum) != 1 || (int)kk != k ||
        sum != ck_sum(v, 23)) {
        c->torn++;
        return 39;
    }
    if (ver > ck_pub[k]) c->future++;
    if (ver < ck_seen[c->id][k]) c->backwards++;
    ck_seen[c->id][k] = ver;
    return 39;
}

static void ck_task(void *arg)
{
    ckc_t *c = arg;
    int fd = lb_connect();
    char *rq = malloc(8 * 90);
    uint8_t *rb = malloc(8 * 48);
    uint32_t x = 2463534242u ^ (c->id * 977);
    if (fd < 0 || !rq || !rb) {
        c->errors++;
        goto out;
    }
    while (!go) vTaskDelay(1);
    while (!stop) {
        int n = 0, ks[8], isset[8], want = 0;
        for (int i = 0; i < 8; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            int k = x % CK;
            char key[12], val[33];
            snprintf(key, sizeof key, "chk:%04d", k);
            isset[i] = (x >> 20) & 1 && (k % CC) == c->id; /* only the key's own writer SETs it */
            ks[i] = k;
            if (isset[i]) {
                uint32_t ver = ck_pub[k] + 1; /* this client is the only writer of k */
                ck_val(val, k, ver);
                ck_pub[k] = ver; /* publish before sending: readers may see it immediately */
                const char *av[] = {"SET", key, val};
                n += resp(rq + n, 3, av, NULL);
                want += 5;
                c->sets++;
            } else {
                const char *av[] = {"GET", key};
                n += resp(rq + n, 2, av, NULL);
                want += 39; /* "$32\r\n" + 32 + "\r\n" (a miss is shorter: handled below) */
                c->gets++;
            }
        }
        if (send_all(fd, rq, n) < 0) {
            c->errors++;
            break;
        }
        int got = 0, off = 0;
        for (int i = 0; i < 8; i++) { /* read reply by reply */
            int need = isset[i] ? 5 : 5;
            while (got - off < need) {
                int r = recv(fd, rb + got, 8 * 48 - got, 0);
                if (r <= 0) {
                    c->errors++;
                    goto out;
                }
                got += r;
            }
            if (isset[i]) {
                if (memcmp(rb + off, "+OK\r\n", 5) != 0) c->errors++;
                off += 5;
                continue;
            }
            if (memcmp(rb + off, "$-1\r\n", 5) != 0)
                while (got - off < 39) {
                    int r = recv(fd, rb + got, 8 * 48 - got, 0);
                    if (r <= 0) {
                        c->errors++;
                        goto out;
                    }
                    got += r;
                }
            int u = ck_reply(c, ks[i], rb + off, got - off);
            if (u < 0) {
                c->errors++;
                goto out;
            }
            off += u;
        }
        (void)want;
    }
out:
    if (fd >= 0) close(fd);
    free(rq);
    free(rb);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

static void consistency_test(void)
{
    int fd = lb_connect();
    char rq[96], key[12], val[33];
    uint8_t rb[64];
    for (int k = 0; k < CK; k++) { /* version 0 everywhere */
        snprintf(key, sizeof key, "chk:%04d", k);
        ck_val(val, k, 0);
        const char *av[] = {"SET", key, val};
        send_all(fd, rq, resp(rq, 3, av, NULL));
        recv_n(fd, rb, 5);
        ck_pub[k] = 0;
        for (int c = 0; c < CC; c++) ck_seen[c][k] = 0;
    }
    memset(ckc, 0, sizeof ckc);
    for (int c = 0; c < CC; c++) {
        ckc[c].id = c;
        xTaskCreatePinnedToCore(ck_task, "ck", 4096, &ckc[c], 5, NULL, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    go = 1;
    vTaskDelay(pdMS_TO_TICKS(3000));
    stop = 1;
    for (int c = 0; c < CC; c++) xSemaphoreTake(done, portMAX_DELAY);
    go = stop = 0;
    /* final state: every key must hold its writer's last version */
    uint32_t final_bad = 0, unreadable = 0;
    ckc_t fin = {.id = 0};
    for (int k = 0; k < CK; k++) {
        snprintf(key, sizeof key, "chk:%04d", k);
        const char *av[] = {"GET", key};
        send_all(fd, rq, resp(rq, 2, av, NULL));
        if (recv_n(fd, rb, 5) < 0 || memcmp(rb, "$32\r\n", 5) != 0 || recv_n(fd, rb + 5, 34) < 0) {
            unreadable++;
            continue;
        }
        unsigned kk;
        uint32_t ver, sum;
        const char *v = (const char *)rb + 5;
        if (sscanf(v, "%4x:%8" SCNx32, &kk, &ver) != 2 || sscanf(v + 24, "%8" SCNx32, &sum) != 1 ||
            (int)kk != k || sum != ck_sum(v, 23)) {
            unreadable++;
            continue;
        }
        if (ver != ck_pub[k]) final_bad++;
    }
    /* clean up: the store must have room for the benchmark keys that are preloaded next */
    for (int k = 0; k < CK; k++) {
        snprintf(key, sizeof key, "chk:%04d", k);
        const char *av[] = {"DEL", key};
        send_all(fd, rq, resp(rq, 2, av, NULL));
        recv_n(fd, rb, 4); /* ":1\r\n" */
    }
    close(fd);
    (void)fin;
    /* wait until the server has closed the test's connections: the first benchmark run of the
       previous version started while they were still open and hit the 8-connection limit */
    extern volatile int srv_active[2];
    for (int w = 0; w < 300 && (srv_active[0] + srv_active[1]) > 0; w++) vTaskDelay(pdMS_TO_TICKS(10));
    ckc_t a = {0};
    for (int c = 0; c < CC; c++) {
        a.gets += ckc[c].gets, a.sets += ckc[c].sets, a.torn += ckc[c].torn, a.future += ckc[c].future;
        a.backwards += ckc[c].backwards, a.misses += ckc[c].misses, a.errors += ckc[c].errors;
    }
    int ok = !a.torn && !a.future && !a.backwards && !a.misses && !a.errors && !final_bad && !unreadable;
    printf("RESULT {\"part\":\"lb_consistency\",\"server\":\"%s\",\"cores\":\"%s\",\"engine\":\"%s\",\"clients\":%d,"
           "\"run_ms\":3000,\"gets_checked\":%" PRIu32 ",\"sets\":%" PRIu32 ",\"torn\":%" PRIu32 ",\"future\":%" PRIu32
           ",\"backwards\":%" PRIu32 ",\"misses\":%" PRIu32 ",\"io_errors\":%" PRIu32 ",\"final_mismatch\":%" PRIu32
           ",\"final_unreadable\":%" PRIu32 ",\"pass\":%d}\n",
           variant_server_name(), variant_cores_name(), kv_engine_name(), CC, a.gets, a.sets, a.torn, a.future,
           a.backwards, a.misses, a.errors, final_bad, unreadable, ok);
}

void loopback_run(void)
{
    done = xSemaphoreCreateCounting(8, 0);
    vTaskPrioritySet(NULL, 20); /* controller preempts the busy client tasks */
    functional();
#if CONFIG_KV_LOOPBACK_FUNCTIONAL_ONLY
    return;
#endif
#if CONFIG_KV_LOOPBACK_FINAL
    /* Entry 7: correctness under concurrency, then 5 repetitions x 2 s of each workload */
    consistency_test();
    preload();
    g_lb_run_ms = 2000;
    for (g_rep = 0; g_rep < 5; g_rep++) {
        bench("quiet", 1, 1, 0);  /* GET, one request in flight */
        bench("quiet", 4, 1, 0);  /* GET, 4 connections, one in flight each (comparable to microredis) */
        bench("quiet", 4, 1, 2);  /* 90/10 mix, 4 connections, one in flight each */
        bench("quiet", 4, 16, 0); /* GET, 4 connections x 16 pipelined */
        bench("quiet", 4, 16, 1); /* SET */
        bench("quiet", 4, 16, 2); /* 90/10 GET/SET mix */
    }
    return;
#endif
    echo_baseline(1); /* echo task on core 1: socket calls cross to lwIP's core 0 */
    echo_baseline(0); /* echo task on core 0: same core as lwIP's task */
    preload();
    static const int conns[] = {1, 4};
    static const int Ps[] = {1, 16};
    for (int pass_ = 0; pass_ < 2; pass_++) {
        const char *scen = pass_ ? "wifisim_core0" : "quiet";
        if (pass_) {
            wifisim_cfg_t c = WIFISIM_DEFAULT(0);
            wifisim_start(&c);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        for (int s = 0; s < 2; s++)
            for (int pi = 0; pi < 2; pi++)
                for (int ci = 0; ci < 2; ci++) bench(scen, conns[ci], Ps[pi], s);
        if (pass_) wifisim_stop();
    }
}
#endif /* CONFIG_KV_LOOPBACK_TEST */
