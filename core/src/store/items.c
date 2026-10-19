// items.c - an item as read, and the binary record SQLite keeps it in (items.h).
#include "items.h"

#include "util/bytea.h"
#include "util/err.h"
#include "../../../../shared/c/le_bytes.h"

#include <stdlib.h>
#include <string.h>

void cb_item_init(cb_item *it) {
    it->n = 0;
    cb_buf_init(&it->store);
}

void cb_item_reset(cb_item *it) {
    it->n = 0;
    cb_buf_reset(&it->store);
}

void cb_item_free(cb_item *it) {
    cb_buf_free(&it->store);
    it->n = 0;
}

static size_t put_str(cb_buf *b, const void *p, size_t n) {
    size_t off = b->len;
    cb_buf_append(b, p, n);
    cb_buf_putc(b, 0);
    return off;
}

bool cb_av_same(const cb_av *a, const cb_av *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case CB_AV_S: return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
    case CB_AV_N: return a->n == b->n;
    case CB_AV_BOOL: return a->b == b->b;
    case CB_AV_B: return a->len == b->len && (!a->len || memcmp(a->bytes, b->bytes, a->len) == 0);
    case CB_AV_SL:
        if (a->n_list != b->n_list) return false;
        for (int i = 0; i < a->n_list; i++)
            if (strcmp(a->list[i], b->list[i]) != 0) return false;
        return true;
    }
    return false;
}

bool cb_item_add(cb_item *it, const cb_av *v) {
    if (it->n >= CB_ITEM_ATTRS || !v->name) return false;
    int i = it->n;
    it->a[i].name = put_str(&it->store, v->name, strlen(v->name));
    it->a[i].kind = v->kind;
    it->a[i].n = v->n;
    it->a[i].b = v->b;
    it->a[i].off = it->a[i].len = 0;
    if (v->kind == CB_AV_S) {
        size_t n = v->s ? strlen(v->s) : 0;
        it->a[i].off = put_str(&it->store, v->s ? v->s : "", n);
        it->a[i].len = n;
    } else if (v->kind == CB_AV_B) {
        it->a[i].off = put_str(&it->store, v->bytes, v->len);
        it->a[i].len = v->len;
    } else if (v->kind == CB_AV_SL) {
        if (v->n_list < 0 || v->n_list > CB_ITEM_LIST_MAX) return false;
        it->a[i].off = it->store.len;
        it->a[i].len = (size_t)v->n_list;
        for (int k = 0; k < v->n_list; k++) put_str(&it->store, v->list[k] ? v->list[k] : "", v->list[k] ? strlen(v->list[k]) : 0);
    }
    if (it->store.oom) return false;
    it->n++;
    return true;
}

void cb_item_at(const cb_item *it, int i, cb_av *out) {
    const char *base = (const char *)it->store.p;
    memset(out, 0, sizeof *out);
    out->name = base + it->a[i].name;
    out->kind = it->a[i].kind;
    out->n = it->a[i].n;
    out->b = it->a[i].b;
    if (out->kind == CB_AV_S) out->s = base + it->a[i].off;
    if (out->kind == CB_AV_B) {
        out->bytes = (const uint8_t *)base + it->a[i].off;
        out->len = it->a[i].len;
    }
    if (out->kind == CB_AV_SL) {
        // The strings lie one after another; the item keeps their pointers
        // (a cache, which is why the cast: valid until the next SL is got).
        cb_item *m = (cb_item *)it;
        const char *p = base + it->a[i].off;
        int n = (int)it->a[i].len;
        for (int k = 0; k < n && k < CB_ITEM_LIST_MAX; k++) {
            m->list[k] = p;
            p += strlen(p) + 1;
        }
        out->list = m->list;
        out->n_list = n;
    }
}

bool cb_item_get(const cb_item *it, const char *name, cb_av *out) {
    const char *base = (const char *)it->store.p;
    for (int i = 0; i < it->n; i++)
        if (strcmp(base + it->a[i].name, name) == 0) {
            cb_item_at(it, i, out);
            return true;
        }
    return false;
}

bool cb_item_s(const cb_item *it, const char *name, char *out, size_t cap) {
    cb_av v;
    if (!cb_item_get(it, name, &v) || v.kind != CB_AV_S) return false;
    size_t n = strlen(v.s);
    if (n >= cap) return false;
    memcpy(out, v.s, n + 1);
    return true;
}

