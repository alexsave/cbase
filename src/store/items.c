// items.c - an item as read, and the binary record SQLite keeps it in (items.h).
#include "items.h"

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
        default: return false;
        }
        bool ok = cb_item_add(it, &v);
        free(text);
        if (!ok) return false;
    }
    return true;
}
