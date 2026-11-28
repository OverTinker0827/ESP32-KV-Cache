/* kvload: lean RESP load generator for the Pi benchmarks.
 * Why: redis-benchmark only exits on its 250 ms timer, so its run time (and any rps derived from it,
 * including our wall-clock rps) is quantised to 0.25 s (up to ~10 % on 2-5 s runs). It also spends
 * more client CPU per request than needed, and on this 4-core box the client's cores are the limit.
 * kvload is duration-based (warm-up, then a timed window read with clock_gettime), pinned, one epoll
 * per thread, pre-formatted requests, and a minimal reply scanner.
 *
 * usage: kvload -p port [-h 127.0.0.1] [-t threads] [-C cpu,cpu] [-c conns_per_thread] [-P pipeline] [-z zipf_theta] [-e set_ex_s]
 *               [-r keyspace] [-d value_bytes] [-g get_fraction 0..1] [-D seconds] [-W warmup_s] [-L load_keys]
 * output (one CSV line): ops_per_s,p50_us,p99_us,p999_us,client_cpu_pct,errors
 *   latency = time from sending a batch of P requests on a connection to its last reply. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *host = "127.0.0.1";
static int port = 7777, nthreads = 2, conns = 25, pipe_n = 1, vsize = 32, dur = 8, warm = 2;
static uint32_t keyspace = 100000;
static double getfrac = 1.0;
static int cpus[16], ncpus;
static atomic_int phase; /* 0 warm-up, 1 measuring, 2 stop */
static double t_meas0, t_meas1;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* log-linear latency histogram: 0.1 us resolution below 100 us, ~1 % above */
#define NB 4096
typedef struct {
    uint64_t b[NB];
} hist_t;
static inline int hbin(double us)
{
    if (us < 100) return (int)(us * 10);
    int i = 1000 + (int)(log2f((float)(us / 100)) * 300);
    return i < NB ? i : NB - 1;
}
static double hval(int i) { return i < 1000 ? i / 10.0 : 100 * exp2((i - 1000) / 300.0); }

typedef struct {
    int fd, outstanding;
    double t_sent;
    char *rb;
    size_t rlen, rcap;
    uint64_t rng;
} lconn_t;

typedef struct {
    int id;
    uint64_t ops, errors;
    hist_t h;
} th_t;

static inline uint64_t xs(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return *s = x;
}

/* Zipf keys (-z theta): YCSB's ZipfianGenerator (Gray et al., SIGMOD'94), O(1) per draw after an O(n)
 * zeta precompute; rank 0 is the hottest key. theta 0 = uniform (the default path). */
static double zipf_theta, z_alpha, z_zetan, z_eta, z_half_pow;
static void zipf_init(uint32_t n, double theta)
{
    double zeta2 = 0;
    z_zetan = 0;
    for (uint32_t i = 1; i <= n; i++) z_zetan += 1.0 / pow((double)i, theta);
    for (int i = 1; i <= 2; i++) zeta2 += 1.0 / pow((double)i, theta);
    z_alpha = 1.0 / (1.0 - theta);
    z_eta = (1 - pow(2.0 / n, 1 - theta)) / (1 - zeta2 / z_zetan);
    z_half_pow = 1 + pow(0.5, theta);
}
static inline uint32_t zipf_next(uint64_t *rng)
{
    double u = (double)(xs(rng) >> 11) * (1.0 / 9007199254740992.0), uz = u * z_zetan;
    if (uz < 1.0) return 0;
    if (uz < z_half_pow) return 1;
    uint32_t r = (uint32_t)(keyspace * pow(z_eta * u - z_eta + 1, z_alpha));
    return r < keyspace ? r : keyspace - 1;
}