bool cb_item_n(const cb_item *it, const char *name, int64_t *out) {
    cb_av v;
    if (!cb_item_get(it, name, &v) || v.kind != CB_AV_N) return false;
    *out = v.n;
    return true;
}

bool cb_item_bool(const cb_item *it, const char *name, bool *out) {
    cb_av v;
    if (!cb_item_get(it, name, &v) || v.kind != CB_AV_BOOL) return false;
    *out = v.b;
    return true;
}

int64_t cb_item_n_or(const cb_item *it, const char *name, int64_t def) {
    int64_t v;
    return cb_item_n(it, name, &v) ? v : def;
}

int cb_item_text(const cb_item *it, cb_buf *st, const char **names, const char **values, int cap) {
    size_t noff[CB_ITEM_ATTRS], voff[CB_ITEM_ATTRS];
    int n = 0;
    cb_buf_reset(st);
    for (int i = 0; i < it->n && n < cap && n < CB_ITEM_ATTRS; i++, n++) {
        cb_av v;
        cb_item_at(it, i, &v);
        noff[n] = st->len;
        cb_buf_puts(st, v.name);
        cb_buf_putc(st, 0);
        voff[n] = st->len;
        switch (v.kind) {
        case CB_AV_S: cb_buf_puts(st, v.s); break;
        case CB_AV_N: cb_buf_printf(st, "%lld", (long long)v.n); break;
        case CB_AV_BOOL: cb_buf_puts(st, v.b ? "true" : "false"); break;
        case CB_AV_B: {
            char *hex = malloc(2 * v.len + 3);
            if (!hex) return -1;
            cb_bytea_encode(v.bytes, v.len, hex, 2 * v.len + 3);
            cb_buf_puts(st, hex);
            free(hex);
            break;
        }
        case CB_AV_SL:
            for (int k = 0; k < v.n_list; k++) cb_buf_printf(st, "%s%s", k ? "," : "", v.list[k]);
            break;
        }
        cb_buf_putc(st, 0);
    }
    if (st->oom) return -1;
    for (int i = 0; i < n; i++) {
        names[i] = (const char *)st->p + noff[i];
        values[i] = (const char *)st->p + voff[i];
    }
    return n;
}

// ---- the limits (items.h CB_ITEM_*) ---------------------------------------------------------

size_t cb_av_size(const cb_av *v) {
    size_t n = v->name ? strlen(v->name) : 0;
    switch (v->kind) {
    case CB_AV_S: return n + (v->s ? strlen(v->s) : 0);
    case CB_AV_N: return n + 21;
    case CB_AV_BOOL: return n + 1;
    case CB_AV_B: return n + v->len;
    case CB_AV_SL:
        n += 3;
        for (int i = 0; i < v->n_list; i++) n += (v->list[i] ? strlen(v->list[i]) : 0) + 1;
        return n;
    }
    return n;
}

size_t cb_item_size(const cb_item *it) {
    size_t n = 0;
    for (int i = 0; i < it->n; i++) {
        cb_av v;
        cb_item_at(it, i, &v);
        n += cb_av_size(&v);
    }
    return n;
}

static bool key_ok(const char *s) {
    if (!s || !*s) return false;
    return memchr(s, 0, CB_ITEM_KEY_MAX + 1) != NULL;   // at most CB_ITEM_KEY_MAX bytes
}

int cb_ikey_check(cb_ikey k) { return key_ok(k.pk) && key_ok(k.sk) ? CB_OK : CB_E_INVALID; }

static bool name_ok(const char *n) { return n && *n && memchr(n, 0, CB_ITEM_NAME_MAX + 1) != NULL; }

static bool is_key_name(const char *n) { return strcmp(n, CB_ITEM_PK) == 0 || strcmp(n, CB_ITEM_SK) == 0; }

static bool is_index_key(const char *n) {
    return strcmp(n, CB_ITEM_GSI1PK) == 0 || strcmp(n, CB_ITEM_GSI1SK) == 0 || strcmp(n, CB_ITEM_GSI2PK) == 0 ||
           strcmp(n, CB_ITEM_GSI2SK) == 0;
}

// A value a write sends: its kind's own shape, and an index key a key's.
static bool value_ok(const cb_av *v) {
    if (!name_ok(v->name)) return false;
    switch (v->kind) {
    case CB_AV_S: return !is_index_key(v->name) || key_ok(v->s);
    case CB_AV_N:
    case CB_AV_BOOL: return !is_index_key(v->name);
    case CB_AV_B: return !is_index_key(v->name) && (v->len == 0 || v->bytes);
    case CB_AV_SL:
        if (is_index_key(v->name) || v->n_list < 0 || v->n_list > CB_ITEM_LIST_MAX || (v->n_list && !v->list)) return false;
        return true;
    }
    return false;
}

