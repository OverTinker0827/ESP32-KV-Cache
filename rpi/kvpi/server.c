/* kvpi-server v0: single-threaded epoll RESP server over the kvpi engine (the Redis-equivalent
 * design, so the first comparison with Redis is engine + request path, not threading).
 * Pipelining: every complete command in a read is executed, replies are gathered in one buffer
 * and written with one write() per wake.
 * usage: kvpi-server [-p port] [-m arena_MiB] [-k expected_keys] */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "engine.h"

#define MAXARGS 64
#define RBUF0 (16 << 10)

typedef struct {
    int fd;
    char *rb;
    size_t rcap, rlen, roff;
    char *wb;
    size_t wcap, wlen, woff;
    int want_out, closing;
} conn_t;

static kv_t *kv;
static int ep;

/* ---------------------------------------------------------------- output */
static void wreserve(conn_t *c, size_t n)
{
    if (c->wlen + n <= c->wcap) return;
    size_t cap = c->wcap ? c->wcap : 16384;
    while (cap < c->wlen + n) cap *= 2;
    c->wb = realloc(c->wb, cap);
    c->wcap = cap;
}
static inline void wput(conn_t *c, const void *p, size_t n)
{
    wreserve(c, n);
    memcpy(c->wb + c->wlen, p, n);
    c->wlen += n;
}
#define WLIT(c, s) wput(c, s, sizeof(s) - 1)
static void wint(conn_t *c, char pre, long long v)
{
    char b[32];
    int n = snprintf(b, sizeof b, "%c%lld\r\n", pre, v);
    wput(c, b, n);
}
static void wbulk(conn_t *c, const void *p, size_t n)
{
    wreserve(c, n + 24);
    char *o = c->wb + c->wlen;
    int h = sprintf(o, "$%zu\r\n", n);
    memcpy(o + h, p, n);
    memcpy(o + h + n, "\r\n", 2);
    c->wlen += h + n + 2;
}

/* ---------------------------------------------------------------- parsing */
/* parse one command at c->rb+c->roff; returns 1 complete, 0 incomplete, -1 protocol error */
static int parse(conn_t *c, int *argc, char **argv, size_t *argl)
{
    char *p = c->rb + c->roff, *end = c->rb + c->rlen;
    if (p >= end) return 0;
    if (*p != '*') { /* inline command: split on spaces */
        char *nl = memchr(p, '\n', end - p);
        if (!nl) return 0;
        char *e = nl > p && nl[-1] == '\r' ? nl - 1 : nl;
        int n = 0;
        for (char *q = p; q < e && n < MAXARGS;) {
            while (q < e && *q == ' ') q++;
            if (q >= e) break;
            char *s = q;
            while (q < e && *q != ' ') q++;
            argv[n] = s, argl[n] = q - s, n++;
        }
        *argc = n;
        c->roff = nl + 1 - c->rb;
        return 1;
    }
    char *nl = memchr(p, '\n', end - p);
    if (!nl) return 0;
    long n = strtol(p + 1, NULL, 10);
    if (n <= 0 || n > MAXARGS) return -1;
    p = nl + 1;
    for (long i = 0; i < n; i++) {
        if (p >= end) return 0;
        if (*p != '$') return -1;
        nl = memchr(p, '\n', end - p);
        if (!nl) return 0;
        long l = strtol(p + 1, NULL, 10);
        if (l < 0) return -1;
        p = nl + 1;
        if (end - p < l + 2) return 0;
        argv[i] = p, argl[i] = (size_t)l;
        p += l + 2;
    }
    *argc = (int)n;
    c->roff = p - c->rb;
    return 1;
}

#define IS(i, s) (argl[i] == sizeof(s) - 1 && strncasecmp(argv[i], s, sizeof(s) - 1) == 0)

