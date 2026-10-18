// items.c - an item as read, and the binary record SQLite keeps it in (items.h).
#include "items.h"

#include "util/bytea.h"

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

// ---- the record --------------------------------------------------------------------------

static void put_u32(cb_buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    cb_buf_append(b, t, 4);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
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
            uint32_t len = get_u32(p + i);
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
            v.n = (int64_t)((uint64_t)get_u32(p + i) | (uint64_t)get_u32(p + i + 4) << 32);
            i += 8;
            break;
        case CB_AV_BOOL:
            if (n - i < 1 || p[i] > 1) return false;
            v.b = p[i++] == 1;
            break;
        case CB_AV_SL: {
            if (n - i < 4) return false;
            uint32_t count = get_u32(p + i);
            i += 4;
            if (count > CB_ITEM_LIST_MAX) return false;
            static _Thread_local char strs[CB_ITEM_LIST_MAX][256];
            static _Thread_local const char *ptrs[CB_ITEM_LIST_MAX];
            for (uint32_t k = 0; k < count; k++) {
                if (n - i < 4) return false;
                uint32_t sl = get_u32(p + i);
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
