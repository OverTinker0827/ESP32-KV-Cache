/* kvpi-server v2: v1 (SO_REUSEPORT worker per core + seqlock stripes) with io_uring instead of
 * epoll + read/write (design D3 in rpi/OPTIMIZATION_PLAN.md).
 *  - epoll v1 costs per request: 1 read + 1 write syscall + a share of epoll_wait (+ epoll_ctl when
 *    a write is short). Here every recv/send of every connection is queued as an SQE and the whole
 *    loop pass is ONE io_uring_enter (submit + wait), so syscalls are per batch, not per request.
 *  - IORING_SETUP_SINGLE_ISSUER | DEFER_TASKRUN: completions run in the worker's own context when it
 *    enters the kernel (no task-work interrupts in the middle of a batch).
 *  - multishot accept; one recv in flight per connection; replies built in wb while a send of the
 *    previous batch is in flight (double buffer), so the worker never waits for a send.
 *  - optional -q: SQPOLL (a kernel thread polls the SQ: no syscall at all while busy; costs a core).
 * usage: kvpi-server-uring [-p port] [-c cpu,...] [-s stripes] [-m arena_MiB_per_stripe] [-k keys] [-q sqpoll_cpu] */
#include "store.h"
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int port = 7777, sqpoll_cpu = -1;
static int use_mshot = 0; /* -M 1: multishot recv into a provided-buffer ring (N2) */
static int busy_us = 0;   /* -b N: spin up to N us for completions before sleeping (N3) */
#define NBUF 1024 /* provided buffers per worker (power of two) */
#define BSZ 4096

enum { OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3, OP_CANCEL = 4 };

typedef struct {
    conn_t c;    /* rb = receive buffer, wb = replies being built */
    char *sb;    /* replies being sent */
    size_t scap, slen, soff;
    int recv_on, send_on, dead;
} uconn_t;

typedef struct {
    struct io_uring ring;
    int ls;
    struct io_uring_buf_ring *br; /* multishot: kernel picks a buffer from here per completion */
    char *bufs;
} wk_t;

static inline struct io_uring_sqe *sqe_get(struct io_uring *r)
{
    struct io_uring_sqe *s;
    while (!(s = io_uring_get_sqe(r))) io_uring_submit(r);
    return s;
}
#define UD(p, op) ((uint64_t)(uintptr_t)(p) | (op))

static void arm_accept(wk_t *w)
{
    struct io_uring_sqe *s = sqe_get(&w->ring);
    io_uring_prep_multishot_accept(s, w->ls, NULL, NULL, 0);
    io_uring_sqe_set_data64(s, OP_ACCEPT);
}

static void arm_recv(wk_t *w, uconn_t *u)
{
    conn_t *c = &u->c;
    if (use_mshot) { /* one SQE keeps receiving until it ends (no F_MORE): no re-arm per request */
        struct io_uring_sqe *s = sqe_get(&w->ring);
        io_uring_prep_recv_multishot(s, c->fd, NULL, 0, 0);
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = 0;
        io_uring_sqe_set_data64(s, UD(u, OP_RECV));
        u->recv_on = 1;
        return;
    }
    if (c->rcap - c->rlen < 4096) {
        if (c->roff > 0) {
            memmove(c->rb, c->rb + c->roff, c->rlen - c->roff);
            c->rlen -= c->roff, c->roff = 0;
        }
        if (c->rcap - c->rlen < 4096) c->rb = realloc(c->rb, c->rcap *= 2);
    }
    struct io_uring_sqe *s = sqe_get(&w->ring);
    io_uring_prep_recv(s, c->fd, c->rb + c->rlen, c->rcap - c->rlen, 0);
    io_uring_sqe_set_data64(s, UD(u, OP_RECV));
    u->recv_on = 1;
}

