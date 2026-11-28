/* kvpi object values (see objects.h). Plain single-threaded structures:
 *  - list: power-of-two ring buffer of str_t*  (O(1) push/pop both ends, O(1) index)
 *  - dict: open addressing, linear probing, stored hash per slot, tombstone-free backward-shift delete;
 *          used for hashes (value = str_t*), sets (no value) and the member index of sorted sets
 *  - zset: skiplist ordered by (score, member) + dict member -> node */
#include "objects.h"
#include <stdlib.h>
#include <string.h>
#include "engine.h"

struct obj {
    uint8_t type;
};

static str_t *str_new(const char *s, size_t n)
{
    str_t *x = malloc(sizeof *x + n);
    x->len = (uint32_t)n;
    memcpy(x->d, s, n);
    return x;
}
static inline int str_eq(const str_t *a, const char *s, size_t n) { return a->len == n && memcmp(a->d, s, n) == 0; }

int obj_type(const obj_t *o) { return o->type; }

/* ---------------------------------------------------------------- list */
typedef struct {
    struct obj h;
    str_t **v;
    size_t head, len, cap;
} list_t;

obj_t *list_new(void)
{
    list_t *l = calloc(1, sizeof *l);
    l->h.type = OBJ_LIST;
    l->cap = 8;
    l->v = malloc(l->cap * sizeof *l->v);
    return &l->h;
}
size_t list_len(const obj_t *o) { return ((const list_t *)o)->len; }
static void list_grow(list_t *l)
{
    str_t **n = malloc(l->cap * 2 * sizeof *n);
    for (size_t i = 0; i < l->len; i++) n[i] = l->v[(l->head + i) & (l->cap - 1)];
    free(l->v);
    l->v = n, l->head = 0, l->cap *= 2;
}
void list_push(obj_t *o, int left, const char *s, size_t n)
{
    list_t *l = (list_t *)o;
    if (l->len == l->cap) list_grow(l);
    str_t *x = str_new(s, n);
    if (left) {
        l->head = (l->head - 1) & (l->cap - 1);
        l->v[l->head] = x;
    } else {
        l->v[(l->head + l->len) & (l->cap - 1)] = x;
    }
    l->len++;
}
str_t *list_pop(obj_t *o, int left)
{
    list_t *l = (list_t *)o;
    if (!l->len) return NULL;
    str_t *x;
    if (left) {
        x = l->v[l->head];
        l->head = (l->head + 1) & (l->cap - 1);
    } else {
        x = l->v[(l->head + l->len - 1) & (l->cap - 1)];
    }
    l->len--;
    return x;
}
str_t *list_at(const obj_t *o, long i)
{
    const list_t *l = (const list_t *)o;
    if (i < 0) i += (long)l->len;
    if (i < 0 || (size_t)i >= l->len) return NULL;
    return l->v[(l->head + (size_t)i) & (l->cap - 1)];
}

/* ---------------------------------------------------------------- dict */
typedef struct {
    uint64_t h; /* 0 = empty (hashes are forced non-zero) */
    str_t *k;
    void *v;
} dent_t;
typedef struct {
    struct obj hd;
    dent_t *t;
    size_t mask, n;
    uint64_t rng;
} dict_t;

static inline uint64_t dh(const char *s, size_t n)
{
    uint64_t h = kv_hash(s, n);
    return h ? h : 1;
}
static dict_t *dict_init(dict_t *d, int type)
{
    d->hd.type = (uint8_t)type;
    d->mask = 7;
    d->t = calloc(8, sizeof *d->t);
    d->rng = 0x9E3779B97F4A7C15ull ^ (uintptr_t)d;
    return d;
}
obj_t *hash_new(void) { return &dict_init(calloc(1, sizeof(dict_t)), OBJ_HASH)->hd; }
obj_t *set_new(void) { return &dict_init(calloc(1, sizeof(dict_t)), OBJ_SET)->hd; }
size_t dict_len(const obj_t *o) { return ((const dict_t *)o)->n; }