// The distinct names one write names, to count them and to find one twice.
typedef struct {
    const char *n[CB_ITEM_ATTRS + CB_ITEM_UPDATE_NAMES + 4];
    int         count;
} names;

static bool names_has(const names *ns, const char *n) {
    for (int i = 0; i < ns->count; i++)
        if (strcmp(ns->n[i], n) == 0) return true;
    return false;
}

static void names_add(names *ns, const char *n) {
    if (!names_has(ns, n) && ns->count < (int)(sizeof ns->n / sizeof ns->n[0])) ns->n[ns->count++] = n;
}

// One write's checks; *size gets its bytes.
static bool one_ok(const cb_iwrite *x, bool txn, size_t *size) {
    if (x->op < CB_IW_PUT || x->op > CB_IW_CHECK || (x->op == CB_IW_CHECK && !txn)) return false;
    if (cb_ikey_check(x->key) != CB_OK) return false;
    if (x->n_cond < (x->op == CB_IW_CHECK ? 1 : 0) || x->n_cond > CB_ICOND_MAX) return false;
    names ns = { .count = 0 };
    size_t sz = strlen(CB_ITEM_PK) + strlen(x->key.pk) + strlen(CB_ITEM_SK) + strlen(x->key.sk);
    int n = x->op == CB_IW_PUT ? x->n_attrs : x->op == CB_IW_UPDATE ? x->n_sets : 0;
    if (n < 0 || n > CB_ITEM_ATTRS || (n > 0 && !(x->op == CB_IW_PUT ? (const void *)x->attrs : (const void *)x->sets)))
        return false;
    for (int i = 0; i < n; i++) {
        const cb_av *v = x->op == CB_IW_PUT ? &x->attrs[i] : &x->sets[i].v;
        bool remove = x->op == CB_IW_UPDATE && x->sets[i].how == CB_SET_REMOVE;
        if (remove ? !name_ok(v->name) : !value_ok(v)) return false;
        if (x->op == CB_IW_UPDATE && (x->sets[i].how < CB_SET_TO || x->sets[i].how > CB_SET_REMOVE)) return false;
        if (is_key_name(v->name) || names_has(&ns, v->name)) return false;
        names_add(&ns, v->name);
        if (!remove) sz += cb_av_size(v);
    }
    if ((x->op == CB_IW_PUT || x->op == CB_IW_UPDATE) && x->ttl_ms > 0) {
        if (names_has(&ns, CB_ITEM_TTL_MS) || names_has(&ns, CB_ITEM_TTL_S)) return false;
        names_add(&ns, CB_ITEM_TTL_MS);
        names_add(&ns, CB_ITEM_TTL_S);
        sz += strlen(CB_ITEM_TTL_MS) + 21 + strlen(CB_ITEM_TTL_S) + 21;
    }
    if (x->op == CB_IW_PUT && ns.count + 2 > CB_ITEM_ATTRS) return false;
    for (int i = 0; i < x->n_cond; i++) {
        const cb_iclause *c = &x->cond[i];
        if (c->check < CB_C_EXISTS || c->check > CB_C_ABSENT_OR_LT) return false;
        if (c->check == CB_C_EXISTS || c->check == CB_C_ABSENT) continue;
        if (!name_ok(c->v.name) || (c->check != CB_C_ATTR_EXISTS && !value_ok(&c->v))) return false;
        names_add(&ns, c->v.name);
    }
    if (x->op == CB_IW_UPDATE && ns.count > CB_ITEM_UPDATE_NAMES) return false;
    if (sz > CB_ITEM_SIZE_MAX) return false;
    *size = sz;
    return true;
}

int cb_iwrite_check(const cb_iwrite *w, int n, bool txn) {
    if (!w || n < 1 || n > (txn ? CB_ITEM_TXN_MAX : 1)) return CB_E_INVALID;
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        size_t sz = 0;
        if (!one_ok(&w[i], txn, &sz)) return CB_E_INVALID;
        total += sz;
        for (int j = 0; j < i; j++)
            if (strcmp(w[j].key.pk, w[i].key.pk) == 0 && strcmp(w[j].key.sk, w[i].key.sk) == 0) return CB_E_INVALID;
    }
    return total <= CB_ITEM_TXN_SIZE_MAX ? CB_OK : CB_E_INVALID;
}