static void start_send(wk_t *w, uconn_t *u)
{
    conn_t *c = &u->c;
    char *tb = u->sb;
    size_t tc = u->scap;
    u->sb = c->wb, u->scap = c->wcap, u->slen = c->wlen, u->soff = 0;
    c->wb = tb, c->wcap = tc, c->wlen = 0;
    struct io_uring_sqe *s = sqe_get(&w->ring);
    io_uring_prep_send(s, c->fd, u->sb, u->slen, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, UD(u, OP_SEND));
    u->send_on = 1;
}

static void maybe_free(uconn_t *u)
{
    if (!u->dead || u->recv_on || u->send_on) return;
    close(u->c.fd);
    free(u->c.rb);
    free(u->c.wb);
    free(u->sb);
    free(u);
}

/* stop a connection: with a multishot recv still armed, cancel it; its last CQE frees the conn */
static void kill_conn(wk_t *w, uconn_t *u)
{
    u->dead = 1;
    if (use_mshot && u->recv_on) {
        struct io_uring_sqe *s = sqe_get(&w->ring);
        io_uring_prep_cancel64(s, UD(u, OP_RECV), 0);
        io_uring_sqe_set_data64(s, OP_CANCEL);
    }
    maybe_free(u);
}

static void on_recv(wk_t *w, uconn_t *u, int res, unsigned flags)
{
    conn_t *c = &u->c;
    if (!use_mshot || !(flags & IORING_CQE_F_MORE)) u->recv_on = 0;
    if (use_mshot && (flags & IORING_CQE_F_BUFFER)) { /* copy out of the provided buffer, hand it back */
        unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
        if (res > 0 && !u->dead) {
            while (c->rcap - c->rlen < (size_t)res) {
                if (c->roff > 0) {
                    memmove(c->rb, c->rb + c->roff, c->rlen - c->roff);
                    c->rlen -= c->roff, c->roff = 0;
                    continue;
                }
                c->rb = realloc(c->rb, c->rcap *= 2);
            }
            memcpy(c->rb + c->rlen, w->bufs + (size_t)bid * BSZ, res);
        }
        io_uring_buf_ring_add(w->br, w->bufs + (size_t)bid * BSZ, BSZ, bid, io_uring_buf_ring_mask(NBUF), 0);
        io_uring_buf_ring_advance(w->br, 1);
    }
    if (use_mshot && res == -ENOBUFS) { /* ring ran dry: data stays in the socket, re-arm */
        if (!u->dead && !u->recv_on) arm_recv(w, u);
        return maybe_free(u);
    }
    if (res <= 0 || u->dead) return kill_conn(w, u);
    c->rlen += res;
#if KV_BATCH
    int r = exec_batch(c);
#else
    int argc;
    char *argv[MAXARGS];
    size_t argl[MAXARGS];
    int r;
    while ((r = parse(c, &argc, argv, argl)) == 1) exec_cmd(c, argc, argv, argl);
#endif
    if (r < 0) {
        WLIT(c, "-ERR protocol error\r\n");
        c->closing = 1;
    }
    if (c->roff == c->rlen) c->roff = c->rlen = 0;
    if (!u->send_on && c->wlen) start_send(w, u);
    if (c->closing) {
        if (!u->send_on) return kill_conn(w, u); /* QUIT: close once the reply is out */
    } else if (!u->recv_on) {
        arm_recv(w, u);
    }
    maybe_free(u);
}

