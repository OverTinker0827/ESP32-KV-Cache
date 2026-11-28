/* Randomised engine test against a trivial reference model (mixed SET/GET/DEL, varied sizes,
 * index growth, class changes on overwrite, backward-shift deletes). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

#define NK 20000
static int ref_len[NK];
static unsigned char ref_seed[NK];

static void fill(unsigned char *b, int len, unsigned char seed)
{
    for (int i = 0; i < len; i++) b[i] = (unsigned char)(seed + i * 31);
}

int main(void)
{
    kv_t *kv = kv_create(512u << 20, 16); /* tiny index on purpose: forces many grows */
    static unsigned char buf[70000], chk[70000];
    memset(ref_len, -1, sizeof ref_len);
    srand(7);
    long live = 0;
    for (long it = 0; it < 3000000; it++) {
        int i = rand() % NK, op = rand() % 10;
        char k[32];
        int kl = snprintf(k, sizeof k, "k%d:%d", i, i * 7);
        uint64_t h = kv_hash(k, kl);
        if (op < 5) {
            int len = rand() % 8 == 0 ? rand() % 65000 : rand() % 200;
            unsigned char seed = (unsigned char)rand();
            fill(buf, len, seed);
            assert(kv_set(kv, k, kl, buf, len, h) == 0);
            if (ref_len[i] < 0) live++;
            ref_len[i] = len, ref_seed[i] = seed;
        } else if (op < 8) {
            const uint8_t *v;
            size_t vl;
            int r = kv_get(kv, k, kl, h, &v, &vl);
            assert(r == (ref_len[i] >= 0));
            if (r) {
                assert((int)vl == ref_len[i]);
                fill(chk, ref_len[i], ref_seed[i]);
                assert(memcmp(v, chk, vl) == 0);
            }
        } else {
            int r = kv_del(kv, k, kl, h);
            assert(r == (ref_len[i] >= 0));
            if (r) live--;
            ref_len[i] = -1;
        }
    }
    kv_info_t in;
    kv_info(kv, &in);
    assert((long)in.count == live);
    /* every live key still reachable after all the deletes */
    for (int i = 0; i < NK; i++) {
        char k[32];
        int kl = snprintf(k, sizeof k, "k%d:%d", i, i * 7);
        const uint8_t *v;
        size_t vl;
        assert(kv_get(kv, k, kl, kv_hash(k, kl), &v, &vl) == (ref_len[i] >= 0));
    }
    printf("engine test OK: %ld live keys, %lu slots\n", live, (unsigned long)in.index_slots);
    kv_destroy(kv);
    return 0;
}