int cb_iquery_check(const cb_iquery *q) {
    if (!q || q->index < 0 || q->index > 2 || !key_ok(q->pk)) return CB_E_INVALID;
    if ((q->sk_prefix && !key_ok(q->sk_prefix)) || (q->sk_below && !key_ok(q->sk_below))) return CB_E_INVALID;
    return CB_OK;
}

// ---- the record --------------------------------------------------------------------------

static void put_u32(cb_buf *b, uint32_t v) {
    uint8_t t[4];
    le_put_u32(t, v);
    cb_buf_append(b, t, 4);
}

bool cb_item_record_encode(const cb_item *it, cb_buf *out) {
    const char *base = (const char *)it->store.p;
    for (int i = 0; i < it->n; i++) {
        const char *name = base + it->a[i].name;
        size_t nl = strlen(name);
        if (nl > 255) return false;
        cb_buf_putc(out, (char)nl);
        cb_buf_append(out, name, nl);
        cb_buf_putc(out, (char)it->a[i].kind);
        switch (it->a[i].kind) {
        case CB_AV_S:
        case CB_AV_B:
            if (it->a[i].len > UINT32_MAX) return false;
            put_u32(out, (uint32_t)it->a[i].len);
            cb_buf_append(out, base + it->a[i].off, it->a[i].len);
            break;
        case CB_AV_N: {
            uint64_t v = (uint64_t)it->a[i].n;
            put_u32(out, (uint32_t)v);
            put_u32(out, (uint32_t)(v >> 32));
            break;
        }
        case CB_AV_BOOL: cb_buf_putc(out, it->a[i].b ? 1 : 0); break;
        case CB_AV_SL: {
            put_u32(out, (uint32_t)it->a[i].len);
            const char *p = base + it->a[i].off;
            for (size_t k = 0; k < it->a[i].len; k++) {
                size_t sl = strlen(p);
                put_u32(out, (uint32_t)sl);
                cb_buf_append(out, p, sl);
                p += sl + 1;
            }
            break;
        }
        default: return false;
        }
    }
    return !out->oom;
}

bool cb_item_record_decode(const uint8_t *p, size_t n, cb_item *it) {
    size_t i = 0;
    while (i < n) {
        size_t nl = p[i++];
        if (nl == 0 || nl > n - i) return false;
        char name[256];
        memcpy(name, p + i, nl);
        name[nl] = 0;
        i += nl;
        if (i >= n) return false;
        cb_av v = { .name = name, .kind = (cb_av_kind)p[i++] };
        char *text = NULL;
        switch (v.kind) {
        case CB_AV_S:
        case CB_AV_B: {
            if (n - i < 4) return false;
            uint32_t len = le_get_u32(p + i);
            i += 4;
            if (len > n - i) return false;
            if (v.kind == CB_AV_B) {
                v.bytes = p + i;
                v.len = len;
            } else {
                if (memchr(p + i, 0, len)) return false;
                text = malloc(len + 1);
                if (!text) return false;
                memcpy(text, p + i, len);
                text[len] = 0;
                v.s = text;
            }
            i += len;
            break;
        }
        case CB_AV_N:
            if (n - i < 8) return false;
            v.n = (int64_t)((uint64_t)le_get_u32(p + i) | (uint64_t)le_get_u32(p + i + 4) << 32);
            i += 8;
            break;
        case CB_AV_BOOL:
            if (n - i < 1 || p[i] > 1) return false;
            v.b = p[i++] == 1;
            break;
        case CB_AV_SL: {
            if (n - i < 4) return false;
            uint32_t count = le_get_u32(p + i);
            i += 4;
            if (count > CB_ITEM_LIST_MAX) return false;
            static _Thread_local char strs[CB_ITEM_LIST_MAX][256];
            static _Thread_local const char *ptrs[CB_ITEM_LIST_MAX];
            for (uint32_t k = 0; k < count; k++) {
                if (n - i < 4) return false;
                uint32_t sl = le_get_u32(p + i);
                i += 4;
                if (sl > n - i || sl >= sizeof strs[0] || memchr(p + i, 0, sl)) return false;
                memcpy(strs[k], p + i, sl);
                strs[k][sl] = 0;
                ptrs[k] = strs[k];
                i += sl;
            }
            v.list = ptrs;
            v.n_list = (int)count;
            break;
        }
        default: return false;
        }
        bool ok = cb_item_add(it, &v);
        free(text);
        if (!ok) return false;
    }
    return true;
}
