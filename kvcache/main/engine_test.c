/* On-board correctness tests for the storage engine (CONFIG_KV_ENGINE_TEST).
 * Values carry a pattern derived from (key, version), so any corruption is detected.
 * Prints "ETEST PASS name" / "ETEST FAIL name ..." and "ETEST SUMMARY pass=N fail=M". */
#include "sdkconfig.h"
#if CONFIG_KV_ENGINE_TEST
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kv.h"

static int pass, fail;
static uint8_t *vbuf, *obb;
#define OBCAP (70 * 1024)

#define CHECK(name, cond, ...)                                                                                         \
    do {                                                                                                               \
        if (cond) pass++;                                                                                              \
        else {                                                                                                         \
            fail++;                                                                                                    \
            printf("ETEST FAIL %s: ", name);                                                                           \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static void pattern(uint8_t *v, size_t n, const char *key, uint32_t ver)
{
    uint32_t x = ver * 2654435761u;
    for (const char *p = key; *p; p++) x = (x ^ (uint8_t)*p) * 16777619u;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v[i] = 'a' + x % 26;
    }
}

static int set_pat(const char *key, size_t n, uint32_t ver, uint32_t ttl)
{
    pattern(vbuf, n, key, ver);
    return kv_set((const uint8_t *)key, strlen(key), vbuf, n, ttl, 0);
}

/* 1 = present with exactly the pattern, 0 = missing, -1 = wrong content */
static int get_check(const char *key, size_t n, uint32_t ver)
{
    obuf_t o = {.buf = obb, .len = 0, .cap = OBCAP};
    int rc = kv_get_resp((const uint8_t *)key, strlen(key), &o);
    if (rc == KV_NOTFOUND) return 0;
    if (rc != KV_OK) return -1;
    char hdr[16];
    int hn = snprintf(hdr, sizeof hdr, "$%u\r\n", (unsigned)n);
    if (o.len != hn + n + 2 || memcmp(obb, hdr, hn) != 0) return -1;
    pattern(vbuf, n, key, ver);
    if (memcmp(obb + hn, vbuf, n) != 0) return -1;
    return obb[hn + n] == '\r' && obb[hn + n + 1] == '\n' ? 1 : -1;
}

static void basic_ops(const char *tag)
{
    char n[64];
    kv_flush();
#define T(x) (snprintf(n, sizeof n, "%s/%s", tag, x), n)
    CHECK(T("set_small"), set_pat("a", 5, 1, 0) == KV_OK, "set failed");
    CHECK(T("get_small"), get_check("a", 5, 1) == 1, "bad value");
    CHECK(T("missing"), get_check("nope", 5, 1) == 0, "found a missing key");
    CHECK(T("overwrite_same_class"), set_pat("a", 6, 2, 0) == KV_OK && get_check("a", 6, 2) == 1, "bad");
    CHECK(T("overwrite_bigger"), set_pat("a", 300, 3, 0) == KV_OK && get_check("a", 300, 3) == 1, "bad");
    CHECK(T("overwrite_smaller"), set_pat("a", 1, 4, 0) == KV_OK && get_check("a", 1, 4) == 1, "bad");
    CHECK(T("empty_value"), set_pat("e", 0, 1, 0) == KV_OK && get_check("e", 0, 1) == 1, "bad");
    CHECK(T("del"), kv_del((const uint8_t *)"a", 1) == 1 && get_check("a", 1, 4) == 0, "bad");
    CHECK(T("del_missing"), kv_del((const uint8_t *)"a", 1) == 0, "deleted twice");
    CHECK(T("exists"), kv_exists((const uint8_t *)"e", 1) == 1, "bad");
    /* TTL */
    CHECK(T("pttl_none"), kv_pttl((const uint8_t *)"e", 1) == -1, "expected -1");
    CHECK(T("set_px"), set_pat("t", 10, 1, 60) == KV_OK && get_check("t", 10, 1) == 1, "bad");
    int64_t pt = kv_pttl((const uint8_t *)"t", 1);
    CHECK(T("pttl_range"), pt > 0 && pt <= 60, "pttl=%lld", (long long)pt);
    vTaskDelay(pdMS_TO_TICKS(90));
    CHECK(T("expired"), get_check("t", 10, 1) == 0, "still there");
    /* EXPIRE on an entry stored without an expiry field (layout rebuild) */
    set_pat("x", 40, 7, 0);
    CHECK(T("expire_rebuild"), kv_expire((const uint8_t *)"x", 1, 5000) == 1, "expire failed");
    pt = kv_pttl((const uint8_t *)"x", 1);
    CHECK(T("expire_pttl"), pt > 4000 && pt <= 5000, "pttl=%lld", (long long)pt);
    CHECK(T("expire_value_kept"), get_check("x", 40, 7) == 1, "value changed");
    CHECK(T("expire_missing"), kv_expire((const uint8_t *)"zz", 2, 100) == 0, "bad");
    /* KEEPTTL */
    pattern(vbuf, 12, "x", 8);
    kv_set((const uint8_t *)"x", 1, vbuf, 12, 0, KV_KEEPTTL);
    pt = kv_pttl((const uint8_t *)"x", 1);
    CHECK(T("keepttl"), pt > 4000 && get_check("x", 12, 8) == 1, "pttl=%lld", (long long)pt);
    /* NX / XX */
    CHECK(T("nx_exists"), kv_set((const uint8_t *)"x", 1, vbuf, 3, 0, KV_NX) == KV_NOTSET, "bad");
    CHECK(T("xx_missing"), kv_set((const uint8_t *)"y", 1, vbuf, 3, 0, KV_XX) == KV_NOTSET, "bad");
    /* chained (large) values, byte-exact, overwrite and delete */
    /* 64 KB first: it needs 17 of this test arena's 22 pages, so it must go in before the others */
    static const size_t big[] = {65535, 4089, 5000, 20000};
    for (int i = 0; i < 4; i++) {
        char k[24];
        snprintf(k, sizeof k, "big:%u", (unsigned)big[i]);
        int r = set_pat(k, big[i], 1, 0);
        CHECK(T("chain_set"), r == KV_OK, "size %u rc %d", (unsigned)big[i], r);
        CHECK(T("chain_get"), get_check(k, big[i], 1) == 1, "size %u", (unsigned)big[i]);
        if (big[i] == 65535) /* frees its 17 pages for the rest of the suite (noeviction mode) */
            CHECK(T("chain_del"), kv_del((const uint8_t *)k, strlen(k)) == 1 && get_check(k, big[i], 1) == 0,
                  "bad");
    }
    CHECK(T("chain_overwrite_small"), set_pat("big:20000", 7, 2, 0) == KV_OK && get_check("big:20000", 7, 2) == 1, "bad");
    CHECK(T("small_overwrite_chain"), set_pat("e", 9000, 3, 0) == KV_OK && get_check("e", 9000, 3) == 1, "bad");
    CHECK(T("chain_expire"), kv_expire((const uint8_t *)"e", 1, 3000) == 1 && get_check("e", 9000, 3) == 1, "bad");
    kv_stats_t st;
    kv_flush();
    kv_stats(&st);
    CHECK(T("flush_empty"), st.keys == 0, "keys=%u", (unsigned)st.keys);
#undef T
}

/* fill to eviction with a mix of sizes (forces CLOCK + page stealing + chain eviction), then
   verify that every key still present has exactly its latest value */
static void stress(const char *tag, const char *kfmt, int nkeys)
{
    char n[64], k[40];
    kv_flush();
    kv_set_evict(1);
    uint16_t *ver = calloc(nkeys, sizeof(uint16_t)); /* 0 = never stored */
    uint16_t *len = calloc(nkeys, sizeof(uint16_t));
    uint32_t x = 12345, sets = 0, okset = 0;
    for (int round = 0; round < 6000; round++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        int i = x % nkeys;
        /* 85 % small (8-127 B), 12 % medium (300-1199 B), 3 % chained (4.5-13.5 KB) */
        size_t sz = (x >> 8) % 100 < 85   ? 8 + (x >> 16) % 120
                    : (x >> 16) % 100 < 80 ? 300 + (x >> 20) % 900
                                           : 4500 + (x >> 20) % 9000;
        snprintf(k, sizeof k, kfmt, i);
        sets++;
        if (set_pat(k, sz, ver[i] + 1, 0) == KV_OK) {
            okset++;
            ver[i]++;
            len[i] = sz;
        } /* rejected (-OOM): the previous value, if any, must still be intact */
    }
    int present = 0, bad = 0;
    for (int i = 0; i < nkeys; i++) {
        snprintf(k, sizeof k, kfmt, i);
        int r = ver[i] ? get_check(k, len[i], ver[i]) : (get_check(k, 1, 1) == 0 ? 0 : -1);
        if (r == 1) present++; /* evicted keys are simply absent (r == 0) */
        else if (r < 0) bad++;
    }
    kv_stats_t st;
    kv_stats(&st);
    snprintf(n, sizeof n, "%s/stress_integrity", tag);
    CHECK(n, bad == 0, "%d corrupted of %d present", bad, present + bad);
    printf("ETEST INFO %s/stress sets=%" PRIu32 " ok=%" PRIu32 " present=%d keys=%u evictions=%u\n", tag, sets, okset,
           present, (unsigned)st.keys, (unsigned)st.evictions);
    free(ver);
    free(len);
    kv_set_evict(0);
}

void engine_test_run(size_t reserve)
{
    vbuf = malloc(OBCAP);
    obb = malloc(OBCAP);
    if (!vbuf || !obb || kv_init(reserve) != 0) {
        printf("ETEST FAIL init\n");
        return;
    }
    kv_set_evict(0);
    basic_ops("plain");
    stress("plain", "s:%d", 1500);
#if CONFIG_KV_ENGINE_OPT
    /* prefix compression: same suite with prefixed keys, plus a prefix learned after insert */
    kv_flush();
    kv_set_prefix(1);
    basic_ops("prefix");
    kv_flush();
    char n[48];
    const char *k1 = "sensor:temp:0001";
    pattern(vbuf, 20, k1, 1);
    /* GET of a key whose prefix is unknown must not learn it; SET learns it */
    obuf_t o = {.buf = obb, .len = 0, .cap = OBCAP};
    kv_get_resp((const uint8_t *)k1, strlen(k1), &o);
    int before = kv_prefix_count();
    set_pat(k1, 20, 1, 0);
    snprintf(n, sizeof n, "prefix/learned_on_set");
    CHECK(n, kv_prefix_count() == before + 1, "count %d -> %d", before, kv_prefix_count());
    snprintf(n, sizeof n, "prefix/roundtrip");
    CHECK(n, get_check(k1, 20, 1) == 1, "bad");
    stress("prefix", "sensor:room:%d", 1500);
    kv_flush();
    kv_set_prefix(0);
#endif
    kv_deinit();
    free(vbuf);
    free(obb);
    printf("ETEST SUMMARY pass=%d fail=%d engine=%s\n", pass, fail, kv_engine_name());
}
#endif
