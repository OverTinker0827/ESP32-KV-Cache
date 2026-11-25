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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "server.h"
#include "cmd.h"
#include "kv.h"
#include "route.h"

static const char *TAG = "srv_opt";

#if CONFIG_KV_CORES_SHARED || CONFIG_KV_CORES_SHARDED || CONFIG_KV_CORES_OWNER
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
#if CONFIG_KV_ENGINE_OPT
    kv_stream_t *st;  /* streamed SET in progress (value larger than the receive buffer) */
    size_t st_left;   /* value bytes still to come */
    uint8_t streaming;
#endif
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
/* diagnostics: what the event loop spends its iterations on */
volatile uint32_t srv_diag[6];
volatile int srv_active[2]; /* open connections per loop (benchmarks wait for 0 between runs) */
volatile uint64_t srv_cyc[3]; /* CPU cycles (wall, this task) inside select, recv, send */ /* iterations, select>0, select==0, select<0 (last errno in [5]), accept failures */
static int rr;

extern volatile uint64_t srv_cyc[3];
static int send_all(int fd, const uint8_t *p, size_t n)
{
    uint32_t c0 = ccount();
    while (n) {
        int w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        p += w;
        n -= w;
    }
    srv_cyc[2] += ccount() - c0;
    return 0;
}

