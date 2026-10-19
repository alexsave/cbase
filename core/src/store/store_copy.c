// store_copy.c - see store_copy.h.
#include "store/store_copy.h"

#include "store/store_priv.h"

#include <string.h>

int cb_copy_put(cb_store *dst, cb_ikey key, const cb_item *it) {
    cb_av attrs[CB_ITEM_ATTRS];
    int na = 0;
    for (int a = 0; a < it->n && na < CB_ITEM_ATTRS; a++) {
        cb_av v;
        cb_item_at(it, a, &v);
        if (strcmp(v.name, CB_ITEM_PK) == 0 || strcmp(v.name, CB_ITEM_SK) == 0) continue;
        attrs[na++] = v;
    }
    cb_iwrite w;
    memset(&w, 0, sizeof w);
    w.op = CB_IW_PUT;
    w.key = key;
    w.attrs = attrs;
    w.n_attrs = na;
    return dst->ops->item_write(dst, &w, NULL);
}

bool cb_copy_same(cb_store *dst, cb_ikey key, const cb_item *src) {
    cb_item got;
    cb_item_init(&got);
    bool same = dst->ops->item_get(dst, key, true, &got) == CB_OK && got.n == src->n;
    for (int a = 0; same && a < src->n; a++) {
        cb_av want, have;
        cb_item_at(src, a, &want);
        same = cb_item_get(&got, want.name, &have) && cb_av_same(&want, &have);
    }
    cb_item_free(&got);
    return same;
}

void cb_copy_family(const char *pk, char *out, size_t cap) {
    const char *h = strchr(pk, '#');
    size_t n = h ? (size_t)(h - pk) : strlen(pk);
    if (n >= cap) n = cap - 1;
    memcpy(out, pk, n);
    out[n] = 0;
}

typedef struct {
    const char *pk, *sk;
    bool        found;
} finder;

static bool find_item(const cb_item *it, void *ud) {
    finder *f = ud;
    char pk[CB_ITEM_KEY_MAX + 1], sk[CB_ITEM_KEY_MAX + 1];
    if (cb_item_s(it, CB_ITEM_PK, pk, sizeof pk) && cb_item_s(it, CB_ITEM_SK, sk, sizeof sk) && strcmp(pk, f->pk) == 0 &&
        strcmp(sk, f->sk) == 0)
        f->found = true;
    return !f->found;
}

int cb_copy_index_roundtrip(cb_store *dst, cb_ikey key, int *indexed) {
    static const char *const PK[2] = { CB_ITEM_GSI1PK, CB_ITEM_GSI2PK }, *const SK[2] = { CB_ITEM_GSI1SK, CB_ITEM_GSI2SK };
    *indexed = 0;
    cb_item it;
    cb_item_init(&it);
    if (dst->ops->item_get(dst, key, true, &it) != CB_OK) {
        cb_item_free(&it);
        return -1;
    }
    int bad = 0;
    for (int i = 0; i < 2; i++) {
        char gp[CB_ITEM_KEY_MAX + 1], gs[CB_ITEM_KEY_MAX + 1];
        if (!cb_item_s(&it, PK[i], gp, sizeof gp) || !cb_item_s(&it, SK[i], gs, sizeof gs)) continue;
        (*indexed)++;
        finder f = { key.pk, key.sk, false };
        cb_iquery q = { .index = i + 1, .pk = gp, .sk_prefix = gs, .forward = true };
        if (dst->ops->item_query(dst, &q, find_item, &f) != CB_OK || !f.found) bad++;
    }
    cb_item_free(&it);
    return bad;
}
