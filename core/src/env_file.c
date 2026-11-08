#include "env_file.h"

#include "util/err.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;   // NUL-terminated in the file's buffer
    char *val;
    int   line;
} env_line;

static bool key_ok(const char *k, size_t n) {
    if (n == 0 || !((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= 'a' && k[0] <= 'z') || k[0] == '_')) return false;
    for (size_t i = 1; i < n; i++)
        if (!((k[i] >= 'A' && k[i] <= 'Z') || (k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_'))
            return false;
    return true;
}

static bool blank(char c) { return c == ' ' || c == '\t'; }

// One line (s, n bytes, no '\n'), number `no`: a setting into *out (true), or skipped (false, err
// empty), or refused (false, a sentence in err). The key and value are cut in place.
static bool parse_line(const char *path, int no, char *s, size_t n, env_line *out, char *err, size_t cap) {
    err[0] = 0;
    if (n && s[n - 1] == '\r') n--;
    size_t b = 0;
    while (b < n && blank(s[b])) b++;
    if (b == n || s[0] == '#') return false;
    char *eq = memchr(s, '=', n);
    if (!eq) {
        snprintf(err, cap, "CBASE_ENV_FILE %s line %d is not KEY=VALUE (no '=')", path, no);
        return false;
    }
    size_t kn = (size_t)(eq - s);
    if (kn && blank(s[kn - 1])) {
        snprintf(err, cap, "CBASE_ENV_FILE %s line %d has a blank before its '=' (none is allowed around it)", path, no);
        return false;
    }
    if (!key_ok(s, kn)) {
        snprintf(err, cap, "CBASE_ENV_FILE %s line %d: \"%.*s\" is not a key (a letter or '_', then letters, digits and '_')",
                 path, no, (int)(kn > 64 ? 64 : kn), s);
        return false;
    }
    char *v = eq + 1;
    size_t vn = n - kn - 1;
    if (vn && blank(v[0])) {
        snprintf(err, cap, "CBASE_ENV_FILE %s line %d has a blank after its '=' (none is allowed around it)", path, no);
        return false;
    }
    if (vn && (v[0] == '"' || v[0] == '\'')) {
        if (vn < 2 || v[vn - 1] != v[0]) {
            snprintf(err, cap, "CBASE_ENV_FILE %s line %d opens a %s quote its value does not end with", path, no,
                     v[0] == '"' ? "double" : "single");
            return false;
        }
        v++;
        vn -= 2;
    } else {
        // An inline comment, as Node's loadEnvFile reads one: a '#' after a blank ends an unquoted
        // value, and the blanks before it go too. A '#' with no blank before it is the value's own.
        size_t c = 1;
        while (c < vn && !(v[c] == '#' && blank(v[c - 1]))) c++;
        if (c < vn) {
            vn = c;
            while (vn && blank(v[vn - 1])) vn--;
        } else if (vn && blank(v[vn - 1])) {
            snprintf(err, cap, "CBASE_ENV_FILE %s line %d's value ends in a blank (quote the value if it is meant)", path, no);
            return false;
        }
    }
    s[kn] = 0;
    v[vn] = 0;
    *out = (env_line){ s, v, no };
    return true;
}

int cb_env_file_load(const char *path, char *err, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, cap, "CBASE_ENV_FILE %s cannot be read: %s", path, strerror(errno));
        return CB_E_INVALID;
    }
    char *buf = malloc(CB_ENV_FILE_MAX + 1);
    size_t len = buf ? fread(buf, 1, CB_ENV_FILE_MAX + 1, f) : 0;
    bool bad_read = buf && ferror(f);
    fclose(f);
    if (!buf) {
        snprintf(err, cap, "CBASE_ENV_FILE %s: out of memory", path);
        return CB_E_INVALID;
    }
    if (bad_read) {
        snprintf(err, cap, "CBASE_ENV_FILE %s cannot be read", path);
        free(buf);
        return CB_E_INVALID;
    }
    if (len > CB_ENV_FILE_MAX) {
        snprintf(err, cap, "CBASE_ENV_FILE %s is larger than %d bytes", path, CB_ENV_FILE_MAX);
        free(buf);
        return CB_E_INVALID;
    }
    buf[len] = 0;
    // Every line is read and checked before anything is set: a refused file sets nothing.
    size_t n_lines = 1;
    for (size_t i = 0; i < len; i++) n_lines += buf[i] == '\n';
    env_line *set = calloc(n_lines, sizeof *set);
    if (!set) {
        snprintf(err, cap, "CBASE_ENV_FILE %s: out of memory", path);
        free(buf);
        return CB_E_INVALID;
    }
    int rc = CB_OK;
    size_t n_set = 0;
    int no = 0;
    for (char *s = buf; rc == CB_OK && s <= buf + len; ) {
        no++;
        char *e = memchr(s, '\n', (size_t)(buf + len - s));
        size_t n = e ? (size_t)(e - s) : (size_t)(buf + len - s);
        if (memchr(s, 0, n)) {
            snprintf(err, cap, "CBASE_ENV_FILE %s line %d holds a NUL byte", path, no);
            rc = CB_E_INVALID;
            break;
        }
        env_line l;
        if (parse_line(path, no, s, n, &l, err, cap)) {
            for (size_t i = 0; i < n_set; i++)
                if (strcmp(set[i].key, l.key) == 0) {
                    snprintf(err, cap, "CBASE_ENV_FILE %s line %d sets %s again (line %d did)", path, no, l.key, set[i].line);
                    rc = CB_E_INVALID;
                }
            set[n_set++] = l;
        } else if (err[0]) {
            rc = CB_E_INVALID;
        }
        if (!e) break;
        s = e + 1;
    }
    for (size_t i = 0; rc == CB_OK && i < n_set; i++)
        if (!getenv(set[i].key) && setenv(set[i].key, set[i].val, 0) != 0) {
            snprintf(err, cap, "CBASE_ENV_FILE %s line %d: %s cannot be set: %s", path, set[i].line, set[i].key, strerror(errno));
            rc = CB_E_INVALID;
        }
    free(set);
    free(buf);
    return rc;
}
