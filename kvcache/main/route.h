#pragma once
/* Request routing for the owner-based and queue-dispatch server modes (Entry 7).
 *  owner    : 2 event loops; shard s belongs to loop (s & 1); an engine call for a key owned by
 *             the other loop is forwarded to it through a lock-free ring, the owner is woken
 *             with an eventfd, the caller waits (serving calls forwarded to itself meanwhile).
 *  dispatch : 1 receiver loop on core 0 parses everything; every engine call is executed by a
 *             worker task on core 1 that owns all data (lock-free ring, spinning worker).
 * In every other build these are plain calls into the engine. */
#include "kv.h"

#if CONFIG_KV_CORES_OWNER || CONFIG_KV_CORES_DISPATCH
#define KV_ROUTED 1
int r_set(const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, uint32_t ttl_ms, int flags);
int r_get_resp(const uint8_t *k, size_t kl, obuf_t *o);
int r_del(const uint8_t *k, size_t kl);
int r_exists(const uint8_t *k, size_t kl);
int64_t r_pttl(const uint8_t *k, size_t kl);
int r_expire(const uint8_t *k, size_t kl, uint32_t ttl_ms);
void r_flush(void);
void r_stats(kv_stats_t *s);
void route_start(void);      /* creates rings, eventfds, the dispatch worker */
void route_service(int loop); /* owner mode: execute calls forwarded to this loop */
int route_wake_fd(int loop);  /* owner mode: eventfd to put in the loop's select set, -1 if none */
void route_wake_clear(int loop);
#else
#define KV_ROUTED 0
#define r_set kv_set
#define r_get_resp kv_get_resp
#define r_del kv_del
#define r_exists kv_exists
#define r_pttl kv_pttl
#define r_expire kv_expire
#define r_flush kv_flush
#define r_stats kv_stats
#endif
