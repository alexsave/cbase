// media.c - see media.h.
#include "media.h"

#include "config.h"
#include "hex/hex.h"
#include "util/blob.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include <stdio.h>
#include <string.h>

#define MOD "media"

#define MEDIA_DEFAULT_BYTES (5u * 1024 * 1024)
#define MEDIA_MAX_BYTES     (10u * 1024 * 1024)
#define MEDIA_EXPIRES_S     300   // a form is good for 5 minutes

// The types a client may upload, each with the extension its key ends in. Exact strings: nothing is
// folded or trimmed, and SVG (a document that runs script) is not here.
static const struct { const char *type, *ext; } TYPES[] = {
    { "image/jpeg", ".jpg" }, { "image/png", ".png" }, { "image/webp", ".webp" },
};

// What cb_media_configure keeps. Written once at boot, before any request thread exists; read only after.
static cb_blob g_blob;
static bool    g_on;
static char    g_prefix[128];

int cb_media_post_with(const cb_blob *blob, const char *prefix, const char *content_type, uint64_t max_bytes,
                       int64_t now_ms, cb_media_form *out) {
    if (!blob || !blob->post_form) return CB_E_STATE;
    const char *ext = NULL;
    for (size_t i = 0; content_type && i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (strcmp(content_type, TYPES[i].type) == 0) ext = TYPES[i].ext;
    if (!ext || !prefix || !*prefix) return CB_E_INVALID;
    if (max_bytes == 0) max_bytes = MEDIA_DEFAULT_BYTES;
    if (max_bytes > MEDIA_MAX_BYTES) return CB_E_INVALID;
    uint8_t raw[16];
    cb_random_bytes(raw, sizeof raw);
    char id[33], key[256];
    hex_encode(raw, sizeof raw, id, sizeof id);
    if (snprintf(key, sizeof key, "%s%s%s", prefix, id, ext) >= (int)sizeof key) return CB_E_INVALID;
    return blob->post_form(blob->ud, key, content_type, max_bytes, now_ms, MEDIA_EXPIRES_S, out);
}

void cb_media_close(void) {
    if (g_blob.close) g_blob.close(g_blob.ud);
    memset(&g_blob, 0, sizeof g_blob);
    g_on = false;
    g_prefix[0] = 0;
}

int cb_media_configure(const cb_config *cfg, char *err, size_t cap) {
    cb_media_close();   // a second configure (a test, a reboot in one process) frees the place the first opened
    if (!cfg->media_bucket[0]) return CB_OK;
    const cb_blob_backend *b = cb_blob_backends[0];
    if (!b) {
        snprintf(err, cap, "CBASE_MEDIA_BUCKET is set and this binary links no object store backend to post an upload to");
        return CB_E_INVALID;
    }
    int rc = b->open(cfg->media_bucket, cfg->media_region, &g_blob);
    if (rc != CB_OK) {
        snprintf(err, cap, "CBASE_MEDIA_BUCKET=%s: the %s backend would not open: %s", cfg->media_bucket, b->name, cb_strerror(rc));
        return CB_E_INVALID;
    }
    if (!g_blob.post_form) {
        cb_media_close();
        snprintf(err, cap, "CBASE_MEDIA_BUCKET is set and the %s backend cannot make an upload form", b->name);
        return CB_E_INVALID;
    }
    snprintf(g_prefix, sizeof g_prefix, "%s", cfg->media_prefix);
    g_on = true;
    CB_INFO(MOD, "uploads on: bucket %s, folder %s", cfg->media_bucket, g_prefix);
    return CB_OK;
}

int cb_media_post(const char *content_type, uint64_t max_bytes, int64_t now_ms, cb_media_form *out) {
    if (!g_on) return CB_E_STATE;
    return cb_media_post_with(&g_blob, g_prefix, content_type, max_bytes, now_ms, out);
}
