/* ------------------------------------------------ streamed SET (values larger than rx buffer)
 * Included at the end of kv_opt.c. The server parses "SET key <len>" and hands the value over as
 * it arrives from the network, so a value of any size needs no receive buffer larger than 4 KB
 * (with the 40 KB deployment reserve a 10 KB SET could not get a 16 KB receive buffer).
 * begin() allocates the whole chain up front (pages pinned so eviction, page stealing and the
 * adaptive release cannot take them), write() copies bytes straight into the chunks, commit()
 * publishes and indexes the entry (replacing an old value), abort() returns the chunks.
 * Only for values that need a chain; smaller ones always fit the receive buffer. */

static uint8_t *stream_dst(kv_stream_t *st, size_t at, size_t *avail)
{
    if (at < st->first) {
        *avail = st->first - at;
        return (uint8_t *)ref2e(st->ref) + st->voff + at;
    }
    size_t co = at - st->first, i = co / CONT_DATA;
    *avail = CONT_DATA - co % CONT_DATA;
    return (uint8_t *)((chdr_t *)ref2p(st->conts[i]) + 1) + co % CONT_DATA;
}

static void pin_one(uint32_t ref, int on)
{
    phdr_t *ph = page_hdr(ref >> REF_OFF_BITS);
    ph->shard = on ? (ph->shard | PG_PINNED) : (ph->shard & ~PG_PINNED);
}

static void stream_release(shard_t *sh, kv_stream_t *st, int nconts)
{
    for (int i = 0; i < nconts; i++) {
        pin_one(st->conts[i], 0);
        chunk_put(sh, st->conts[i]);
    }
    pin_one(st->ref, 0);
    chunk_put(sh, st->ref);
}

int kv_stream_begin(kv_stream_t *st, const uint8_t *k, size_t kl, size_t vl)
{
    memset(st, 0, sizeof *st);
    if (kl > MAX_KEY || vl > MAX_VAL) return KV_ETOOBIG;
    memcpy(st->key, k, kl);
    key_t q;
    mkkey(&q, st->key, kl, 1);
    st->pid = q.pid;
    st->plen = q.plen;
    st->h = q.h;
    st->kl = kl;
    int fl = F_CHAIN | F_TTL | (q.pid ? F_PFX : 0);
    size_t skl = q.pid ? kl - q.plen : kl;
    st->voff = e_prefix_bytes(fl, skl);
    if (cls_for(sizeof(ehdr_t) + (q.pid ? 1 : 0) + skl + vl) >= 0) return KV_ETOOBIG; /* fits: normal SET */
    st->first = cls_size[CLS_MAX] - st->voff;
    st->nconts = (vl - st->first + CONT_DATA - 1) / CONT_DATA;
    st->fl = fl;
    st->vl = vl;
    shard_t *sh = shard_of(q.h);
    int rc = KV_OK;
    SH_LOCK(sh);
    if (sh->count >= sh->max_count && !(evict_on && evict_in_class(sh, CLS_MAX))) {
        int c2;
        for (c2 = 0; c2 < (int)NCLS && !(evict_on && evict_in_class(sh, c2)); c2++) {
        }
        if (c2 == (int)NCLS) rc = KV_ENOMEM;
    }
    if (rc == KV_OK) {
        st->ref = chunk_alloc(sh, CLS_MAX);
        if (!st->ref) {
            rc = KV_ENOMEM;
        } else {
            ((chdr_t *)ref2p(st->ref))->flags = F_CONT; /* not an entry yet; never evicted */
            ((chdr_t *)ref2p(st->ref))->head = 0;
            pin_one(st->ref, 1);
            int got;
            for (got = 0; got < st->nconts; got++) {
                st->conts[got] = chunk_alloc(sh, CLS_MAX);
                if (!st->conts[got]) break;
                ((chdr_t *)ref2p(st->conts[got]))->flags = F_CONT;
                ((chdr_t *)ref2p(st->conts[got]))->head = 0;
                pin_one(st->conts[got], 1);
            }
            if (got < st->nconts) {
                stream_release(sh, st, got);
                rc = KV_ENOMEM;
            }
        }
    }
    SH_UNLOCK(sh);
    if (rc == KV_OK) {
        st->gen = store_gen;
        st->active = 1;
    }
    return rc;
}

size_t kv_stream_write(kv_stream_t *st, const uint8_t *p, size_t n)
{
    size_t done = 0;
    if (!st->active || st->gen != store_gen) return n; /* flushed meanwhile: swallow the bytes */
    while (done < n && st->written < st->vl) {
        size_t avail;
        uint8_t *d = stream_dst(st, st->written, &avail);
        size_t take = n - done < avail ? n - done : avail;
        if (take > st->vl - st->written) take = st->vl - st->written;
        memcpy(d, p + done, take);
        done += take;
        st->written += take;
    }
    return done;
}

void kv_stream_abort(kv_stream_t *st)
{
    if (!st->active) return;
    st->active = 0;
    if (st->gen != store_gen) return; /* chunks already gone with the flush */
    shard_t *sh = shard_of(st->h);
    SH_LOCK(sh);
    stream_release(sh, st, st->nconts);
    SH_UNLOCK(sh);
}

int kv_stream_commit(kv_stream_t *st)
{
    if (!st->active) return KV_ENOMEM;
    if (st->gen != store_gen || st->written != st->vl) {
        kv_stream_abort(st);
        return KV_ENOMEM;
    }
    st->active = 0;
    key_t q = {.k = st->key, .h = st->h, .kl = st->kl, .plen = st->plen, .pid = st->pid};
    shard_t *sh = shard_of(q.h);
    SH_LOCK(sh);
    size_t left = st->vl - st->first;
    for (int i = 0; i < st->nconts; i++) { /* continuation headers */
        chdr_t *c = ref2p(st->conts[i]);
        c->len = left < CONT_DATA ? left : CONT_DATA;
        left -= c->len;
        c->next = i + 1 < st->nconts ? st->conts[i + 1] : 0;
        c->head = st->ref;
        c->flags = F_CONT;
        pin_one(st->conts[i], 0);
    }
    ehdr_t *e = ref2e(st->ref); /* head entry header */
    size_t skl = q.pid ? q.kl - q.plen : q.kl;
    e->vlen = st->vl;
    e->klen = skl;
    uint8_t *p = (uint8_t *)(e + 1);
    uint32_t noexp = 0;
    memcpy(p, &noexp, 4);
    p += 4;
    if (q.pid) *p++ = q.pid;
    memcpy(p, q.pid ? q.k + q.plen : q.k, skl);
    p += skl;
    memcpy(p, &st->conts[0], 4);
    pin_one(st->ref, 0);
    __sync_synchronize();
    e->flags = F_USED | F_REF | st->fl; /* publish last */
    uint32_t pos;
    ehdr_t *old = lookup(sh, &q, &pos);
    if (old) {
        uint32_t oref = sh->idx[pos] & REF_MASK;
        sh->idx[pos] = SLOT(TAG_OF(q.h), st->ref);
        chunk_free(sh, oref);
    } else {
        sh->idx[pos] = SLOT(TAG_OF(q.h), st->ref);
        sh->count++;
    }
    SH_UNLOCK(sh);
    return KV_OK;
}