/* GET: "*2\r\n$3\r\nGET\r\n$16\r\nkey:%012u\r\n"; SET adds "$<vsize>\r\n<value>\r\n" (+ "EX <s>" with -e) */
static char *val;
static int set_ex; /* -e seconds: every SET carries EX */
static size_t build(char *o, lconn_t *c)
{
    size_t n = 0;
    for (int i = 0; i < pipe_n; i++) {
        uint32_t k = zipf_theta > 0 ? zipf_next(&c->rng) : (uint32_t)(xs(&c->rng) % keyspace);
        int is_get = getfrac >= 1.0 || (double)(xs(&c->rng) % 1000000) < getfrac * 1e6;
        if (is_get) memcpy(o + n, "*2\r\n$3\r\nGET\r\n$16\r\nkey:", 22), n += 22;
        else if (set_ex) memcpy(o + n, "*5\r\n$3\r\nSET\r\n$16\r\nkey:", 22), n += 22;
        else memcpy(o + n, "*3\r\n$3\r\nSET\r\n$16\r\nkey:", 22), n += 22;
        for (int d = 11; d >= 0; d--) o[n + d] = '0' + k % 10, k /= 10;
        n += 12;
        o[n++] = '\r', o[n++] = '\n';
        if (!is_get) {
            n += sprintf(o + n, "$%d\r\n", vsize), memcpy(o + n, val, vsize), n += vsize, o[n++] = '\r', o[n++] = '\n';
            if (set_ex) {
                char t[16];
                int tl = snprintf(t, sizeof t, "%d", set_ex);
                n += sprintf(o + n, "$2\r\nEX\r\n$%d\r\n%s\r\n", tl, t);
            }
        }
    }
    return n;
}

/* count complete replies at the front of rb; consumes them */
static int scan(lconn_t *c, uint64_t *err)
{
    char *p = c->rb, *e = c->rb + c->rlen;
    int n = 0;
    while (p < e) {
        char *nl = memchr(p, '\n', e - p);
        if (!nl) break;
        if (*p == '$') {
            long l = strtol(p + 1, NULL, 10);
            if (l >= 0) {
                if (e - (nl + 1) < l + 2) break;
                nl += l + 2;
            }
        } else if (*p == '-') {
            (*err)++;
        }
        p = nl + 1;
        n++;
    }
    size_t used = p - c->rb;
    memmove(c->rb, p, c->rlen - used);
    c->rlen -= used;
    return n;
}

