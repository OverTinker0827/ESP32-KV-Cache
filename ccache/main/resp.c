#include "resp.h"

/* parse "<digits>\r\n" starting at p; returns pointer past \n, NULL if incomplete, (void*)1 on error */
static inline HOT const uint8_t *parse_num(const uint8_t *p, const uint8_t *end, long *out)
{
    long v = 0;
    int neg = 0, nd = 0;
    if (p < end && *p == '-') {
        neg = 1;
        p++;
    }
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        if (++nd > 9) return (const uint8_t *)1;
    }
    if (p + 1 >= end) return NULL;
    if (p[0] != '\r' || p[1] != '\n' || nd == 0) return (const uint8_t *)1;
    *out = neg ? -v : v;
    return p + 2;
}

static int parse_inline(const uint8_t *buf, size_t len, arg_t *argv, int *argc)
{
    const uint8_t *nl = memchr(buf, '\n', len);
    if (!nl) return len > 4096 ? -1 : 0;
    const uint8_t *p = buf, *end = nl;
    if (end > p && end[-1] == '\r') end--;
    int n = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        const uint8_t *s = p;
        while (p < end && *p != ' ' && *p != '\t') p++;
        if (n == RESP_MAX_ARGS) return -1;
        argv[n].p = s;
        argv[n].n = p - s;
        n++;
    }
    *argc = n;
    return nl + 1 - buf;
}

HOT int resp_parse(const uint8_t *buf, size_t len, arg_t *argv, int *argc, size_t *need)
{
    *need = 0;
    if (len == 0) return 0;
    if (buf[0] != '*') return parse_inline(buf, len, argv, argc);
    const uint8_t *p = buf + 1, *end = buf + len;
    long n;
    p = parse_num(p, end, &n);
    if (!p) return 0;
    if (p == (const uint8_t *)1 || n < 0 || n > RESP_MAX_ARGS) return -1;
    for (long i = 0; i < n; i++) {
        if (p >= end) return 0;
        if (*p != '$') return -1;
        long bl;
        const uint8_t *q = parse_num(p + 1, end, &bl);
        if (!q) return 0;
        if (q == (const uint8_t *)1 || bl < 0 || bl > RESP_MAX_BULK) return -1;
        if ((size_t)(end - q) < (size_t)bl + 2) {
            *need = (q - buf) + bl + 2; /* lower bound for the whole command */
            return 0;
        }
        if (q[bl] != '\r' || q[bl + 1] != '\n') return -1;
        argv[i].p = q;
        argv[i].n = bl;
        p = q + bl + 2;
    }
    *argc = n;
    return p - buf;
}