static void exec_cmd(conn_t *c, int argc, char **argv, size_t *argl)
{
    if (argc == 0) return;
    if (IS(0, "GET") && argc == 2) {
        const uint8_t *v;
        size_t vl;
        if (kv_get(kv, argv[1], argl[1], kv_hash(argv[1], argl[1]), &v, &vl)) wbulk(c, v, vl);
        else WLIT(c, "$-1\r\n");
    } else if (IS(0, "SET") && argc >= 3) {
        if (kv_set(kv, argv[1], argl[1], argv[2], argl[2], kv_hash(argv[1], argl[1])) == 0) WLIT(c, "+OK\r\n");
        else WLIT(c, "-OOM command not allowed when used memory > 'maxmemory'\r\n");
    } else if (IS(0, "DEL") || IS(0, "UNLINK")) {
        long n = 0;
        for (int i = 1; i < argc; i++) n += kv_del(kv, argv[i], argl[i], kv_hash(argv[i], argl[i]));
        wint(c, ':', n);
    } else if (IS(0, "EXISTS")) {
        long n = 0;
        const uint8_t *v;
        size_t vl;
        for (int i = 1; i < argc; i++) n += kv_get(kv, argv[i], argl[i], kv_hash(argv[i], argl[i]), &v, &vl);
        wint(c, ':', n);
    } else if (IS(0, "MGET")) {
        wint(c, '*', argc - 1);
        for (int i = 1; i < argc; i++) {
            const uint8_t *v;
            size_t vl;
            if (kv_get(kv, argv[i], argl[i], kv_hash(argv[i], argl[i]), &v, &vl)) wbulk(c, v, vl);
            else WLIT(c, "$-1\r\n");
        }
    } else if (IS(0, "MSET") && argc % 2 == 1) {
        for (int i = 1; i < argc; i += 2) kv_set(kv, argv[i], argl[i], argv[i + 1], argl[i + 1], kv_hash(argv[i], argl[i]));
        WLIT(c, "+OK\r\n");
    } else if (IS(0, "PING")) {
        if (argc > 1) wbulk(c, argv[1], argl[1]);
        else WLIT(c, "+PONG\r\n");
    } else if (IS(0, "ECHO") && argc == 2) {
        wbulk(c, argv[1], argl[1]);
    } else if (IS(0, "DBSIZE")) {
        kv_info_t in;
        kv_info(kv, &in);
        wint(c, ':', (long long)in.count);
    } else if (IS(0, "FLUSHALL") || IS(0, "FLUSHDB")) {
        kv_flush(kv);
        WLIT(c, "+OK\r\n");
    } else if (IS(0, "CONFIG") || IS(0, "COMMAND") || IS(0, "CLIENT")) {
        WLIT(c, "*0\r\n");
    } else if (IS(0, "INFO")) {
        kv_info_t in;
        kv_info(kv, &in);
        char b[256];
        int n = snprintf(b, sizeof b, "# kvpi\r\nkeys:%lu\r\nused_bytes:%lu\r\nslab_bytes:%lu\r\nindex_slots:%lu\r\n",
                         (unsigned long)in.count, (unsigned long)in.used_bytes, (unsigned long)in.slabs_bytes,
                         (unsigned long)in.index_slots);
        wbulk(c, b, n);
    } else if (IS(0, "QUIT")) {
        WLIT(c, "+OK\r\n");
        c->closing = 1;
    } else {
        WLIT(c, "-ERR unknown command\r\n");
    }
}

/* ---------------------------------------------------------------- event loop */
static void conn_close(conn_t *c)
{
    close(c->fd);
    free(c->rb);
    free(c->wb);
    free(c);
}

static int flush_out(conn_t *c)
{
    while (c->woff < c->wlen) {
        ssize_t n = write(c->fd, c->wb + c->woff, c->wlen - c->woff);
        if (n < 0) {
            if (errno == EAGAIN) break;
            return -1;
        }
        c->woff += n;
    }
    if (c->woff == c->wlen) c->woff = c->wlen = 0;
    int want = c->wlen > 0;
    if (want != c->want_out) {
        struct epoll_event ev = {.events = EPOLLIN | (want ? EPOLLOUT : 0), .data.ptr = c};
        epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
        c->want_out = want;
    }
    return 0;
}

static int on_readable(conn_t *c)
{
    for (;;) {
        if (c->rcap - c->rlen < 4096) {
            if (c->roff > 0) { /* compact */
                memmove(c->rb, c->rb + c->roff, c->rlen - c->roff);
                c->rlen -= c->roff, c->roff = 0;
            }
            if (c->rcap - c->rlen < 4096) c->rb = realloc(c->rb, c->rcap *= 2);
        }
        ssize_t n = read(c->fd, c->rb + c->rlen, c->rcap - c->rlen);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EAGAIN) break;
            if (errno == EINTR) continue;
            return -1;
        }
        c->rlen += n;
        int argc;
        char *argv[MAXARGS];
        size_t argl[MAXARGS];
        int r;
        while ((r = parse(c, &argc, argv, argl)) == 1) exec_cmd(c, argc, argv, argl);
        if (r < 0) {
            WLIT(c, "-ERR protocol error\r\n");
            c->closing = 1;
        }
        if (c->roff == c->rlen) c->roff = c->rlen = 0;
        if ((size_t)n < c->rcap - (c->rlen + n)) break; /* short read: socket drained */
    }
    if (flush_out(c) < 0) return -1;
    return c->closing && c->wlen == 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    int port = 7777;
    size_t arena_mb = 2048, keys = 1 << 20;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-m")) arena_mb = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "-k")) keys = atol(argv[i + 1]);
    }
    signal(SIGPIPE, SIG_IGN);
    kv = kv_create(arena_mb << 20, keys);
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (void *)&a, sizeof a) || listen(ls, 1024)) { perror("bind/listen"); return 1; }
    ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
    fprintf(stderr, "kvpi-server v0 on :%d, arena %zu MiB\n", port, arena_mb);
    struct epoll_event evs[256];
    for (;;) {
        int n = epoll_wait(ep, evs, 256, -1);
        for (int i = 0; i < n; i++) {
            conn_t *c = evs[i].data.ptr;
            if (!c) {
                int fd;
                while ((fd = accept4(ls, NULL, NULL, SOCK_NONBLOCK)) >= 0) {
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    conn_t *nc = calloc(1, sizeof *nc);
                    nc->fd = fd;
                    nc->rb = malloc(RBUF0), nc->rcap = RBUF0;
                    struct epoll_event e2 = {.events = EPOLLIN, .data.ptr = nc};
                    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e2);
                }
                continue;
            }
            int bad = 0;
            if (evs[i].events & (EPOLLERR | EPOLLHUP)) bad = 1;
            if (!bad && (evs[i].events & EPOLLIN)) bad = on_readable(c) < 0;
            if (!bad && (evs[i].events & EPOLLOUT)) bad = flush_out(c) < 0;
            if (bad) conn_close(c);
        }
    }
}
