#include <stdio.h>
#include <inttypes.h>
#include "cmd.h"
#include "kv.h"
#include "sysmon.h"
#include "esp_heap_caps.h"

srv_stats_t g_stats;

/* ----------------------------------------------------------- reply helpers */
static inline HOT void put(obuf_t *o, const char *s, size_t n)
{
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}
#define PUTS(o, lit) put((o), (lit), sizeof(lit) - 1)

static HOT void put_int(obuf_t *o, int64_t v)
{
    char tmp[24];
    int n = 0;
    uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = '0' + u % 10;
        u /= 10;
    } while (u);
    uint8_t *w = o->buf + o->len;
    *w++ = ':';
    if (v < 0) *w++ = '-';
    while (n) *w++ = tmp[--n];
    *w++ = '\r';
    *w++ = '\n';
    o->len = w - o->buf;
}

static void put_err(obuf_t *o, const char *msg)
{
    size_t n = strlen(msg);
    if (n > CMD_MIN_ROOM - 8) n = CMD_MIN_ROOM - 8;
    PUTS(o, "-");
    put(o, msg, n);
    PUTS(o, "\r\n");
}

static int put_bulk(obuf_t *o, const uint8_t *p, size_t n)
{
    char hdr[16];
    int hn = snprintf(hdr, sizeof hdr, "$%u\r\n", (unsigned)n);
    if (!ob_room(o, hn + n + 2)) return KV_ENOROOM;
    put(o, hdr, hn);
    put(o, (const char *)p, n);
    PUTS(o, "\r\n");
    return 0;
}

/* case-insensitive compare of arg with an UPPERCASE literal */
static inline HOT int eq(const arg_t *a, const char *s, size_t n)
{
    if (a->n != n) return 0;
    for (size_t i = 0; i < n; i++)
        if ((a->p[i] & 0xDF) != (uint8_t)s[i]) return 0;
    return 1;
}
#define EQ(a, lit) eq((a), (lit), sizeof(lit) - 1)