static long d_find(const dict_t *d, const char *k, size_t kl, uint64_t h)
{
    for (size_t p = h & d->mask;; p = (p + 1) & d->mask) {
        const dent_t *e = &d->t[p];
        if (!e->h) return -(long)p - 1;
        if (e->h == h && str_eq(e->k, k, kl)) return (long)p;
    }
}
static void d_grow(dict_t *d)
{
    size_t on = d->mask + 1, nn = on * 2;
    dent_t *old = d->t;
    d->t = calloc(nn, sizeof *d->t);
    d->mask = nn - 1;
    for (size_t i = 0; i < on; i++)
        if (old[i].h) {
            size_t p = old[i].h & d->mask;
            while (d->t[p].h) p = (p + 1) & d->mask;
            d->t[p] = old[i];
        }
    free(old);
}
/* returns the entry for k, inserting an empty-valued one if absent (*isnew = 1) */
static dent_t *d_upsert(dict_t *d, const char *k, size_t kl, int *isnew)
{
    uint64_t h = dh(k, kl);
    long p = d_find(d, k, kl, h);
    if (p >= 0) {
        *isnew = 0;
        return &d->t[p];
    }
    if ((d->n + 1) * 10 > (d->mask + 1) * 7) {
        d_grow(d);
        p = d_find(d, k, kl, h);
    }
    dent_t *e = &d->t[-p - 1];
    e->h = h, e->k = str_new(k, kl), e->v = NULL;
    d->n++;
    *isnew = 1;
    return e;
}
static void d_remove_at(dict_t *d, size_t i)
{
    d->t[i].h = 0;
    d->n--;
    for (size_t j = (i + 1) & d->mask; d->t[j].h; j = (j + 1) & d->mask) {
        size_t home = d->t[j].h & d->mask;
        if (((j - home) & d->mask) >= ((j - i) & d->mask)) {
            d->t[i] = d->t[j];
            d->t[j].h = 0;
            i = j;
        }
    }
}
static dent_t *d_get(const dict_t *d, const char *k, size_t kl)
{
    long p = d_find(d, k, kl, dh(k, kl));
    return p >= 0 ? &d->t[p] : NULL;
}

int hash_set(obj_t *o, const char *f, size_t fl, const char *v, size_t vl)
{
    int isnew;
    dent_t *e = d_upsert((dict_t *)o, f, fl, &isnew);
    free(e->v);
    e->v = str_new(v, vl);
    return isnew;
}
str_t *hash_get(const obj_t *o, const char *f, size_t fl)
{
    dent_t *e = d_get((const dict_t *)o, f, fl);
    return e ? e->v : NULL;
}
int dict_del(obj_t *o, const char *f, size_t fl)
{
    dict_t *d = (dict_t *)o;
    long p = d_find(d, f, fl, dh(f, fl));
    if (p < 0) return 0;
    free(d->t[p].k);
    if (d->hd.type == OBJ_HASH) free(d->t[p].v);
    d_remove_at(d, (size_t)p);
    return 1;
}
int set_add(obj_t *o, const char *m, size_t ml)
{
    int isnew;
    d_upsert((dict_t *)o, m, ml, &isnew);
    return isnew;
}
int set_has(const obj_t *o, const char *m, size_t ml) { return d_get((const dict_t *)o, m, ml) != NULL; }
str_t *set_pop(obj_t *o)
{
    dict_t *d = (dict_t *)o;
    if (!d->n) return NULL;
    uint64_t x = d->rng;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    d->rng = x;
    size_t p = x & d->mask;
    while (!d->t[p].h) p = (p + 1) & d->mask;
    str_t *k = d->t[p].k;
    d_remove_at(d, p);
    return k;
}
void dict_each(const obj_t *o, void (*cb)(void *, const str_t *, const str_t *), void *arg)
{
    const dict_t *d = (const dict_t *)o;
    for (size_t i = 0; i <= d->mask; i++)
        if (d->t[i].h) cb(arg, d->t[i].k, d->hd.type == OBJ_HASH ? d->t[i].v : NULL);
}

