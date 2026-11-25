/* Optimized server: select() event loop(s), zero-copy parsing, batched replies.
 *  - every complete command in a recv() is executed, replies are appended to one tx
 *    buffer and sent with ONE send() -> pipelined requests cost one lwIP call per batch
 *  - no per-client task: no 4 KiB stack per connection, no context switches
 *  - loops are pinned: SINGLE -> core 1 (WiFi + lwIP keep core 0), SINGLE_CORE0 -> core 0,
 *    SHARED/SHARDED -> one loop per core, connections assigned round robin */
#include <errno.h>
#include <stdlib.h>
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "server.h"
#include "cmd.h"
#include "kv.h"

static const char *TAG = "srv_opt";

#if CONFIG_KV_CORES_SHARED || CONFIG_KV_CORES_SHARDED
#define NLOOPS 2
#else
#define NLOOPS 1
#endif
#define MAXC CONFIG_KV_MAX_CLIENTS
#define BIG_BUF (RESP_MAX_BULK + 1024)

typedef struct {
    int fd;
    uint8_t *rx, *tx;
    size_t rlen, rcap, tcap;
} conn_t;

typedef struct {
    int id, core;
    int lfd; /* listener, loop 0 only */
    conn_t c[MAXC];
    int pending[MAXC];
    int npending;
    portMUX_TYPE pmux;
} loop_t;

static loop_t loops[NLOOPS];
static int rr;

static int send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        p += w;
        n -= w;
    }
    return 0;
}

