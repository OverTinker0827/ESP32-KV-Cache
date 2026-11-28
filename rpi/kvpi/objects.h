/* kvpi object values: lists, hashes, sets, sorted sets. An object belongs to one key in one stripe
 * and is only touched with that stripe's writer lock held (no optimistic reads of objects). */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { OBJ_LIST = 1, OBJ_HASH = 2, OBJ_SET = 3, OBJ_ZSET = 4 };

typedef struct {
    uint32_t len;
    char d[];
} str_t;

typedef struct obj obj_t;
int obj_type(const obj_t *o);
void obj_free(void *o); /* engine callback */

/* lists */
obj_t *list_new(void);
size_t list_len(const obj_t *o);
void list_push(obj_t *o, int left, const char *s, size_t n);
str_t *list_pop(obj_t *o, int left); /* caller frees */
str_t *list_at(const obj_t *o, long i); /* borrowed; negative = from the end */

/* hashes (field -> value) and sets (members) */
obj_t *hash_new(void);
obj_t *set_new(void);
size_t dict_len(const obj_t *o);
int hash_set(obj_t *o, const char *f, size_t fl, const char *v, size_t vl); /* 1 new field, 0 updated */
str_t *hash_get(const obj_t *o, const char *f, size_t fl);
int dict_del(obj_t *o, const char *f, size_t fl); /* hash field or set member */
int set_add(obj_t *o, const char *m, size_t ml);
int set_has(const obj_t *o, const char *m, size_t ml);
str_t *set_pop(obj_t *o); /* random member, caller frees */
/* iterate fields/members (and values for hashes): cb returns nothing */
void dict_each(const obj_t *o, void (*cb)(void *arg, const str_t *k, const str_t *v), void *arg);

/* sorted sets */
obj_t *zset_new(void);
size_t zset_len(const obj_t *o);
int zset_add(obj_t *o, double score, const char *m, size_t ml, int incr, double *out); /* 1 new member */
int zset_score(const obj_t *o, const char *m, size_t ml, double *out);
int zset_rem(obj_t *o, const char *m, size_t ml);
str_t *zset_popmin(obj_t *o, double *score); /* caller frees */
/* members with rank in [start, stop] (already normalised, inclusive) */
void zset_range(const obj_t *o, long start, long stop, void (*cb)(void *arg, const str_t *m, double s), void *arg);