static int to_i64(const arg_t *a, int64_t *out)
{
    if (a->n == 0 || a->n > 18) return -1;
    int64_t v = 0;
    size_t i = 0;
    int neg = 0;
    if (a->p[0] == '-') {
        neg = 1;
        i = 1;
        if (a->n == 1) return -1;
    }
    for (; i < a->n; i++) {
        if (a->p[i] < '0' || a->p[i] > '9') return -1;
        v = v * 10 + (a->p[i] - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

static void reply_kv_err(obuf_t *o, int rc)
{
    if (rc == KV_ENOMEM) put_err(o, "OOM command not allowed when used memory > 'maxmemory'.");
    else if (rc == KV_ETOOBIG) put_err(o, "ERR key or value too large");
    else put_err(o, "ERR internal");
}

/* ------------------------------------------------------------------- SET */
static HOT void do_set(int argc, const arg_t *argv, obuf_t *o)
{
    uint32_t ttl = 0;
    int flags = 0;
    for (int i = 3; i < argc; i++) {
        const arg_t *a = &argv[i];
        int64_t v;
        if ((EQ(a, "EX") || EQ(a, "PX")) && i + 1 < argc) {
            if (to_i64(&argv[i + 1], &v) || v <= 0 || v > 0x7FFFFFFF / 1000) {
                put_err(o, "ERR invalid expire time in 'set' command");
                return;
            }
            ttl = (a->p[0] & 0xDF) == 'E' ? (uint32_t)v * 1000 : (uint32_t)v;
            i++;
        } else if (EQ(a, "NX")) flags |= KV_NX;
        else if (EQ(a, "XX")) flags |= KV_XX;
        else if (EQ(a, "KEEPTTL")) flags |= KV_KEEPTTL;
        else {
            put_err(o, "ERR syntax error");
            return;
        }
    }
    int rc = kv_set(argv[1].p, argv[1].n, argv[2].p, argv[2].n, ttl, flags);
    if (likely(rc == KV_OK)) PUTS(o, "+OK\r\n");
    else if (rc == KV_NOTSET) PUTS(o, "$-1\r\n");
    else reply_kv_err(o, rc);
}

/* ------------------------------------------------------------------ INFO */
static int do_info(obuf_t *o)
{
    static char tmp[1400];
    kv_stats_t st;
    kv_stats(&st);
    uint64_t up, i0, i1;
    sysmon_cpu(&up, &i0, &i1);
    int n = snprintf(tmp, sizeof tmp,
                     "# Server\r\nimpl:kvcache\r\nengine:%s\r\nserver:%s\r\ncores:%s\r\niram_hot:%d\r\n"
                     "# Memory\r\nheap_free:%u\r\nheap_min_free:%u\r\nheap_largest:%u\r\n"
                     "keys:%" PRIu32 "\r\nmem_used:%" PRIu32 "\r\nmem_total:%" PRIu32 "\r\n"
                     "evictions:%" PRIu32 "\r\nexpired:%" PRIu32 "\r\nindex_slots:%" PRIu32
                     "\r\nmaxmemory_policy:%s\r\n"
                     "# CPU\r\nuptime_us:%" PRIu64 "\r\nidle0_us:%" PRIu64 "\r\nidle1_us:%" PRIu64 "\r\n"
                     "# Stats\r\ncmds:%" PRIu32 "\r\nconns:%" PRIu32 "\r\nbatches:%" PRIu32
                     "\r\nexec_cycles:%" PRIu64 "\r\nsvc_hist_us_log2:",
                     kv_engine_name(), variant_server_name(), variant_cores_name(), KV_IRAM_ON,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL), st.keys,
                     st.mem_used, st.mem_total, st.evictions, st.expired, st.index_slots,
                     kv_get_evict() ? "allkeys-lru" : "noeviction", up, i0, i1, g_stats.cmds, g_stats.conns,
                     g_stats.batches, g_stats.exec_cycles);
    for (int b = 0; b < 24 && n < (int)sizeof tmp - 16; b++)
        n += snprintf(tmp + n, sizeof tmp - n, "%s%" PRIu32, b ? "," : "", g_stats.svc_hist[b]);
    n += snprintf(tmp + n, sizeof tmp - n, "\r\n");
    return put_bulk(o, (const uint8_t *)tmp, n);
}

/* --------------------------------------------------------------- dispatch */
HOT int cmd_exec(int argc, const arg_t *argv, obuf_t *o)
{
    if (argc == 0) return 0;
    const arg_t *c = &argv[0];
    uint8_t c0 = c->p[0] & 0xDF;
    int64_t v;

    switch (c0) {
    case 'G':
        if (EQ(c, "GET")) {
            if (argc != 2) goto arity;
            return kv_get_resp(argv[1].p, argv[1].n, o) == KV_ENOROOM ? KV_ENOROOM : 0;
        }
        break;
    case 'S':
        if (EQ(c, "SET")) {
            if (argc < 3) goto arity;
            do_set(argc, argv, o);
            return 0;
        }
        if (EQ(c, "SETEX")) {
            if (argc != 4) goto arity;
            if (to_i64(&argv[2], &v) || v <= 0 || v > 0x7FFFFFFF / 1000) {
                put_err(o, "ERR invalid expire time in 'setex' command");
                return 0;
            }
            int rc = kv_set(argv[1].p, argv[1].n, argv[3].p, argv[3].n, (uint32_t)v * 1000, 0);
            if (rc == KV_OK) PUTS(o, "+OK\r\n");
            else reply_kv_err(o, rc);
            return 0;
        }
        if (EQ(c, "STATS.RESET")) {
            memset((void *)&g_stats, 0, sizeof g_stats);
            PUTS(o, "+OK\r\n");
            return 0;
        }
        break;
    case 'D':
        if (EQ(c, "DEL")) {
            if (argc < 2) goto arity;
            int64_t n = 0;
            for (int i = 1; i < argc; i++) n += kv_del(argv[i].p, argv[i].n);
            put_int(o, n);
            return 0;
        }
        if (EQ(c, "DBSIZE")) {
            kv_stats_t st;
            kv_stats(&st);
            put_int(o, st.keys);
            return 0;
        }
        break;
    case 'P':
        if (EQ(c, "PING")) {
            if (argc == 1) PUTS(o, "+PONG\r\n");
            else return put_bulk(o, argv[1].p, argv[1].n);
            return 0;
        }
        if (EQ(c, "PTTL")) {
            if (argc != 2) goto arity;
            put_int(o, kv_pttl(argv[1].p, argv[1].n));
            return 0;
        }
        if (EQ(c, "PEXPIRE")) {
            if (argc != 3) goto arity;
            if (to_i64(&argv[2], &v) || v < 0 || v > 0x7FFFFFFF) goto badint;
            put_int(o, kv_expire(argv[1].p, argv[1].n, (uint32_t)v));
            return 0;
        }
        break;
    case 'T':
        if (EQ(c, "TTL")) {
            if (argc != 2) goto arity;
            int64_t ms = kv_pttl(argv[1].p, argv[1].n);
            put_int(o, ms < 0 ? ms : (ms + 500) / 1000);
            return 0;
        }
        break;
    case 'E':
        if (EQ(c, "EXISTS")) {
            if (argc < 2) goto arity;
            int64_t n = 0;
            for (int i = 1; i < argc; i++) n += kv_exists(argv[i].p, argv[i].n);
            put_int(o, n);
            return 0;
        }
        if (EQ(c, "EXPIRE")) {
            if (argc != 3) goto arity;
            if (to_i64(&argv[2], &v) || v < 0 || v > 0x7FFFFFFF / 1000) goto badint;
            put_int(o, kv_expire(argv[1].p, argv[1].n, (uint32_t)v * 1000));
            return 0;
        }
        if (EQ(c, "ECHO")) {
            if (argc != 2) goto arity;
            return put_bulk(o, argv[1].p, argv[1].n);
        }
        break;
    case 'F':
        if (EQ(c, "FLUSHALL") || EQ(c, "FLUSHDB")) {
            kv_flush();
            PUTS(o, "+OK\r\n");
            return 0;
        }
        break;
    case 'I':
        if (EQ(c, "INFO")) return do_info(o);
        break;
    case 'C':
        if (EQ(c, "COMMAND")) {
            PUTS(o, "*0\r\n");
            return 0;
        }
        if (EQ(c, "CLIENT")) { /* redis-py sends CLIENT SETINFO on connect */
            PUTS(o, "+OK\r\n");
            return 0;
        }
        if (EQ(c, "CONFIG")) {
            if (argc == 4 && EQ(&argv[1], "SET") && EQ(&argv[2], "MAXMEMORY-POLICY")) {
                if (EQ(&argv[3], "NOEVICTION")) kv_set_evict(0);
                else kv_set_evict(1);
                PUTS(o, "+OK\r\n");
            } else {
                PUTS(o, "*0\r\n");
            }
            return 0;
        }
        break;
    case 'Q':
        if (EQ(c, "QUIT")) {
            PUTS(o, "+OK\r\n");
            return 1;
        }
        break;
    }
    put_err(o, "ERR unknown command");
    return 0;
arity:
    put_err(o, "ERR wrong number of arguments");
    return 0;
badint:
    put_err(o, "ERR value is not an integer or out of range");
    return 0;
}