/* ---------------------------------------------------------------- zset */
#define ZMAXLVL 24
typedef struct znode {
    double score;
    str_t *m;
    struct znode *back;
    struct {
        struct znode *next;
        size_t span;
    } lv[];
} znode_t;
typedef struct {
    struct obj hd;
    dict_t members; /* member -> znode_t* (key strings owned by the dict) */
    znode_t *head;
    int level;
    size_t len;
    uint64_t rng;
} zset_t;

static znode_t *zn_new(int lvl, double s, str_t *m)
{
    znode_t *n = calloc(1, sizeof *n + lvl * sizeof n->lv[0]);
    n->score = s, n->m = m;
    return n;
}
obj_t *zset_new(void)
{
    zset_t *z = calloc(1, sizeof *z);
    z->hd.type = OBJ_ZSET;
    dict_init(&z->members, OBJ_SET);
    z->head = zn_new(ZMAXLVL, 0, NULL);
    z->level = 1;
    z->rng = 0x2545F4914F6CDD1Dull ^ (uintptr_t)z;
    return &z->hd;
}
size_t zset_len(const obj_t *o) { return ((const zset_t *)o)->len; }
static int zcmp(double s, const str_t *m, double s2, const str_t *m2)
{
    if (s != s2) return s < s2 ? -1 : 1;
    size_t n = m->len < m2->len ? m->len : m2->len;
    int c = memcmp(m->d, m2->d, n);
    return c ? c : (m->len < m2->len ? -1 : m->len > m2->len);
}
static int zrand_level(zset_t *z)
{
    uint64_t x = z->rng;
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    z->rng = x;
    int l = 1;
    while (l < ZMAXLVL && (x & 3) == 0) l++, x >>= 2; /* p = 1/4 */
    return l;
}
static void z_insert(zset_t *z, double s, str_t *m)
{
    znode_t *upd[ZMAXLVL], *x = z->head;
    size_t rank[ZMAXLVL];
    for (int i = z->level - 1; i >= 0; i--) {
        rank[i] = i == z->level - 1 ? 0 : rank[i + 1];
        while (x->lv[i].next && zcmp(x->lv[i].next->score, x->lv[i].next->m, s, m) < 0)
            rank[i] += x->lv[i].span, x = x->lv[i].next;
        upd[i] = x;
    }
    int lvl = zrand_level(z);
    if (lvl > z->level) {
        for (int i = z->level; i < lvl; i++) rank[i] = 0, upd[i] = z->head, z->head->lv[i].span = z->len;
        z->level = lvl;
    }
    x = zn_new(lvl, s, m);
    for (int i = 0; i < lvl; i++) {
        x->lv[i].next = upd[i]->lv[i].next;
        upd[i]->lv[i].next = x;
        x->lv[i].span = upd[i]->lv[i].span - (rank[0] - rank[i]);
        upd[i]->lv[i].span = (rank[0] - rank[i]) + 1;
    }
    for (int i = lvl; i < z->level; i++) upd[i]->lv[i].span++;
    x->back = upd[0] == z->head ? NULL : upd[0];
    if (x->lv[0].next) x->lv[0].next->back = x;
    z->len++;
}
static void z_unlink(zset_t *z, double s, const str_t *m)
{
    znode_t *upd[ZMAXLVL], *x = z->head;
    for (int i = z->level - 1; i >= 0; i--) {
        while (x->lv[i].next && zcmp(x->lv[i].next->score, x->lv[i].next->m, s, m) < 0) x = x->lv[i].next;
        upd[i] = x;
    }
    x = x->lv[0].next;
    if (!x || zcmp(x->score, x->m, s, m) != 0) return;
    for (int i = 0; i < z->level; i++) {
        if (upd[i]->lv[i].next == x) {
            upd[i]->lv[i].span += x->lv[i].span - 1;
            upd[i]->lv[i].next = x->lv[i].next;
        } else {
            upd[i]->lv[i].span--;
        }
    }
    if (x->lv[0].next) x->lv[0].next->back = x->back;
    while (z->level > 1 && !z->head->lv[z->level - 1].next) z->level--;
    z->len--;
    free(x);
}
int zset_add(obj_t *o, double score, const char *m, size_t ml, int incr, double *out)
{
    zset_t *z = (zset_t *)o;
    int isnew;
    dent_t *e = d_upsert(&z->members, m, ml, &isnew);
    if (!isnew) {
        znode_t *n = e->v;
        double ns = incr ? n->score + score : score;
        if (ns != n->score) {
            z_unlink(z, n->score, e->k);
            z_insert(z, ns, e->k);
            /* find the new node for the dict */
            znode_t *x = z->head;
            for (int i = z->level - 1; i >= 0; i--)
                while (x->lv[i].next && zcmp(x->lv[i].next->score, x->lv[i].next->m, ns, e->k) < 0) x = x->lv[i].next;
            e->v = x->lv[0].next;
        }
        if (out) *out = ns;
        return 0;
    }
    z_insert(z, score, e->k);
    znode_t *x = z->head;
    for (int i = z->level - 1; i >= 0; i--)
        while (x->lv[i].next && zcmp(x->lv[i].next->score, x->lv[i].next->m, score, e->k) < 0) x = x->lv[i].next;
    e->v = x->lv[0].next;
    if (out) *out = score;
    return 1;
}
int zset_score(const obj_t *o, const char *m, size_t ml, double *out)
{
    dent_t *e = d_get(&((const zset_t *)o)->members, m, ml);
    if (!e) return 0;
    *out = ((znode_t *)e->v)->score;
    return 1;
}
int zset_rem(obj_t *o, const char *m, size_t ml)
{
    zset_t *z = (zset_t *)o;
    long p = d_find(&z->members, m, ml, dh(m, ml));
    if (p < 0) return 0;
    dent_t *e = &z->members.t[p];
    z_unlink(z, ((znode_t *)e->v)->score, e->k);
    free(e->k);
    d_remove_at(&z->members, (size_t)p);
    return 1;
}
str_t *zset_popmin(obj_t *o, double *score)
{
    zset_t *z = (zset_t *)o;
    znode_t *x = z->head->lv[0].next;
    if (!x) return NULL;
    str_t *m = str_new(x->m->d, x->m->len);
    *score = x->score;
    zset_rem(o, m->d, m->len);
    return m;
}
void zset_range(const obj_t *o, long start, long stop, void (*cb)(void *, const str_t *, double), void *arg)
{
    const zset_t *z = (const zset_t *)o;
    znode_t *x = z->head->lv[0].next;
    for (long r = 0; x && r <= stop; r++, x = x->lv[0].next)
        if (r >= start) cb(arg, x->m, x->score);
}

/* ---------------------------------------------------------------- free */
static void dict_free(dict_t *d)
{
    for (size_t i = 0; i <= d->mask; i++)
        if (d->t[i].h) {
            free(d->t[i].k);
            if (d->hd.type == OBJ_HASH) free(d->t[i].v);
        }
    free(d->t);
}
void obj_free(void *p)
{
    obj_t *o = p;
    switch (o->type) {
    case OBJ_LIST: {
        list_t *l = (list_t *)o;
        for (size_t i = 0; i < l->len; i++) free(l->v[(l->head + i) & (l->cap - 1)]);
        free(l->v);
        break;
    }
    case OBJ_HASH:
    case OBJ_SET:
        dict_free((dict_t *)o);
        break;
    case OBJ_ZSET: {
        zset_t *z = (zset_t *)o;
        znode_t *x = z->head->lv[0].next;
        while (x) {
            znode_t *nx = x->lv[0].next;
            free(x);
            x = nx;
        }
        free(z->head);
        dict_free(&z->members); /* frees member strings */
        break;
    }
    }
    free(o);
}
