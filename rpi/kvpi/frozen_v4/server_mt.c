/* kvpi-server v1: multi-core (design D1 + D2 in rpi/OPTIMIZATION_PLAN.md).
 *  D1  one worker thread per server core, pinned; each has its own SO_REUSEPORT listening socket
 *      and its own epoll, so a connection lives on one core for its whole life (no accept thread,
 *      no io-thread handoff)
 *  D2  data shared by all workers: S stripes (power of two), each its own engine instance plus one
 *      64 B-aligned sequence word that is both the writer lock and the seqlock:
 *        writer: CAS seq even->odd (lock), write, store seq+1 (unlock)
 *        reader: seq even? -> lookup + copy value into the reply -> seq unchanged? else roll back, retry
 *      readers never write a shared line, so GETs scale with cores
 * usage: kvpi-server-mt [-p port] [-c cpu,cpu,...] [-s stripes] [-m arena_MiB_per_stripe] [-k keys] */
#include "store.h"
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "engine.h"

static int port = 7777;

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
        epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
        c->want_out = want;
    }
    return 0;
}

static int on_readable(conn_t *c)
{
    for (;;) {
        if (c->rcap - c->rlen < 4096) {
            if (c->roff > 0) {
                memmove(c->rb, c->rb + c->roff, c->rlen - c->roff);
                c->rlen -= c->roff, c->roff = 0;
            }
            if (c->rcap - c->rlen < 4096) c->rb = realloc(c->rb, c->rcap *= 2);
        }
        size_t room = c->rcap - c->rlen;
        ssize_t n = read(c->fd, c->rb + c->rlen, room);
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
        if ((size_t)n < room) break; /* short read: socket drained */
    }
    if (flush_out(c) < 0) return -1;
    return c->closing && c->wlen == 0 ? -1 : 0;
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg >> 8, cpu = (int)(intptr_t)arg & 0xFF;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    ws = &wstats[id];
    int one = 1;
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(ls, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (void *)&a, sizeof a) || listen(ls, 1024)) {
        perror("bind/listen");
        exit(1);
    }
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
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
                    nc->fd = fd, nc->ep = ep;
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
    return NULL;
}

int main(int argc, char **argv)
{
    const char *cpus = "3";
    size_t arena_mb = 256, keys = 1 << 20;
    nstripes = 64;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-c")) cpus = argv[i + 1];
        else if (!strcmp(argv[i], "-s")) nstripes = (uint32_t)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-m")) arena_mb = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "-k")) keys = atol(argv[i + 1]);
    }
    if (nstripes & (nstripes - 1)) {
        fprintf(stderr, "stripes must be a power of two\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    stripes = aligned_alloc(64, sizeof(stripe_t) * nstripes);
    memset(stripes, 0, sizeof(stripe_t) * nstripes);
    for (uint32_t i = 0; i < nstripes; i++) {
        stripes[i].kv = kv_create(arena_mb << 20, keys / nstripes + 1);
        if (!stripes[i].kv) {
            fprintf(stderr, "kv_create failed\n");
            return 1;
        }
        kv_set_shared(stripes[i].kv, 1);
    }
    int cpu_list[16], nw = 0;
    for (const char *p = cpus; *p && nw < 16;) {
        cpu_list[nw++] = atoi(p);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    fprintf(stderr, "kvpi-server-mt v1 on :%d, %d workers, %u stripes x %zu MiB arena\n", port, nw, nstripes, arena_mb);
    pthread_t t[16];
    for (int i = 0; i < nw; i++) pthread_create(&t[i], NULL, worker, (void *)(intptr_t)((i << 8) | cpu_list[i]));
    for (int i = 0; i < nw; i++) pthread_join(t[i], NULL);
    return 0;
}
