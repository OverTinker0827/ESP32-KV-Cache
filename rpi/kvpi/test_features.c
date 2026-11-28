/* Engine v5 feature tests: expiry (lazy + sweep), NX / XX / KEEPTTL, adding/removing a deadline
 * (entry layout change), LRU eviction under a memory limit (recently used keys must survive far
 * better than cold ones), and the randomised SET/GET/DEL model test still passing with TTLs mixed in. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

#define K(i) (snprintf(kb, sizeof kb, "key:%08d", (i)), kb)
static char kb[32];

static int get(kv_t *kv, const char *k, char *out)
{
    kv_val_t v;
    if (!kv_lookup(kv, k, strlen(k), kv_hash(k, strlen(k)), &v)) return 0;
    if (out) memcpy(out, v.v, v.vl), out[v.vl] = 0;
    return 1;
}
static int put(kv_t *kv, const char *k, const char *v, uint64_t exp, int flags)
{
    return kv_put(kv, k, strlen(k), v, strlen(v), kv_hash(k, strlen(k)), exp, KV_T_STR, flags);
}

int main(void)
{
    char out[256];
    kv_clock_set(1000000);
    kv_t *kv = kv_create(256u << 20, 1024);

    /* expiry: lazy on read */
    assert(put(kv, "a", "1", 1000500, 0) == 0);
    assert(get(kv, "a", out) && !strcmp(out, "1"));
    kv_clock_set(1000500);
    assert(!get(kv, "a", NULL)); /* deadline reached */
    kv_info_t in;
    kv_info(kv, &in);
    assert(in.count == 0 && in.expired == 1);

    /* optimistic reader also hides expired keys */
    put(kv, "r", "x", 1000600, 0);
    kv_val_t v;
    assert(kv_get_rd(kv, "r", 1, kv_hash("r", 1), &v) == 1 && v.expire_ms == 1000600);
    kv_clock_set(1000700);
    assert(kv_get_rd(kv, "r", 1, kv_hash("r", 1), &v) == 0);

    /* NX / XX / KEEPTTL */
    assert(put(kv, "n", "1", 0, KV_NX) == 0);
    assert(put(kv, "n", "2", 0, KV_NX) == 1 && get(kv, "n", out) && !strcmp(out, "1"));
    assert(put(kv, "nx", "1", 0, KV_XX) == 1 && !get(kv, "nx", NULL));
    assert(put(kv, "n", "3", 0, KV_XX) == 0 && get(kv, "n", out) && !strcmp(out, "3"));
    put(kv, "t", "v", 2000000, 0);
    assert(put(kv, "t", "w", 0, KV_KEEPTTL) == 0);
    assert(kv_lookup(kv, "t", 1, kv_hash("t", 1), &v) && v.expire_ms == 2000000 && v.vl == 1 && v.v[0] == 'w');
    assert(put(kv, "t", "z", 0, 0) == 0); /* plain SET clears the TTL */
    assert(kv_lookup(kv, "t", 1, kv_hash("t", 1), &v) && v.expire_ms == 0);

    /* set_expire: add, change, remove (layout changes both ways), value intact */
    assert(kv_set_expire(kv, "n", 1, kv_hash("n", 1), 3000000) == 1);
    assert(kv_lookup(kv, "n", 1, kv_hash("n", 1), &v) && v.expire_ms == 3000000 && v.v[0] == '3');
    assert(kv_set_expire(kv, "n", 1, kv_hash("n", 1), 3500000) == 1);
    assert(kv_lookup(kv, "n", 1, kv_hash("n", 1), &v) && v.expire_ms == 3500000);
    assert(kv_set_expire(kv, "n", 1, kv_hash("n", 1), 0) == 1);
    assert(kv_lookup(kv, "n", 1, kv_hash("n", 1), &v) && v.expire_ms == 0 && v.v[0] == '3');
    assert(kv_set_expire(kv, "missing", 7, kv_hash("missing", 7), 5) == 0);

    /* active sweep removes expired keys without reads */
    kv_flush(kv);
    for (int i = 0; i < 5000; i++) put(kv, K(i), "v", i % 2 ? 1000800 : 0, 0);
    kv_clock_set(1000900);
    size_t swept = 0;
    for (int i = 0; i < 1000; i++) swept += kv_expire_step(kv, 64);
    kv_info(kv, &in);
    assert(swept == 2500 && in.count == 2500);
    for (int i = 0; i < 5000; i++) assert(get(kv, K(i), NULL) == (i % 2 == 0));
    kv_destroy(kv);

    /* LRU eviction: 8 MiB limit, 100-byte values -> fits ~60k entries; insert 300k keys while
     * touching a "hot" 5k set every second of simulated time. Hot keys must survive. */
    kv = kv_create(256u << 20, 1024);
    kv_set_limit(kv, 8u << 20, KV_LRU);
    char val[101];
    memset(val, 'v', 100), val[100] = 0;
    uint64_t now = 5000000;
    kv_clock_set(now);
    for (int i = 0; i < 5000; i++) assert(put(kv, K(i), val, 0, 0) == 0);
    int fails = 0;
    for (int i = 5000; i < 300000; i++) {
        if (i % 1000 == 0) { /* 1 s passes; the hot set is read again */
            now += 1000;
            kv_clock_set(now);
            for (int j = 0; j < 5000; j++) get(kv, K(j), NULL);
        }
        if (put(kv, K(i), val, 0, 0) != 0) fails++;
    }
    kv_info(kv, &in);
    int hot = 0, cold = 0;
    for (int j = 0; j < 5000; j++) hot += get(kv, K(j), NULL);
    for (int j = 150000; j < 155000; j++) cold += get(kv, K(j), NULL);
    printf("lru: keys %lu, slab %lu MiB, evictions %lu, put failures %d, hot kept %d/5000, old-cold kept %d/5000\n",
           (unsigned long)in.count, (unsigned long)(in.slabs_bytes >> 20), (unsigned long)in.evictions, fails, hot, cold);
    assert(in.slabs_bytes <= (8u << 20) + (1u << 20));
    assert(fails == 0 && in.evictions > 200000);
    assert(hot > 4500 && cold < 1000);
    kv_destroy(kv);

    /* noeviction: at the limit, writes fail and nothing is evicted */
    kv = kv_create(256u << 20, 1024);
    kv_set_limit(kv, 4u << 20, KV_NOEVICT);
    int ok = 0, oom = 0;
    for (int i = 0; i < 100000; i++) {
        if (put(kv, K(i), val, 0, 0) == 0) ok++;
        else oom++;
    }
    kv_info(kv, &in);
    assert(oom > 0 && in.evictions == 0 && (int)in.count == ok);
    printf("noeviction: stored %d, refused %d\n", ok, oom);
    kv_destroy(kv);
    puts("feature tests OK");
    return 0;
}