static void on_send(wk_t *w, uconn_t *u, int res)
{
    u->send_on = 0;
    if (res < 0) return kill_conn(w, u);
    u->soff += res;
    if (u->soff < u->slen) { /* short send: rest of the same buffer */
        struct io_uring_sqe *s = sqe_get(&w->ring);
        io_uring_prep_send(s, u->c.fd, u->sb + u->soff, u->slen - u->soff, MSG_NOSIGNAL);
        io_uring_sqe_set_data64(s, UD(u, OP_SEND));
        u->send_on = 1;
        return;
    }
    if (u->c.wlen) start_send(w, u);
    else if (u->c.closing) return kill_conn(w, u);
    maybe_free(u);
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg >> 8, cpu = (int)(intptr_t)arg & 0xFF;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    ws = &wstats[id];
    wk_t w = {0};
    int one = 1;
    w.ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(w.ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(w.ls, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(w.ls, (void *)&a, sizeof a) || listen(w.ls, 1024)) {
        perror("bind/listen");
        exit(1);
    }
    struct io_uring_params p = {0};
    if (sqpoll_cpu >= 0) {
        p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF | IORING_SETUP_SINGLE_ISSUER;
        p.sq_thread_cpu = sqpoll_cpu;
        p.sq_thread_idle = 2000;
    } else {
        p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    }
    int e = io_uring_queue_init_params(4096, &w.ring, &p);
    if (e < 0) {
        fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-e));
        exit(1);
    }
    if (use_mshot) {
        int err = 0;
        w.br = io_uring_setup_buf_ring(&w.ring, NBUF, 0, 0, &err);
        if (!w.br) {
            fprintf(stderr, "io_uring_setup_buf_ring: %s\n", strerror(-err));
            exit(1);
        }
        w.bufs = aligned_alloc(4096, (size_t)NBUF * BSZ);
        for (int i = 0; i < NBUF; i++)
            io_uring_buf_ring_add(w.br, w.bufs + (size_t)i * BSZ, BSZ, i, io_uring_buf_ring_mask(NBUF), i);
        io_uring_buf_ring_advance(w.br, NBUF);
    }
    arm_accept(&w);
    for (;;) {
        if (busy_us && !io_uring_cq_ready(&w.ring)) { /* N3: poll for a while instead of sleeping */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                io_uring_submit_and_get_events(&w.ring);
                if (io_uring_cq_ready(&w.ring)) break;
                clock_gettime(CLOCK_MONOTONIC, &t1);
                if ((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000 >= busy_us) break;
            }
        }
        if (!io_uring_cq_ready(&w.ring)) io_uring_submit_and_wait(&w.ring, 1);
        else io_uring_submit(&w.ring);
        struct io_uring_cqe *cqe;
        unsigned head, n = 0;
        io_uring_for_each_cqe(&w.ring, head, cqe)
        {
            n++;
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            int op = ud & 7, res = cqe->res;
            uconn_t *u = (uconn_t *)(uintptr_t)(ud & ~7ull);
            if (op == OP_ACCEPT) {
                if (res >= 0) {
                    setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    uconn_t *nu = calloc(1, sizeof *nu);
                    nu->c.fd = res;
                    nu->c.rb = malloc(RBUF0), nu->c.rcap = RBUF0;
                    arm_recv(&w, nu);
                }
                if (!(cqe->flags & IORING_CQE_F_MORE)) arm_accept(&w);
            } else if (op == OP_RECV) {
                on_recv(&w, u, res, cqe->flags);
            } else if (op == OP_SEND) {
                on_send(&w, u, res);
            }
        }
        io_uring_cq_advance(&w.ring, n);
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
        else if (!strcmp(argv[i], "-q")) sqpoll_cpu = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-M")) use_mshot = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-b")) busy_us = atoi(argv[i + 1]);
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
    for (const char *q = cpus; *q && nw < 16;) {
        cpu_list[nw++] = atoi(q);
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
    }
    fprintf(stderr, "kvpi-server-uring on :%d, %d workers, %u stripes, sqpoll %d, mshot %d, busy %d us\n", port, nw,
            nstripes, sqpoll_cpu, use_mshot, busy_us);
    pthread_t t[16];
    for (int i = 0; i < nw; i++) pthread_create(&t[i], NULL, worker, (void *)(intptr_t)((i << 8) | cpu_list[i]));
    for (int i = 0; i < nw; i++) pthread_join(t[i], NULL);
    return 0;
}