static void *thread(void *arg)
{
    th_t *t = arg;
    if (ncpus) {
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(cpus[t->id % ncpus], &cs);
        pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    }
    int ep = epoll_create1(0);
    lconn_t *cs = calloc(conns, sizeof *cs);
    size_t wcap = (size_t)pipe_n * (96 + vsize) + 64; /* request + value + "EX <s>" */
    char *wb = malloc(wcap);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, host, &a.sin_addr);
    for (int i = 0; i < conns; i++) {
        lconn_t *c = &cs[i];
        c->fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect(c->fd, (void *)&a, sizeof a)) {
            perror("connect");
            exit(1);
        }
        c->rcap = 1 << 16;
        c->rb = malloc(c->rcap);
        c->rng = 0x9E3779B97F4A7C15ull * (t->id * 1000 + i + 1);
        struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
        epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
        size_t n = build(wb, c);
        c->t_sent = now();
        c->outstanding = pipe_n;
        if (write(c->fd, wb, n) != (ssize_t)n) { perror("write"); exit(1); }
    }
    struct epoll_event evs[256];
    while (atomic_load_explicit(&phase, memory_order_relaxed) < 2) {
        int n = epoll_wait(ep, evs, 256, 100);
        int meas = atomic_load_explicit(&phase, memory_order_relaxed) == 1;
        for (int i = 0; i < n; i++) {
            lconn_t *c = evs[i].data.ptr;
            if (c->rcap - c->rlen < 4096) c->rb = realloc(c->rb, c->rcap *= 2);
            ssize_t r = read(c->fd, c->rb + c->rlen, c->rcap - c->rlen);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                fprintf(stderr, "connection closed by server\n");
                exit(1);
            }
            c->rlen += r;
            int got = scan(c, &t->errors);
            c->outstanding -= got;
            if (meas) t->ops += got;
            if (c->outstanding <= 0) {
                double tn = now();
                if (meas) t->h.b[hbin((tn - c->t_sent) * 1e6)]++;
                size_t m = build(wb, c);
                c->t_sent = tn;
                c->outstanding = pipe_n;
                if (write(c->fd, wb, m) != (ssize_t)m) { perror("write"); exit(1); }
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    long load = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *o = argv[i], *v = argv[i + 1];
        if (!strcmp(o, "-h")) host = v;
        else if (!strcmp(o, "-p")) port = atoi(v);
        else if (!strcmp(o, "-t")) nthreads = atoi(v);
        else if (!strcmp(o, "-c")) conns = atoi(v);
        else if (!strcmp(o, "-P")) pipe_n = atoi(v);
        else if (!strcmp(o, "-r")) keyspace = (uint32_t)atol(v);
        else if (!strcmp(o, "-d")) vsize = atoi(v);
        else if (!strcmp(o, "-g")) getfrac = atof(v);
        else if (!strcmp(o, "-D")) dur = atoi(v);
        else if (!strcmp(o, "-W")) warm = atoi(v);
        else if (!strcmp(o, "-L")) load = atol(v);
        else if (!strcmp(o, "-z")) zipf_theta = atof(v);
        else if (!strcmp(o, "-e")) set_ex = atoi(v);
        else if (!strcmp(o, "-C"))
            for (const char *p = v; *p && ncpus < 16;) {
                cpus[ncpus++] = atoi(p);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
    }
    val = malloc(vsize);
    if (zipf_theta > 0) zipf_init(keyspace, zipf_theta);
    memset(val, 'x', vsize);
    if (load) { /* fill keys 0..load-1 so GETs hit (one connection, pipelined 64) */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
        inet_pton(AF_INET, host, &a.sin_addr);
        if (connect(fd, (void *)&a, sizeof a)) { perror("connect"); return 1; }
        char *b = malloc(64 * (80 + vsize)), rb[1 << 16];
        for (long k = 0; k < load;) {
            size_t n = 0;
            int m = 0;
            for (; m < 64 && k < load; m++, k++) {
                n += sprintf(b + n, "*3\r\n$3\r\nSET\r\n$16\r\nkey:%012ld\r\n$%d\r\n", k, vsize);
                memcpy(b + n, val, vsize), n += vsize, b[n++] = '\r', b[n++] = '\n';
            }
            if (write(fd, b, n) != (ssize_t)n) return 1;
            int got = 0; /* each reply is "+OK\r\n" (5 B) */
            while (got < m * 5) {
                ssize_t r = read(fd, rb, sizeof rb);
                if (r <= 0) return 1;
                got += r;
            }
        }
        close(fd);
    }
    pthread_t tid[64];
    th_t *ts = calloc(nthreads, sizeof *ts);
    struct rusage ru0, ru1;
    for (int i = 0; i < nthreads; i++) ts[i].id = i, pthread_create(&tid[i], NULL, thread, &ts[i]);
    usleep(warm * 1000000);
    getrusage(RUSAGE_SELF, &ru0);
    t_meas0 = now();
    atomic_store(&phase, 1);
    usleep(dur * 1000000);
    atomic_store(&phase, 2);
    t_meas1 = now();
    getrusage(RUSAGE_SELF, &ru1);
    for (int i = 0; i < nthreads; i++) pthread_join(tid[i], NULL);
    uint64_t ops = 0, err = 0;
    static hist_t h;
    for (int i = 0; i < nthreads; i++) {
        ops += ts[i].ops, err += ts[i].errors;
        for (int b = 0; b < NB; b++) h.b[b] += ts[i].h.b[b];
    }
    uint64_t tot = 0, acc = 0;
    for (int b = 0; b < NB; b++) tot += h.b[b];
    double p50 = 0, p99 = 0, p999 = 0;
    for (int b = 0; b < NB; b++) {
        acc += h.b[b];
        if (!p50 && acc >= tot * 0.50) p50 = hval(b);
        if (!p99 && acc >= tot * 0.99) p99 = hval(b);
        if (!p999 && acc >= tot * 0.999) p999 = hval(b);
    }
    double el = t_meas1 - t_meas0;
    double cpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) * 1e-6 +
                 (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) * 1e-6;
    printf("%.0f,%.1f,%.1f,%.1f,%.0f,%lu\n", ops / el, p50, p99, p999, cpu / el * 100, (unsigned long)err);
    return 0;
}
