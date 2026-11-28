/* RESP connection buffers and parser shared by the kvpi servers after v0 (server.c keeps its own
 * frozen copy so the v0 numbers stay reproducible). */
#pragma once
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXARGS 64
#define RBUF0 (16 << 10)

/* a large value spliced into the reply stream without copying: sent from p right after wb[0..off) */
typedef struct {
    size_t off;
    void *blob; /* reference held until the send completes (kv_blob_unref) */
    const char *p;
    size_t len;
} wseg_t;

typedef struct {
    int fd, ep;
    char *rb;
    size_t rcap, rlen, roff;
    char *wb;
    size_t wcap, wlen, woff;
    int want_out, closing;
    wseg_t *seg;
    int nseg, segcap;
} conn_t;
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
/* bulk reply whose payload is sent straight from (p, len); the caller already holds a reference */
static void wblob(conn_t *c, void *blob, const char *p, size_t len)
{
    char h[24];
    int n = snprintf(h, sizeof h, "$%zu\r\n", len);
    wput(c, h, n);
    if (c->nseg == c->segcap) c->seg = realloc(c->seg, (c->segcap = c->segcap ? c->segcap * 2 : 16) * sizeof *c->seg);
    c->seg[c->nseg++] = (wseg_t){c->wlen, blob, p, len};
    WLIT(c, "\r\n");
}
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

#if KV_FAST
/* ---------------------------------------------------------------- v3 fast path (KV_FAST)
 * - integer formatting without snprintf
 * - RESP array parser: digit loops instead of strtol, and bulk payloads are skipped by length
 *   (memchr only ran over the header lines before, but strtol + memchr per argument dominated)
 * - command lookup: first <= 8 bytes of the name, lower-cased with one OR, compared as a word */
static inline int utoa_rev(char *end, unsigned long long v) /* writes digits ending at end, returns count */
{
    int n = 0;
    do end[-++n] = '0' + v % 10, v /= 10;
    while (v);
    return n;
}

static void wint_fast(conn_t *c, char pre, long long v)
{
    char b[32], *e = b + 30;
    e[0] = '\r', e[1] = '\n';
    unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
    int n = utoa_rev(e, u);
    char *s = e - n;
    if (v < 0) *--s = '-';
    *--s = pre;
    wput(c, s, e + 2 - s);
}
static void wbulk_fast(conn_t *c, const void *p, size_t n)
{
    wreserve(c, n + 24);
    char *o = c->wb + c->wlen;
    char t[24];
    int d = utoa_rev(t + 24, n);
    o[0] = '$';
    memcpy(o + 1, t + 24 - d, d);
    o[1 + d] = '\r', o[2 + d] = '\n';
    memcpy(o + 3 + d, p, n);
    o[3 + d + n] = '\r', o[4 + d + n] = '\n';
    c->wlen += 5 + d + n;
}
#define wint wint_fast
#define wbulk wbulk_fast

/* "<digits>\r\n" at p; returns pointer after it, NULL if incomplete, (char*)1 on error */
static inline char *rdnum(char *p, char *end, long *out)
{
    long v = 0;
    char *q = p;
    while (q < end && (unsigned)(*q - '0') < 10) v = v * 10 + (*q++ - '0');
    if (q + 2 > end) return NULL;
    if (q == p || q[0] != '\r' || q[1] != '\n') return (char *)1;
    *out = v;
    return q + 2;
}

static int parse_fast(conn_t *c, int *argc, char **argv, size_t *argl)
{
    char *p = c->rb + c->roff, *end = c->rb + c->rlen;
    if (p >= end) return 0;
    if (*p != '*') return parse(c, argc, argv, argl); /* inline commands: rare */
    long n;
    char *q = rdnum(p + 1, end, &n);
    if (!q) return 0;
    if (q == (char *)1 || n <= 0 || n > MAXARGS) return -1;
    for (long i = 0; i < n; i++) {
        if (q >= end) return 0;
        if (*q != '$') return -1;
        long l;
        char *d = rdnum(q + 1, end, &l);
        if (!d) return 0;
        if (d == (char *)1) return -1;
        if (end - d < l + 2) return 0;
        argv[i] = d, argl[i] = (size_t)l;
        q = d + l + 2;
    }
    *argc = (int)n;
    c->roff = q - c->rb;
    return 1;
}
#define parse parse_fast

static inline uint64_t cmdword(const char *s, size_t n)
{
    uint64_t w = 0;
    if (n == 0 || n > 8) return 0;
    memcpy(&w, s, n);
    return w | (0x2020202020202020ull >> (8 * (8 - n)) ); /* lower-case ASCII letters */
}
#define CW(lit) (cmdword_lit(lit, sizeof(lit) - 1))
static inline uint64_t cmdword_lit(const char *s, size_t n) { return cmdword(s, n); }
#endif
