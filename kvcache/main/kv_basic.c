/* Basic engine: the "straightforward C port".
 * - one malloc() per entry (node header + key + value), chained buckets
 * - table doubles (realloc + rehash) at load factor 1
 * - FNV-1a hash, FreeRTOS mutex around every call, lazy expiry only, no eviction
 * This is the reference point for "what native C alone buys you". */
#include <stdlib.h>
#include <stdio.h>
#include "kv.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

typedef struct node {
    struct node *next;
    uint32_t hash;
    uint32_t expire;
    uint32_t klen;
    uint32_t vlen;
    uint8_t data[]; /* key then value */
} node_t;

static node_t **tab;
static uint32_t nbuckets, nkeys, nexpired;
static size_t bytes_used;
static SemaphoreHandle_t mtx;
static StaticSemaphore_t mtx_buf;

#define LOCK() xSemaphoreTake(mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(mtx)

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

const char *kv_engine_name(void) { return "basic-malloc-chained"; }

int kv_init(size_t reserve)
{
    if (!mtx) mtx = xSemaphoreCreateMutexStatic(&mtx_buf);
    nbuckets = 64;
    tab = calloc(nbuckets, sizeof(*tab));
    nkeys = 0;
    bytes_used = nbuckets * sizeof(*tab);
    return tab ? 0 : -1;
}

static void free_all(void)
{
    for (uint32_t i = 0; i < nbuckets; i++) {
        node_t *n = tab[i];
        while (n) {
            node_t *nx = n->next;
            free(n);
            n = nx;
        }
        tab[i] = NULL;
    }
    nkeys = 0;
    bytes_used = nbuckets * sizeof(*tab);
}

void kv_deinit(void)
{
    LOCK();
    free_all();
    free(tab);
    tab = NULL;
    nbuckets = 0;
    UNLOCK();
}

void kv_flush(void)
{
    LOCK();
    free_all();
    UNLOCK();
}

static size_t node_size(const node_t *n) { return sizeof(node_t) + n->klen + n->vlen; }

static node_t **find(const uint8_t *k, size_t kl, uint32_t h)
{
    node_t **pp = &tab[h & (nbuckets - 1)];
    while (*pp) {
        node_t *n = *pp;
        if (n->hash == h && n->klen == kl && memcmp(n->data, k, kl) == 0) return pp;
        pp = &n->next;
    }
    return pp;
}

/* lazily drop an expired node at *pp; returns 1 if it was removed */
static int reap(node_t **pp, uint32_t now)
{
    node_t *n = *pp;
    if (n && kv_expired(n->expire, now)) {
        *pp = n->next;
        bytes_used -= node_size(n);
        free(n);
        nkeys--;
        nexpired++;
        return 1;
    }
    return 0;
}

static void grow(void)
{
    uint32_t nb = nbuckets * 2;
    node_t **nt = calloc(nb, sizeof(*nt));
    if (!nt) return; /* keep working at a higher load factor */
    for (uint32_t i = 0; i < nbuckets; i++) {
        node_t *n = tab[i];
        while (n) {
            node_t *nx = n->next;
            n->next = nt[n->hash & (nb - 1)];
            nt[n->hash & (nb - 1)] = n;
            n = nx;
        }
    }
    free(tab);
    bytes_used += (nb - nbuckets) * sizeof(*tab);
    tab = nt;
    nbuckets = nb;
}

int kv_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, uint32_t ttl_ms, int flags)
{
    uint32_t h = fnv1a(k, kl), now = kv_now_ms();
    int rc = KV_OK;
    LOCK();
    node_t **pp = find(k, kl, h);
    reap(pp, now);
    node_t *old = *pp;
    if ((flags & KV_NX) && old) { rc = KV_NOTSET; goto out; }
    if ((flags & KV_XX) && !old) { rc = KV_NOTSET; goto out; }
    node_t *n = malloc(sizeof(node_t) + kl + vl);
    if (!n) { rc = KV_ENOMEM; goto out; }
    n->hash = h;
    n->klen = kl;
    n->vlen = vl;
    n->expire = ttl_ms ? (now + ttl_ms) | 1 : ((flags & KV_KEEPTTL) && old ? old->expire : 0);
    memcpy(n->data, k, kl);
    memcpy(n->data + kl, v, vl);
    bytes_used += node_size(n);
    if (old) {
        n->next = old->next;
        bytes_used -= node_size(old);
        free(old);
        *pp = n;
    } else {
        n->next = NULL;
        *pp = n;
        if (++nkeys > nbuckets) grow();
    }
out:
    UNLOCK();
    return rc;
}

