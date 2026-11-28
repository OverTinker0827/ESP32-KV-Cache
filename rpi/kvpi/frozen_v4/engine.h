/* kvpi engine: Linux/A76 port of the ESP32 kv_opt design (Phase 3), one instance per shard.
 * A shard is single-writer: either owned by one thread (shared-nothing) or wrapped by the caller. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct kv kv_t;

typedef struct {
    uint64_t count, used_bytes, arena_bytes, index_slots, slabs_bytes, evictions;
} kv_info_t;

kv_t *kv_create(size_t arena_bytes, size_t expected_keys);
void kv_destroy(kv_t *kv);

uint64_t kv_hash(const void *k, size_t kl);

/* hash = kv_hash(k, kl), passed in so a dispatcher can route on it without hashing twice */
int kv_set(kv_t *kv, const void *k, size_t kl, const void *v, size_t vl, uint64_t hash); /* 0 ok, -1 oom/too big */
/* returns 1 and points *v into the arena (valid until the next write to this shard), 0 if absent */
int kv_get(kv_t *kv, const void *k, size_t kl, uint64_t hash, const uint8_t **v, size_t *vl);
int kv_del(kv_t *kv, const void *k, size_t kl, uint64_t hash);
void kv_prefetch(kv_t *kv, uint64_t hash); /* bring the index bucket in (batched lookups) */
void kv_prefetch_entry(kv_t *kv, uint64_t hash); /* 2nd stage: the entry behind the home slot */
void kv_flush(kv_t *kv);
/* optimistic concurrent reads (see engine.c); kv_set_shared(kv,1) before other threads read */
void kv_set_shared(kv_t *kv, int on);
int kv_get_rd(kv_t *kv, const void *k, size_t kl, uint64_t h, const uint8_t **v, size_t *vl);
void kv_info(kv_t *kv, kv_info_t *out);
