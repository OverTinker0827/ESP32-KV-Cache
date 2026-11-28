/* kvpi engine v5 (features): one instance per stripe. Single writer per instance (the stripe lock),
 * plus optimistic readers on any thread (kv_get_rd, validated by the caller's seqlock).
 * v0..v4 sources are frozen in frozen_v4/. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct kv kv_t;

typedef struct {
    uint64_t count, used_bytes, arena_bytes, index_slots, slabs_bytes, blob_bytes, evictions, expired, limit_bytes;
} kv_info_t;

/* value types: strings live inline in the arena; other types are a pointer to a heap object
 * (lists, hashes, sets, sorted sets: objects.c) freed through the callback below */
enum { KV_T_STR = 0, KV_T_OBJ = 1, KV_T_BLOB = 2 /* internal: value bytes are a kv_blob_t* */ };
/* strings of KV_BLOB_MIN bytes or more are stored out of line in a reference-counted blob (engine.c) */
#define KV_BLOB_MIN 8192
typedef struct {
    uint32_t refs; /* atomic */
    uint32_t len;
    char d[];
} kv_blob_t;
int kv_blob_try_ref(kv_blob_t *b); /* 0 if the blob is already being retired: re-read the key */
void kv_blob_unref(kv_blob_t *b);
typedef struct {
    const uint8_t *v; /* string bytes, or (for KV_T_OBJ) points at the stored object pointer */
    size_t vl;
    kv_blob_t *blob;    /* non-NULL: large string. kv_get_rd gives only the pointer (v = NULL); writers get v/vl too */
    uint64_t expire_ms; /* 0 = no expiry */
    int type;
} kv_val_t;

/* SET flags */
#define KV_NX 1      /* only if absent */
#define KV_XX 2      /* only if present */
#define KV_KEEPTTL 4 /* overwrite keeps the existing expiry */

enum { KV_NOEVICT = 0, KV_LRU = 1 };

kv_t *kv_create(size_t arena_bytes, size_t expected_keys);
void kv_destroy(kv_t *kv);
/* memory limit for this instance's slab pages (0 = arena size) and what to do at the limit */
void kv_set_limit(kv_t *kv, size_t limit_bytes, int policy);
void kv_set_obj_free(void (*fn)(void *obj)); /* global: frees KV_T_OBJ values */
void kv_set_shared(kv_t *kv, int on);
/* deferred-free hook for memory concurrent readers may still hold (see engine.c) */
void kv_set_retire(void (*fn)(void *p, void (*free_fn)(void *)));

uint64_t kv_hash(const void *k, size_t kl);
/* coarse clock used for expiry and LRU (callers update it once per event-loop pass) */
void kv_clock_set(uint64_t now_ms);
uint64_t kv_now_ms(void);

/* writer side (stripe lock held) ---------------------------------------------------------- */
/* 0 stored, 1 not stored (NX/XX condition), -1 out of memory / too big */
int kv_put(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t h, uint64_t expire_ms, int type,
           int flags);
static inline int kv_set(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t h)
{
    return kv_put(kv, k, kl, v, vl, h, 0, KV_T_STR, 0) < 0 ? -1 : 0;
}
/* 1 found (expired keys are deleted and reported absent), 0 absent */
int kv_lookup(kv_t *kv, const void *k, size_t kl, uint64_t h, kv_val_t *out);
static inline int kv_get(kv_t *kv, const void *k, size_t kl, uint64_t h, const uint8_t **v, size_t *vl)
{
    kv_val_t x;
    if (!kv_lookup(kv, k, kl, h, &x)) return 0;
    *v = x.v, *vl = x.vl;
    return 1;
}
int kv_del(kv_t *kv, const void *k, size_t kl, uint64_t h);
/* set (expire_ms > 0) or clear (0) the expiry of an existing key; 1 if the key exists */
int kv_set_expire(kv_t *kv, const void *k, size_t kl, uint64_t h, uint64_t expire_ms);
size_t kv_expire_step(kv_t *kv, size_t nslots); /* active expiry: scan nslots index slots */
void kv_flush(kv_t *kv);
void kv_info(kv_t *kv, kv_info_t *out);

/* reader side (any thread, no lock) -------------------------------------------------------- */
/* 1 found, 0 absent (or expired), -1 inconsistent read: retry. Caller validates its seqlock. */
int kv_get_rd(kv_t *kv, const void *k, size_t kl, uint64_t h, kv_val_t *out);
void kv_prefetch(kv_t *kv, uint64_t hash);
void kv_prefetch_entry(kv_t *kv, uint64_t hash);