static void conn_close(conn_t *c)
{
    if (c->fd >= 0) close(c->fd);
    free(c->rx);
    free(c->tx);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static int conn_open(loop_t *L, int fd)
{
    for (int i = 0; i < MAXC; i++) {
        conn_t *c = &L->c[i];
        if (c->fd < 0) {
            c->rx = malloc(CONFIG_KV_RX_BUF);
            c->tx = malloc(CONFIG_KV_TX_BUF);
            if (!c->rx || !c->tx) {
                free(c->rx);
                free(c->tx);
                c->rx = c->tx = NULL;
                return -1;
            }
            c->fd = fd;
            c->rcap = CONFIG_KV_RX_BUF;
            c->tcap = CONFIG_KV_TX_BUF;
            c->rlen = 0;
            g_stats.conns++;
            return 0;
        }
    }
    return -1;
}

/* resize a buffer, keeping contents */
static int regrow(uint8_t **b, size_t *cap, size_t want)
{
    uint8_t *nb = realloc(*b, want);
    if (!nb) return -1;
    *b = nb;
    *cap = want;
    return 0;
}

/* returns -1 when the connection must be closed */
static HOT int conn_readable(conn_t *c)
{
    if (unlikely(c->rlen == c->rcap)) { /* partial command fills the buffer: grow */
        size_t want = c->rcap * 2 > BIG_BUF ? BIG_BUF : c->rcap * 2;
        if (want <= c->rcap || regrow(&c->rx, &c->rcap, want) < 0) return -1;
    }
    int n = recv(c->fd, c->rx + c->rlen, c->rcap - c->rlen, 0);
    if (n <= 0) return -1;
    int64_t t0 = esp_timer_get_time();
    c->rlen += n;

    obuf_t o = {.buf = c->tx, .len = 0, .cap = c->tcap};
    arg_t argv[RESP_MAX_ARGS];
    size_t off = 0;
    int quit = 0, ncmd = 0;
    uint32_t cyc = 0;

    while (off < c->rlen && !quit) {
        int argc;
        size_t need;
        int r = resp_parse(c->rx + off, c->rlen - off, argv, &argc, &need);
        if (r == 0) {
            if (need > c->rcap - 0 && off == 0) { /* single command larger than buffer */
                if (need > BIG_BUF || regrow(&c->rx, &c->rcap, need + 64) < 0) {
                    static const char e[] = "-ERR request too large\r\n";
                    send_all(c->fd, (const uint8_t *)e, sizeof e - 1);
                    return -1;
                }
            }
            break;
        }
        if (r < 0) {
            static const char e[] = "-ERR Protocol error\r\n";
            if (o.len) send_all(c->fd, o.buf, o.len);
            send_all(c->fd, (const uint8_t *)e, sizeof e - 1);
            return -1;
        }
        if (o.cap - o.len < CMD_MIN_ROOM) {
            if (send_all(c->fd, o.buf, o.len) < 0) return -1;
            o.len = 0;
        }
        uint32_t c0 = ccount();
        int rc = cmd_exec(argc, argv, &o);
        if (unlikely(rc == KV_ENOROOM)) { /* idempotent read whose reply doesn't fit */
            if (send_all(c->fd, o.buf, o.len) < 0) return -1;
            o.len = 0;
            rc = cmd_exec(argc, argv, &o);
            if (rc == KV_ENOROOM) {
                if (regrow(&c->tx, &c->tcap, BIG_BUF) < 0) return -1;
                o.buf = c->tx;
                o.cap = c->tcap;
                rc = cmd_exec(argc, argv, &o);
            }
        }
        cyc += ccount() - c0;
        if (rc == 1) quit = 1;
        off += r;
        ncmd++;
    }
    if (o.len && send_all(c->fd, o.buf, o.len) < 0) return -1;
    if (off < c->rlen) memmove(c->rx, c->rx + off, c->rlen - off);
    c->rlen -= off;
    /* shrink buffers that were grown for one oversized value */
    if (c->tcap > CONFIG_KV_TX_BUF) regrow(&c->tx, &c->tcap, CONFIG_KV_TX_BUF);
    if (c->rcap > CONFIG_KV_RX_BUF && c->rlen <= CONFIG_KV_RX_BUF) regrow(&c->rx, &c->rcap, CONFIG_KV_RX_BUF);

    if (ncmd) {
        g_stats.cmds += ncmd; /* benign race between loops: stats only */
        g_stats.batches++;
        g_stats.exec_cycles += cyc;
        hist_add(g_stats.svc_hist, (uint32_t)(esp_timer_get_time() - t0));
    }
    return quit ? -1 : 0;
}

static void loop_task(void *arg)
{
    loop_t *L = arg;
    ESP_LOGI(TAG, "loop %d running on core %d", L->id, xPortGetCoreID());
    for (;;) {
        /* adopt connections handed over by the accepting loop */
        if (L->npending) {
            portENTER_CRITICAL(&L->pmux);
            int np = L->npending;
            int tmp[MAXC];
            memcpy(tmp, L->pending, np * sizeof(int));
            L->npending = 0;
            portEXIT_CRITICAL(&L->pmux);
            for (int i = 0; i < np; i++)
                if (conn_open(L, tmp[i]) < 0) close(tmp[i]);
        }
        fd_set rs;
        FD_ZERO(&rs);
        int maxfd = -1;
        if (L->lfd >= 0) {
            FD_SET(L->lfd, &rs);
            maxfd = L->lfd;
        }
        for (int i = 0; i < MAXC; i++)
            if (L->c[i].fd >= 0) {
                FD_SET(L->c[i].fd, &rs);
                if (L->c[i].fd > maxfd) maxfd = L->c[i].fd;
            }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        int n = maxfd >= 0 ? select(maxfd + 1, &rs, NULL, NULL, &tv) : (vTaskDelay(pdMS_TO_TICKS(20)), 0);
        if (n <= 0) {
            kv_active_expire(64); /* idle: incremental TTL sweep */
            continue;
        }
        if (L->lfd >= 0 && FD_ISSET(L->lfd, &rs)) {
            int fd = accept(L->lfd, NULL, NULL);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                loop_t *T = &loops[rr++ % NLOOPS];
                if (T == L) {
                    if (conn_open(L, fd) < 0) close(fd);
                } else {
                    portENTER_CRITICAL(&T->pmux);
                    if (T->npending < MAXC) T->pending[T->npending++] = fd;
                    else fd = -fd - 1;
                    portEXIT_CRITICAL(&T->pmux);
                    if (fd < 0) close(-fd - 1);
                }
            }
        }
        for (int i = 0; i < MAXC; i++) {
            conn_t *c = &L->c[i];
            if (c->fd >= 0 && FD_ISSET(c->fd, &rs))
                if (conn_readable(c) < 0) conn_close(c);
        }
    }
}

void server_start(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(KV_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) {
        ESP_LOGE(TAG, "bind/listen failed errno %d", errno);
        return;
    }
    for (int l = 0; l < NLOOPS; l++) {
        loop_t *L = &loops[l];
        L->id = l;
        L->lfd = l == 0 ? lfd : -1;
        portMUX_INITIALIZE(&L->pmux);
        for (int i = 0; i < MAXC; i++) L->c[i].fd = -1;
#if CONFIG_KV_CORES_SINGLE
        L->core = 1;
#elif CONFIG_KV_CORES_SINGLE_CORE0
        L->core = 0;
#else
        L->core = l; /* loop 0 -> core 0, loop 1 -> core 1 */
#endif
        char name[12] = "kvloop0";
        name[6] = '0' + l;
        xTaskCreatePinnedToCore(loop_task, name, 6144, L, 10, NULL, L->core);
    }
    ESP_LOGI(TAG, "listening on %d with %d loop(s)", KV_PORT, NLOOPS);
}
