/* Basic server: the "first C port" model.
 *  - one FreeRTOS task (4 KiB stack) per client, unpinned
 *  - buffered line reader; every argument is copied into its own malloc() block
 *  - one send() per reply (no batching of pipelined requests)
 * TCP_NODELAY is set (as any sane port would) so the comparison is not about Nagle. */
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

static const char *TAG = "srv_basic";
static volatile int nclients;
/* same diagnostics symbols as server_opt.c (read by the loopback test) */
volatile int srv_active[2];
volatile uint32_t srv_diag[6];
volatile uint64_t srv_cyc[3];
/* tasks migrate between cores and preempt each other: update the shared block atomically */
static portMUX_TYPE stats_mux = portMUX_INITIALIZER_UNLOCKED;
#define STATS_LOCK() portENTER_CRITICAL(&stats_mux)
#define STATS_UNLOCK() portEXIT_CRITICAL(&stats_mux)

typedef struct {
    int fd;
    uint8_t buf[512];
    int pos, len;
} reader_t;

static int rd_fill(reader_t *r)
{
    r->len = recv(r->fd, r->buf, sizeof r->buf, 0);
    r->pos = 0;
    return r->len > 0 ? 0 : -1;
}

static int rd_byte(reader_t *r)
{
    if (r->pos == r->len && rd_fill(r) < 0) return -1;
    return r->buf[r->pos++];
}

/* read a CRLF-terminated line into out (without CRLF) */
static int rd_line(reader_t *r, char *out, int cap)
{
    int n = 0, ch;
    while ((ch = rd_byte(r)) >= 0) {
        if (ch == '\n') {
            if (n && out[n - 1] == '\r') n--;
            out[n] = 0;
            return n;
        }
        if (n < cap - 1) out[n++] = ch;
    }
    return -1;
}

static int rd_exact(reader_t *r, uint8_t *dst, int n)
{
    while (n) {
        if (r->pos == r->len && rd_fill(r) < 0) return -1;
        int k = r->len - r->pos;
        if (k > n) k = n;
        memcpy(dst, r->buf + r->pos, k);
        r->pos += k;
        dst += k;
        n -= k;
    }
    return 0;
}

static void client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    reader_t *r = malloc(sizeof *r);
    uint8_t *out = malloc(1024);
    arg_t argv[RESP_MAX_ARGS];
    char line[32];
    r->fd = fd;
    r->pos = r->len = 0;
    STATS_LOCK();
    g_stats[0].conns++;
    STATS_UNLOCK();
    for (;;) {
        int argc = 0;
        if (rd_line(r, line, sizeof line) < 0) break;
        if (line[0] != '*') break; /* inline commands not supported by the basic port */
        int n = atoi(line + 1);
        if (n <= 0 || n > RESP_MAX_ARGS) break;
        int ok = 1;
        for (int i = 0; i < n && ok; i++) {
            if (rd_line(r, line, sizeof line) < 0 || line[0] != '$') { ok = 0; break; }
            int bl = atoi(line + 1);
            if (bl < 0 || bl > RESP_MAX_BULK) { ok = 0; break; }
            uint8_t *p = malloc(bl + 2); /* copy every argument */
            if (!p || rd_exact(r, p, bl + 2) < 0) { free(p); ok = 0; break; }
            argv[i].p = p;
            argv[i].n = bl;
            argc++;
        }
        int quit = !ok;
        if (ok) {
            int64_t t0 = esp_timer_get_time();
            obuf_t o = {.buf = out, .len = 0, .cap = 1024};
            int64_t e0 = esp_timer_get_time(); /* tasks are unpinned: CCOUNT differs per core */
            int rc = cmd_exec(argc, argv, &o);
            /* large value: one-off bigger buffer, doubled until the reply fits (a single 70 KB
               malloc exceeds the free heap and left the client without a reply) */
            for (size_t cap = 2048; rc == KV_ENOROOM && cap <= RESP_MAX_BULK + 64 * 2; cap *= 2) {
                if (o.buf != out) free(o.buf);
                o.buf = malloc(cap);
                o.cap = cap;
                o.len = 0;
                rc = o.buf ? cmd_exec(argc, argv, &o) : KV_ENOROOM;
                if (!o.buf) break;
            }
            if (rc == KV_ENOROOM) quit = 1;
            uint32_t cyc = (uint32_t)(esp_timer_get_time() - e0) * 240; /* in cycles @240 MHz */
            if (o.buf && send(fd, o.buf, o.len, 0) < 0) quit = 1; /* one send per reply */
            if (o.buf != out) free(o.buf);
            if (rc == 1) quit = 1;
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            STATS_LOCK();
            g_stats[0].exec_cycles += cyc;
            g_stats[0].cmds++;
            g_stats[0].batches++;
            hist_add(g_stats[0].svc_hist, us);
            STATS_UNLOCK();
        }
        for (int i = 0; i < argc; i++) free((void *)argv[i].p);
        if (quit) break;
    }
    close(fd);
    free(r);
    free(out);
    nclients--;
    srv_active[0] = nclients;
    vTaskDelete(NULL);
}

static void listen_task(void *arg)
{
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(KV_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) {
        ESP_LOGE(TAG, "bind/listen failed");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "listening on %d (task per client)", KV_PORT);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        if (nclients >= CONFIG_KV_MAX_CLIENTS) {
            close(fd);
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        nclients++;
        srv_active[0] = nclients;
        if (xTaskCreate(client_task, "kvcli", 4096, (void *)(intptr_t)fd, 10, NULL) != pdPASS) {
            close(fd);
            nclients--;
            srv_active[0] = nclients;
        }
    }
}

void server_start(void)
{
    xTaskCreate(listen_task, "kvlisten", 4096, NULL, 10, NULL);
}