static void conn_close(int loop, conn_t *c)
{
#if CONFIG_KV_ENGINE_OPT
    if (c->st) {
        if (c->streaming) kv_stream_abort(c->st);
        free(c->st);
    }
#endif
    if (c->fd >= 0) srv_active[loop]--;
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
                ESP_LOGW(TAG, "conn_open: no memory for buffers (free heap %u)",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                free(c->rx);
                free(c->tx);
                c->rx = c->tx = NULL;
                return -1;
            }
            c->fd = fd;
            srv_active[L->id]++;
            c->rcap = CONFIG_KV_RX_BUF;
            c->tcap = CONFIG_KV_TX_BUF;
            c->rlen = 0;
            g_stats[L->id].conns++;
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

#if CONFIG_KV_ENGINE_OPT
/* "*3\r\n$3\r\nSET\r\n$<kl>\r\n<key>\r\n$<vl>\r\n" complete at p? -> header length, else 0 */
static int set_stream_header(const uint8_t *p, size_t n, const uint8_t **key, size_t *kl, size_t *vl)
{
    if (n < 16 || memcmp(p, "*3\r\n$3\r\n", 8) != 0) return 0;
    if ((p[8] | 0x20) != 's' || (p[9] | 0x20) != 'e' || (p[10] | 0x20) != 't' || p[11] != '\r' || p[12] != '\n' ||
        p[13] != '$')
        return 0;
    size_t i = 14, v = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') v = v * 10 + (p[i++] - '0');
    if (i + 2 > n || p[i] != '\r' || p[i + 1] != '\n' || v == 0 || v > 255) return 0;
    i += 2;
    *key = p + i;
    *kl = v;
    i += v;
    if (i + 3 > n || p[i] != '\r' || p[i + 1] != '\n' || p[i + 2] != '$') return 0;
    i += 3;
    v = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') v = v * 10 + (p[i++] - '0');
    if (i + 2 > n || p[i] != '\r' || p[i + 1] != '\n') return 0;
    *vl = v;
    return i + 2;
}

/* consume value bytes (then the trailing CRLF) from the start of rx; on completion commit and
   reply. Returns -1 on a send error. A failed begin() leaves the stream inactive: its bytes are
   swallowed and the client gets -OOM, like a normal SET. */
static int stream_feed(conn_t *c)
{
    size_t used = 0;
    if (c->st_left) {
        size_t take = c->rlen < c->st_left ? c->rlen : c->st_left;
        kv_stream_write(c->st, c->rx, take);
        c->st_left -= take;
        used = take;
    }
    if (!c->st_left && c->rlen - used >= 2) {
        used += 2; /* trailing CRLF of the bulk string */
        c->streaming = 0;
        int rc = kv_stream_commit(c->st);
        static const char ok[] = "+OK\r\n";
        static const char oom[] = "-OOM command not allowed when used memory > 'maxmemory'.\r\n";
        if (send_all(c->fd, (const uint8_t *)(rc == KV_OK ? ok : oom), rc == KV_OK ? sizeof ok - 1 : sizeof oom - 1) <
            0)
            return -1;
    }
    memmove(c->rx, c->rx + used, c->rlen - used);
    c->rlen -= used;
    return 0;
}
#endif

/* returns -1 when the connection must be closed */
static HOT int conn_readable(conn_t *c, srv_stats_t *st)
{
    if (unlikely(c->rlen == c->rcap)) { /* partial command fills the buffer: grow */
        size_t want = c->rcap * 2 > BIG_BUF ? BIG_BUF : c->rcap * 2;
        if (want <= c->rcap || regrow(&c->rx, &c->rcap, want) < 0) return -1;
    }
    uint32_t cr0 = ccount();
    int n = recv(c->fd, c->rx + c->rlen, c->rcap - c->rlen, 0);
    srv_cyc[1] += ccount() - cr0;
    if (n <= 0) return -1;
    int64_t t0 = esp_timer_get_time();
    c->rlen += n;
#if CONFIG_KV_ENGINE_OPT && !KV_ROUTED
    if (c->streaming) {
        if (stream_feed(c) < 0) return -1;
        if (c->streaming) return 0; /* more value bytes to come */
    }
#endif

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
#if CONFIG_KV_ENGINE_OPT && !KV_ROUTED /* streaming touches the engine directly */
            /* a SET whose value cannot fit the receive buffer: stream the value into the store */
            const uint8_t *key;
            size_t kl, vl;
            int hl;
            if (need > c->rcap && (hl = set_stream_header(c->rx + off, c->rlen - off, &key, &kl, &vl)) &&
                (c->st || (c->st = malloc(sizeof(kv_stream_t))))) {
                if (o.len && send_all(c->fd, o.buf, o.len) < 0) return -1; /* keep reply order */
                o.len = 0;
                kv_stream_begin(c->st, key, kl, vl); /* on failure: inactive stream -> -OOM */
                c->streaming = 1;
                c->st_left = vl;
                off += hl;
                memmove(c->rx, c->rx + off, c->rlen - off);
                c->rlen -= off;
                off = 0;
                if (stream_feed(c) < 0) return -1;
                if (c->streaming) break;
                continue; /* value complete within this read: go on with what follows */
            }
#endif
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
#if CONFIG_KV_ENGINE_OPT && !KV_ROUTED
            /* a GET reply larger than the transmit buffer: stream it through the same buffer
               (no extra memory), instead of growing the buffer from the heap reserve */
            if (rc == KV_ENOROOM && argc == 2 && argv[0].n == 3 && (argv[0].p[0] | 0x20) == 'g' &&
                (argv[0].p[1] | 0x20) == 'e' && (argv[0].p[2] | 0x20) == 't') {
                size_t off = 0, total = 0;
                do {
                    size_t n = kv_get_range(argv[1].p, argv[1].n, off, c->tx, c->tcap, &total);
                    if (!total) { /* vanished meanwhile: plain miss */
                        if (send_all(c->fd, (const uint8_t *)"$-1\r\n", 5) < 0) return -1;
                        break;
                    }
                    if (send_all(c->fd, c->tx, n) < 0) return -1;
                    off += n;
                } while (off < total);
                rc = 0;
            }
#endif
            /* other oversized replies: grow step by step (doubling) */
            while (rc == KV_ENOROOM && c->tcap < BIG_BUF) {
                size_t want = c->tcap * 2 > BIG_BUF ? BIG_BUF : c->tcap * 2;
                if (regrow(&c->tx, &c->tcap, want) < 0) return -1;
                o.buf = c->tx;
                o.cap = c->tcap;
                rc = cmd_exec(argc, argv, &o);
            }
            if (rc == KV_ENOROOM) return -1;
        }
        cyc += ccount() - c0;
        if (rc == 1) quit = 1;
        off += r;
        ncmd++;
#if CONFIG_KV_CORES_OWNER
        route_service(xPortGetCoreID()); /* forwarded calls from the other loop */
#endif
    }
    if (o.len && send_all(c->fd, o.buf, o.len) < 0) return -1;
    if (off < c->rlen) memmove(c->rx, c->rx + off, c->rlen - off);
    c->rlen -= off;
    /* shrink buffers that were grown for one oversized value */
    if (c->tcap > CONFIG_KV_TX_BUF) regrow(&c->tx, &c->tcap, CONFIG_KV_TX_BUF);
    if (c->rcap > CONFIG_KV_RX_BUF && c->rlen <= CONFIG_KV_RX_BUF) regrow(&c->rx, &c->rcap, CONFIG_KV_RX_BUF);

    if (ncmd) {
        st->cmds += ncmd; /* per-loop block: single writer */
        st->batches++;
        st->exec_cycles += cyc;
        hist_add(st->svc_hist, (uint32_t)(esp_timer_get_time() - t0));
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
#if CONFIG_KV_CORES_OWNER
        int wfd = route_wake_fd(L->id); /* the other loop forwarded a call to us */
        FD_SET(wfd, &rs);
        if (wfd > maxfd) maxfd = wfd;
        route_service(L->id);
#endif
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        uint32_t cs0 = ccount();
        int n = maxfd >= 0 ? select(maxfd + 1, &rs, NULL, NULL, &tv) : (vTaskDelay(pdMS_TO_TICKS(20)), 0);
        srv_cyc[0] += ccount() - cs0;
        srv_diag[0]++;
        srv_diag[n > 0 ? 1 : n == 0 ? 2 : 3]++;
        if (n < 0) srv_diag[5] = errno;
#if CONFIG_KV_CORES_OWNER
        if (n > 0 && FD_ISSET(wfd, &rs)) {
            route_wake_clear(L->id);
            n--;
        }
        route_service(L->id);
#endif
        if (n <= 0) {
#if CONFIG_KV_CORES_OWNER
            kv_active_expire_mask(64, L->id ? 0xAA : 0x55); /* only the shards this loop owns */
#elif !CONFIG_KV_CORES_DISPATCH /* dispatch: the worker sweeps */
            kv_active_expire(64); /* idle: incremental TTL sweep */
#endif
#if CONFIG_KV_ADAPTIVE_RESERVE
            if (L->id == 0) kv_mem_adjust(CONFIG_KV_ADAPTIVE_LOW_KB * 1024, CONFIG_KV_ADAPTIVE_HIGH_KB * 1024);
#endif
            continue;
        }
        if (L->lfd >= 0 && FD_ISSET(L->lfd, &rs)) {
            int fd = accept(L->lfd, NULL, NULL);
            if (fd < 0) srv_diag[4]++;
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
                if (conn_readable(c, &g_stats[L->id]) < 0) conn_close(L->id, c);
        }
    }
}

void server_start(void)
{
#if KV_ROUTED
    route_start();
#endif
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
#elif CONFIG_KV_CORES_SINGLE_CORE0 || CONFIG_KV_CORES_DISPATCH
        L->core = 0; /* dispatch: the receiver sits next to lwIP, the worker has core 1 */
#else
        L->core = l; /* loop 0 -> core 0, loop 1 -> core 1 */
#endif
        char name[12] = "kvloop0";
        name[6] = '0' + l;
        xTaskCreatePinnedToCore(loop_task, name, 6144, L, 10, NULL, L->core);
    }
    ESP_LOGI(TAG, "listening on %d with %d loop(s)", KV_PORT, NLOOPS);
}