static int put_bulk_hdr(obuf_t *o, size_t vl)
{
    char tmp[16];
    int n = snprintf(tmp, sizeof tmp, "$%u\r\n", (unsigned)vl);
    if (!ob_room(o, n + vl + 2)) return -1;
    memcpy(o->buf + o->len, tmp, n);
    o->len += n;
    return 0;
}

int kv_get_resp(const uint8_t *k, size_t kl, obuf_t *o)
{
    uint32_t h = fnv1a(k, kl), now = kv_now_ms();
    int rc;
    LOCK();
    node_t **pp = find(k, kl, h);
    reap(pp, now);
    node_t *n = *pp;
    if (!n) {
        if (!ob_room(o, 5)) rc = KV_ENOROOM;
        else { memcpy(o->buf + o->len, "$-1\r\n", 5); o->len += 5; rc = KV_NOTFOUND; }
    } else if (put_bulk_hdr(o, n->vlen) < 0) {
        rc = KV_ENOROOM;
    } else {
        memcpy(o->buf + o->len, n->data + n->klen, n->vlen);
        o->len += n->vlen;
        o->buf[o->len++] = '\r';
        o->buf[o->len++] = '\n';
        rc = KV_OK;
    }
    UNLOCK();
    return rc;
}

int kv_del(const uint8_t *k, size_t kl)
{
    uint32_t h = fnv1a(k, kl), now = kv_now_ms();
    int r = 0;
    LOCK();
    node_t **pp = find(k, kl, h);
    if (!reap(pp, now) && *pp) {
        node_t *n = *pp;
        *pp = n->next;
        bytes_used -= node_size(n);
        free(n);
        nkeys--;
        r = 1;
    }
    UNLOCK();
    return r;
}

int kv_exists(const uint8_t *k, size_t kl)
{
    uint32_t h = fnv1a(k, kl), now = kv_now_ms();
    LOCK();
    node_t **pp = find(k, kl, h);
    reap(pp, now);
    int r = *pp != NULL;
    UNLOCK();
    return r;
}

int64_t kv_pttl(const uint8_t *k, size_t kl)
{
    uint32_t h = fnv1a(k, kl), now = kv_now_ms();
    int64_t r;
    LOCK();
    node_t **pp = find(k, kl, h);
    reap(pp, now);
    node_t *n = *pp;
    if (!n) r = -2;
    else if (!n->expire) r = -1;
    else r = (int32_t)(n->expire - now);
    UNLOCK();
    return r;
}

int kv_expire(const uint8_t *k, size_t kl, uint32_t ttl_ms)
{
    uint32_t h = fnv1a(k, kl), now = kv_now_ms();
    int r = 0;
    LOCK();
    node_t **pp = find(k, kl, h);
    reap(pp, now);
    if (*pp) {
        (*pp)->expire = (now + ttl_ms) | 1;
        r = 1;
    }
    UNLOCK();
    return r;
}

void kv_active_expire(int budget) { (void)budget; } /* basic: lazy expiry only */
void kv_set_evict(int on) { (void)on; }
int kv_get_evict(void) { return 0; }

void kv_stats(kv_stats_t *s)
{
    LOCK();
    s->keys = nkeys;
    s->mem_used = bytes_used;
    s->mem_total = bytes_used;
    s->evictions = 0;
    s->expired = nexpired;
    s->index_slots = nbuckets;
    s->shards = 1;
    UNLOCK();
}
