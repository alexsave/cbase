/* conf.c - cbase.conf, read and checked: the port of the shell tool's conf.sh, rule for rule and in
 * the same order, so the first error a file gets is the one the shell gives. The format, the keys
 * and the secret scan are conf.sh's header; the messages are conf.sh's words.
 */
#include "conf.h"

#include "cli.h"
#include "../../lib/num_parse.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

cbc_conf_t cbc_conf;

#define CONF_KEYS "CBASE_APP CBASE_VM CBASE_VM_STACK CBASE_VM_ORIGIN CBASE_VM_PORT CBASE_VM_DATA CBASE_VM_SETTINGS CBASE_VM_ARTIFACTS CBASE_VM_REPLACES CBASE_BACKUP_KEY CBASE_BACKUP_BUCKET CBASE_STATIC CBASE_STATIC_BUCKET CBASE_STATIC_PREFIX CBASE_MEMORY CBASE_ROUTE CBASE_DOMAIN CBASE_CERT_ARN CBASE_API_CERT_ARN CBASE_ALIASES CBASE_ZONE CBASE_API_HOST CBASE_API_ROUTE CBASE_STATIC_DIR CBASE_HASHED CBASE_SHELL"

/* The keys the file set, in the file's order: "KEY at line". conf_at reads the last of some keys. */
typedef struct {
  char *key;
  int line;
} seen_t;
static seen_t *seen;
static size_t seen_n, seen_cap;

/* ---- lists ------------------------------------------------------------------------------------ */

static void strs_push(cbc_strs *s, char *v) {
  if (s->n == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 8;
    char **nv = realloc(s->v, cap * sizeof *nv);
    if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    s->v = nv;
    s->cap = cap;
  }
  s->v[s->n++] = v;
}

static void routes_push(cbc_routes *r, cbc_route v) {
  if (r->n == r->cap) {
    size_t cap = r->cap ? r->cap * 2 : 8;
    cbc_route *nv = realloc(r->v, cap * sizeof *nv);
    if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    r->v = nv;
    r->cap = cap;
  }
  r->v[r->n++] = v;
}

/* split_ws: the words of S, as read -a splits them (runs of blanks and newlines separate). */
static void split_ws(const char *s, cbc_strs *out) {
  const char *p = s;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    const char *b = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    strs_push(out, cbc_dupn(b, (size_t)(p - b)));
  }
}

/* ---- errors ----------------------------------------------------------------------------------- */

/* die_line: the file's line and why, and the tool stops (conf_die). */
static void die_line(int no, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void die_line(int no, const char *fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  cbc_fail(CBC_EXIT_FAIL, "%s line %d: %s", cbc_conf.file, no, msg);
}

/* die_file: the file as a whole is wrong (conf_die_file). */
static void die_file(const char *what) __attribute__((noreturn));
static void die_file(const char *what) { cbc_fail(CBC_EXIT_FAIL, "%s %s", cbc_conf.file, what); }

/* ---- the keys the file set -------------------------------------------------------------------- */

static void seen_add(const char *key, int line) {
  if (seen_n == seen_cap) {
    size_t cap = seen_cap ? seen_cap * 2 : 32;
    seen_t *nv = realloc(seen, cap * sizeof *nv);
    if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    seen = nv;
    seen_cap = cap;
  }
  seen[seen_n].key = cbc_dup(key);
  seen[seen_n].line = line;
  seen_n++;
}

/* conf_at: the line the last of KEYS (a blank-separated list) is on, or 0 when none is set. */
static int conf_at(const char *keys) {
  int at = 0;
  for (size_t i = 0; i < seen_n; i++) {
    const char *k = seen[i].key;
    size_t kn = strlen(k);
    const char *p = keys;
    while (*p) {
      while (*p == ' ') p++;
      const char *b = p;
      while (*p && *p != ' ') p++;
      if ((size_t)(p - b) == kn && memcmp(b, k, kn) == 0) {
        at = seen[i].line;
        break;
      }
    }
  }
  return at;
}

static bool key_set(const char *keys) { return conf_at(keys) != 0; }

/* conf_refuses KEYS SENTENCE: one of KEYS set stops the tool at the last one's line. */
static void conf_refuses(const char *keys, const char *sentence) {
  int at = conf_at(keys);
  if (at) die_line(at, "%s", sentence);
}

/* conf_requires KEY OTHER SENTENCE: KEY set without OTHER stops the tool at KEY's line. */
static void conf_requires(const char *key, const char *other, const char *sentence) {
  int at = conf_at(key);
  if (at && !key_set(other)) die_line(at, "%s", sentence);
}

/* ---- the value rules -------------------------------------------------------------------------- */

bool cbc_conf_name_ok(const char *s) { return cbc_re("^[a-z]([a-z0-9-]{0,30}[a-z0-9])?$", s); }

/* host_ok: a lower-case hostname. The shell splits it on '.' with IFS=. and checks each label; a
 * final empty field is not one (bash drops it), an empty field in the middle is (and fails). */
static bool host_ok(const char *h) {
  if (strlen(h) > 253) return false;
  if (!cbc_re("^[a-z0-9.-]+$", h)) return false;
  if (!strchr(h, '.')) return false;
  const char *start = h;
  for (;;) {
    const char *dot = strchr(start, '.');
    const char *end = dot ? dot : start + strlen(start);
    if (dot == NULL && end == start) break;   /* a final empty field is dropped */
    char *label = cbc_dupn(start, (size_t)(end - start));
    bool ok = cbc_re("^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$", label);
    if (!ok) return false;
    if (!dot) break;
    start = dot + 1;
  }
  return true;
}

/* printable_key: a key spelled as a name (upper-case, digits and _), which can be printed. */
static bool printable_key(const char *k) { return cbc_re("^[A-Z][A-Z0-9_]{0,63}$", k); }

/* conf_methods: ALL, or the list's methods once each in a fixed order. */
static const char *conf_methods(int no, const char *list) {
  static const char *const known[] = { "GET", "HEAD", "OPTIONS", "PUT", "POST", "PATCH", "DELETE" };
  const char *known_text = "GET HEAD OPTIONS PUT POST PATCH DELETE";
  if (strcmp(list, "ALL") == 0) return "ALL";
  if (!cbc_re("^[A-Z]+(,[A-Z]+)*$", list))
    die_line(no, "CBASE_ROUTE's methods are ALL or a comma list of %s (upper case, no blanks)", known_text);
  char *copy = cbc_dup(list);
  for (char *m = strtok(copy, ","); m; m = strtok(NULL, ",")) {
    bool ok = false;
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) ok = ok || strcmp(m, known[i]) == 0;
    if (!ok) die_line(no, "%s is not a method CloudFront passes (%s)", m, known_text);
  }
  char out[128] = "";
  for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
    char with_commas[160];
    snprintf(with_commas, sizeof with_commas, ",%s,", known[i]);
    char padded[256];
    snprintf(padded, sizeof padded, ",%s,", list);
    if (strstr(padded, with_commas)) {
      if (out[0]) strcat(out, ",");
      strcat(out, known[i]);
    }
  }
  if (strcmp(out, "GET,HEAD,OPTIONS,PUT,POST,PATCH,DELETE") == 0) return "ALL";
  return cbc_dup(out);
}

/* conf_route: one CBASE_ROUTE (or CBASE_API_ROUTE, when api), normalized and appended. */
static void conf_route(int no, const char *value, bool api) {
  const char *key = api ? "CBASE_API_ROUTE" : "CBASE_ROUTE";
  cbc_strs rest = { 0 };
  split_ws(value, &rest);
  const char *pattern = rest.n > 0 ? rest.v[0] : "";
  const char *origin = rest.n > 1 ? rest.v[1] : "";
  if (!*origin)
    die_line(no, "%s is \"<pattern> <origin> [cache|nocache] [<methods>]\", and this one has no origin (vm, static or ws)", key);
  if (strcmp(pattern, "*") != 0 && !cbc_re("^/[A-Za-z0-9_.~*/-]{0,254}$", pattern))
    die_line(no, "%s's pattern is \"*\" or a path starting with '/' of letters, digits and _ . ~ * / - (CloudFront's path patterns)", key);
  if (api) {
    if (strcmp(pattern, "*") == 0) die_line(no, "CBASE_API_ROUTE's pattern is a path: the API host's default is the VM already");
    if (strcmp(origin, "vm") != 0 && strcmp(origin, "ws") != 0)
      die_line(no, "CBASE_API_ROUTE's origin is vm or ws (the API host has no static files)");
  }
  if (strcmp(origin, "vm") == 0 || strcmp(origin, "static") == 0) {
    /* a vm or static route takes any of the words below */
  } else if (strcmp(origin, "ws") == 0) {
    if (rest.n != 2)
      die_line(no, "a ws route takes no cache or methods: it is the VM, uncached, every method, with long timeouts");
  } else {
    die_line(no, "%s's origin is vm, static or ws", key);
  }
  const char *cache = "";
  const char *methods = "";
  for (size_t i = 2; i < rest.n; i++) {
    const char *f = rest.v[i];
    if (strcmp(f, "cache") == 0 || strcmp(f, "nocache") == 0 || strcmp(f, "origin") == 0) {
      if (*cache) die_line(no, "%s names its cache policy twice", key);
      if (strcmp(f, "origin") == 0 && strcmp(origin, "vm") != 0)
        die_line(no, "the cache mode origin (the origin's Cache-Control) is for a vm route");
      cache = f;
    } else {
      if (*methods) die_line(no, "%s names its methods twice (one comma list, no blanks)", key);
      methods = conf_methods(no, f);
    }
  }
  if (strcmp(origin, "static") == 0) {
    if (!*cache) cache = "cache";
    if (!*methods) methods = "GET,HEAD";
    static const char *const allowed[] = { "GET", "HEAD", "OPTIONS", "GET,HEAD", "GET,OPTIONS", "HEAD,OPTIONS", "GET,HEAD,OPTIONS" };
    bool ok = false;
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) ok = ok || strcmp(methods, allowed[i]) == 0;
    if (!ok) die_line(no, "a static route takes only GET, HEAD and OPTIONS (a bucket answers nothing else)");
  } else if (strcmp(origin, "ws") == 0) {
    cache = "nocache";
    methods = "ALL";
  } else {
    if (!*cache) cache = "nocache";
    if (!*methods) methods = "ALL";
  }
  cbc_routes *taken = api ? &cbc_conf.api_routes : &cbc_conf.routes;
  for (size_t i = 0; i < taken->n; i++) {
    if (strcmp(taken->v[i].pattern, pattern) == 0) die_line(no, "%s %s is routed twice", key, pattern);
  }
  cbc_route r = { cbc_dup(pattern), cbc_dup(origin), cbc_dup(cache), cbc_dup(methods), no };
  routes_push(taken, r);
}

/* conf_secret_scan: stops the tool when LINE looks like it holds a secret. The key's name, a PEM
 * marker, an AWS access key id, or a long run of random-looking characters: the same tests the shell
 * makes, in the same order, and the line is never printed. */
static bool has_upper(const char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') return true; return false; }
static bool has_lower(const char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') return true; return false; }
static bool has_digit(const char *s) { for (; *s; s++) if (*s >= '0' && *s <= '9') return true; return false; }

static bool is_run_char(char c, int which) {
  if (which == 0) return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
  if (which == 1) return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* longest_run: the first maximal run of WHICH characters at least MIN long that PRED accepts, or NULL. */
static bool run_matches(const char *text, int which, size_t min, int need_mixed) {
  const char *p = text;
  while (*p) {
    if (!is_run_char(*p, which)) { p++; continue; }
    const char *b = p;
    while (is_run_char(*p, which)) p++;
    size_t n = (size_t)(p - b);
    if (n < min) continue;
    if (!need_mixed) return true;
    char *run = cbc_dupn(b, n);
    if (has_upper(run) && has_lower(run) && has_digit(run)) return true;
  }
  return false;
}

static void secret_scan(int no, const char *text) {
  char key[4096] = "";
  const char *eq = strchr(text, '=');
  if (eq) {
    size_t n = (size_t)(eq - text);
    if (n >= sizeof key) n = sizeof key - 1;
    memcpy(key, text, n);
    key[n] = '\0';
    cbc_trim(key);
  }
  char upper[4096];
  size_t kn = strlen(key);
  for (size_t i = 0; i <= kn; i++) upper[i] = (key[i] >= 'a' && key[i] <= 'z') ? (char)(key[i] - 32) : key[i];
  if (strstr(upper, "SECRET") || strstr(upper, "TOKEN") || strstr(upper, "PASSWORD") || strstr(upper, "PASSWD") ||
      strstr(upper, "PRIVATE") || strstr(upper, "CREDENTIAL")) {
    if (printable_key(key))
      die_line(no, "%s is named like a secret. cbase.conf is committed and public, so a secret never goes in it: keep it in the app's environment on the VM (or SSM), and out of the repository", key);
    die_line(no, "the key is named like a secret. cbase.conf is committed and public, so a secret never goes in it");
  }
  const char *what = NULL;
  if (strstr(text, "-----BEGIN") || strstr(text, "-----END")) {
    what = "a PEM block (a key or a certificate's private part)";
  } else if (cbc_re("(AKIA|ASIA)[A-Z0-9]{16}", text)) {
    what = "an AWS access key id";
  } else if (run_matches(text, 0, 24, 1)) {
    what = "a long random-looking string (an access key, a token)";
  } else if (run_matches(text, 1, 32, 0)) {
    what = "a long hex string (a token, a key)";
  } else if (run_matches(text, 2, 40, 0)) {
    what = "a long random-looking string (an access key, a token)";
  }
  if (!what) return;
  if (printable_key(key))
    die_line(no, "the value of %s looks like a secret: %s. cbase.conf is committed and public, so a secret never goes in it", key, what);
  die_line(no, "this line looks like a secret: %s. cbase.conf is committed and public, so a secret never goes in it", what);
}

/* read_file: FILE's bytes, whole. Returns false when it cannot be opened or read. */
static bool read_file(const char *file, char **data, size_t *len) {
  FILE *f = fopen(file, "rb");
  if (!f) return false;
  size_t cap = 65536, n = 0;
  char *buf = malloc(cap);
  if (!buf) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  size_t r;
  while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
    n += r;
    if (n == cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) cbc_fail(CBC_EXIT_FAIL, "out of memory");
      buf = nb;
    }
  }
  bool ok = !ferror(f);
  fclose(f);
  *data = buf;
  *len = n;
  return ok;
}

/* ---- the load --------------------------------------------------------------------------------- */

void cbc_conf_load(const char *file) {
  memset(&cbc_conf, 0, sizeof cbc_conf);
  cbc_conf.file = cbc_dup(file);
  /* Every string an unset setting reads as is "", as the shell's CONF_ variables are. */
  char **unset_strings[] = { &cbc_conf.app, &cbc_conf.vm, &cbc_conf.stat, &cbc_conf.vm_stack, &cbc_conf.vm_origin,
                             &cbc_conf.vm_port, &cbc_conf.vm_data, &cbc_conf.vm_settings, &cbc_conf.vm_artifacts,
                             &cbc_conf.vm_replaces, &cbc_conf.backup_key, &cbc_conf.backup_bucket, &cbc_conf.static_bucket,
                             &cbc_conf.static_prefix, &cbc_conf.memory, &cbc_conf.domain, &cbc_conf.cert_arn,
                             &cbc_conf.api_cert_arn, &cbc_conf.zone, &cbc_conf.api_host, &cbc_conf.static_dir, &cbc_conf.shell };
  for (size_t i = 0; i < sizeof unset_strings / sizeof unset_strings[0]; i++) *unset_strings[i] = "";
  cbc_conf.aliases = "on";
  struct stat st;
  if (stat(file, &st) != 0 || !S_ISREG(st.st_mode))
    die_file("does not exist (an app's settings are cbase.conf at its repository's root, or -f FILE)");
  if (access(file, R_OK) != 0) die_file("cannot be read");
  char *data = NULL;
  size_t dlen = 0;
  if (!read_file(file, &data, &dlen)) die_file("cannot be read");
  if (memchr(data, '\0', dlen)) die_file("holds a NUL byte");

  /* The lines: split on '\n'; a last piece with no newline counts if it is not empty; one '\r' at
   * the end of each line is not part of it. */
  cbc_strs lines = { 0 };
  size_t start = 0;
  for (size_t i = 0; i <= dlen; i++) {
    if (i < dlen && data[i] != '\n') continue;
    if (i == start && i == dlen) break;
    size_t end = i;
    if (end > start && data[end - 1] == '\r') end--;
    strs_push(&lines, cbc_dupn(data + start, end - start));
    start = i + 1;
  }

  /* Every line is held to the secret scan first, before anything is parsed or printed. */
  for (size_t i = 0; i < lines.n; i++) secret_scan((int)i + 1, lines.v[i]);

  char *vm_set = "", *static_set = "";
  bool memory_set = false;
  char *dir_set = "";
  long long memory = 0;

  for (size_t i = 0; i < lines.n; i++) {
    int no = (int)i + 1;
    char *line = lines.v[i];
    /* a blank or comment line is skipped */
    const char *first = line;
    while (*first == ' ' || *first == '\t') first++;
    if (!*first || *first == '#') continue;
    const char *eq = strchr(line, '=');
    if (!eq) die_line(no, "is not KEY = value (no '=')");
    char *key = cbc_dupn(line, (size_t)(eq - line));
    char *value = cbc_dup(eq + 1);
    cbc_trim(key);
    cbc_trim(value);
    if (!cbc_re("^[A-Z][A-Z0-9_]*$", key))
      die_line(no, "is not KEY = value (a key is upper-case letters, digits and _, and starts the line)");
    {
      /* [[ " $CONF_KEYS " == *" $key "* ]] */
      char padded[1024];
      snprintf(padded, sizeof padded, " %s ", CONF_KEYS);
      char want[160];
      snprintf(want, sizeof want, " %s ", key);
      if (!strstr(padded, want)) die_line(no, "%s is not a cbase.conf key (the keys: %s)", key, CONF_KEYS);
    }
    if (strchr(value, '#')) die_line(no, "has a '#' in its value: a comment goes on a line of its own");
    if (strchr(value, '"') || strchr(value, '\''))
      die_line(no, "has a quote in its value: values are never quoted (everything after the '=' is the value)");
    if (!*value) die_line(no, "%s has no value (leave the line out for the default)", key);
    bool is_route = strcmp(key, "CBASE_ROUTE") == 0 || strcmp(key, "CBASE_API_ROUTE") == 0;
    if (!is_route) {
      int at = conf_at(key);
      if (at) die_line(no, "sets %s again (line %d set it)", key, at);
    }
    seen_add(key, no);

    if (strcmp(key, "CBASE_APP") == 0) {
      if (!cbc_conf_name_ok(value) || strcmp(value, "none") == 0)
        die_line(no, "CBASE_APP is a name: a lower-case letter, then lower-case letters, digits and '-', at most 32 characters, not ending in '-' (and not none)");
      cbc_conf.app = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM") == 0 || strcmp(key, "CBASE_STATIC") == 0) {
      if (!(strcmp(value, "none") == 0 || cbc_conf_name_ok(value)))
        die_line(no, "%s is a group name (a lower-case letter, then lower-case letters, digits and '-', at most 32 characters) or none", key);
      if (strcmp(key, "CBASE_VM") == 0) vm_set = cbc_dup(value);
      else static_set = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM_STACK") == 0) {
      if (!cbc_re("^[A-Za-z][A-Za-z0-9-]{0,127}$", value))
        die_line(no, "CBASE_VM_STACK is a CloudFormation stack's name (a letter, then letters, digits and -)");
      cbc_conf.vm_stack = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM_ORIGIN") == 0) {
      if (!host_ok(value)) die_line(no, "CBASE_VM_ORIGIN is a lower-case hostname (vm-origin.example.com)");
      cbc_conf.vm_origin = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM_PORT") == 0) {
      int64_t port = 0;
      if (!cbc_re("^[0-9]{4,5}$", value) || !num_parse_i64(value, strlen(value), 0, 99999, &port) || port < 1024 || port > 65535)
        die_line(no, "CBASE_VM_PORT is a port, 1024 to 65535");
      if (port >= 8700 && port <= 8799)
        die_line(no, "CBASE_VM_PORT is a grandfathered member's own port, outside the 8700-8799 cbase claims");
      char num[32];
      snprintf(num, sizeof num, "%lld", (long long)port);
      cbc_conf.vm_port = cbc_dup(num);
    } else if (strcmp(key, "CBASE_VM_DATA") == 0) {
      char slashed[4200];
      snprintf(slashed, sizeof slashed, "%s/", value);
      bool shape = cbc_re("^(/[A-Za-z0-9_][A-Za-z0-9._-]*)+$", value);
      if (!shape || strstr(slashed, "/../") || strncmp(slashed, "/srv/cbase/", 11) == 0)
        die_line(no, "CBASE_VM_DATA is an absolute directory on the VM (/var/lib/app), not under /srv/cbase/");
      cbc_conf.vm_data = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM_REPLACES") == 0) {
      if (!cbc_re("^[A-Za-z0-9_.@-]+\\.service$", value))
        die_line(no, "CBASE_VM_REPLACES is a systemd unit's name (name.service)");
      cbc_conf.vm_replaces = cbc_dup(value);
    } else if (strcmp(key, "CBASE_BACKUP_KEY") == 0) {
      if (!cbc_re("^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*$", value))
        die_line(no, "CBASE_BACKUP_KEY is an object key (cbase/latest.sqlite): letters, digits and . _ - /, no leading /");
      cbc_conf.backup_key = cbc_dup(value);
    } else if (strcmp(key, "CBASE_STATIC_BUCKET") == 0) {
      if (!cbc_re("^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$", value))
        die_line(no, "CBASE_STATIC_BUCKET is an S3 bucket's name");
      cbc_conf.static_bucket = cbc_dup(value);
    } else if (strcmp(key, "CBASE_STATIC_PREFIX") == 0) {
      if (!cbc_re("^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*$", value))
        die_line(no, "CBASE_STATIC_PREFIX is a folder in the bucket (site, sites/one): letters, digits and . _ - /, no leading or trailing /");
      cbc_conf.static_prefix = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM_SETTINGS") == 0) {
      if (!cbc_re("^(/[A-Za-z0-9_.-]+)+/$", value))
        die_line(no, "CBASE_VM_SETTINGS is an SSM path, starting and ending with / (/app/env/)");
      cbc_conf.vm_settings = cbc_dup(value);
    } else if (strcmp(key, "CBASE_VM_ARTIFACTS") == 0) {
      if (!cbc_re("^s3://[a-z0-9][a-z0-9.-]{1,61}[a-z0-9](/[A-Za-z0-9_.-]+)*/$", value))
        die_line(no, "CBASE_VM_ARTIFACTS is s3://<bucket>/<prefix>/, ending with /");
      cbc_conf.vm_artifacts = cbc_dup(value);
    } else if (strcmp(key, "CBASE_BACKUP_BUCKET") == 0) {
      if (!cbc_re("^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$", value))
        die_line(no, "CBASE_BACKUP_BUCKET is an S3 bucket's name");
      cbc_conf.backup_bucket = cbc_dup(value);
    } else if (strcmp(key, "CBASE_MEMORY") == 0) {
      if (!cbc_re("^[0-9]{1,6}[MG]$", value))
        die_line(no, "CBASE_MEMORY is <n>M or <n>G (256M, 1G)");
      size_t dn = strlen(value) - 1;
      int64_t n = 0;
      num_parse_i64(value, dn, 0, 999999, &n);
      memory = n;
      if (value[dn] == 'G') memory *= 1024;
      if (memory < 16 || memory > 65536) die_line(no, "CBASE_MEMORY is from 16M to 64G");
      memory_set = true;
    } else if (strcmp(key, "CBASE_ROUTE") == 0) {
      conf_route(no, value, false);
    } else if (strcmp(key, "CBASE_API_ROUTE") == 0) {
      conf_route(no, value, true);
    } else if (strcmp(key, "CBASE_DOMAIN") == 0) {
      cbc_strs names = { 0 };
      split_ws(value, &names);
      for (size_t g = 0; g < names.n; g++) {
        if (!host_ok(names.v[g]))
          die_line(no, "CBASE_DOMAIN is lower-case hostnames, blank-separated (example.com www.example.com)");
        for (size_t k = 0; k < g; k++) {
          if (strcmp(names.v[k], names.v[g]) == 0) die_line(no, "CBASE_DOMAIN names %s twice", names.v[g]);
        }
      }
      cbc_conf.domains = names;
      cbc_conf.domain = names.v[0];
    } else if (strcmp(key, "CBASE_API_HOST") == 0) {
      if (!host_ok(value)) die_line(no, "CBASE_API_HOST is one lower-case hostname (api.example.com)");
      cbc_conf.api_host = cbc_dup(value);
    } else if (strcmp(key, "CBASE_CERT_ARN") == 0 || strcmp(key, "CBASE_API_CERT_ARN") == 0) {
      static const char *const acm = "^arn:aws:acm:([a-z0-9-]+):[0-9]{12}:certificate/"
                                     "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$";
      if (!cbc_re(acm, value))
        die_line(no, "%s is an ACM certificate's ARN: arn:aws:acm:us-east-1:<account>:certificate/<uuid>", key);
      /* the region is the second colon-field after "arn:aws:acm:" */
      const char *region = value + strlen("arn:aws:acm:");
      size_t rn = (size_t)(strchr(region, ':') - region);
      if (!(rn == 9 && strncmp(region, "us-east-1", 9) == 0))
        die_line(no, "%s is in %.*s, and CloudFront takes a certificate from us-east-1 alone", key, (int)rn, region);
      if (strcmp(key, "CBASE_CERT_ARN") == 0) cbc_conf.cert_arn = cbc_dup(value);
      else cbc_conf.api_cert_arn = cbc_dup(value);
    } else if (strcmp(key, "CBASE_ALIASES") == 0) {
      if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) die_line(no, "CBASE_ALIASES is on or off");
      cbc_conf.aliases = cbc_dup(value);
    } else if (strcmp(key, "CBASE_ZONE") == 0) {
      if (!cbc_re("^Z[A-Z0-9]{1,31}$", value))
        die_line(no, "CBASE_ZONE is a Route 53 hosted zone id (Z and upper-case letters and digits)");
      cbc_conf.zone = cbc_dup(value);
    } else if (strcmp(key, "CBASE_STATIC_DIR") == 0) {
      size_t vn = strlen(value);
      if (vn > 0 && value[vn - 1] == '/') value[vn - 1] = '\0';
      char slashed[4200];
      snprintf(slashed, sizeof slashed, "/%s/", value);
      if (!cbc_re("^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*$", value) || strstr(slashed, "/../"))
        die_line(no, "CBASE_STATIC_DIR is a directory inside the repository, relative to its root (build, ui/out): letters, digits and . _ - /, no leading '/', no '..'");
      dir_set = cbc_dup(value);
    } else if (strcmp(key, "CBASE_HASHED") == 0) {
      cbc_strs globs = { 0 };
      split_ws(value, &globs);
      for (size_t g = 0; g < globs.n; g++) {
        if (!cbc_re("^[A-Za-z0-9._/*?-]+$", globs.v[g]) || globs.v[g][0] == '/')
          die_line(no, "CBASE_HASHED is blank-separated globs of letters, digits and . _ - / * ? (assets/*), each a path in the static dir with no leading '/'");
      }
      cbc_conf.hashed = globs;
    } else if (strcmp(key, "CBASE_SHELL") == 0) {
      if (!cbc_re("^/[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*\\.html$", value))
        die_line(no, "CBASE_SHELL is a page of the build, an absolute path ending in .html (/_shell.html): letters, digits and . _ - /, each part starting with a letter, a digit, _ or -");
      cbc_conf.shell = cbc_dup(value);
    }
  }

  if (!conf_at("CBASE_APP")) die_file("has no CBASE_APP line (the app's name: CBASE_APP = myapp)");
  cbc_conf.vm = *vm_set ? vm_set : cbc_conf.app;
  cbc_conf.stat = *static_set ? static_set : cbc_conf.app;
  if (strcmp(cbc_conf.vm, "none") == 0 && strcmp(cbc_conf.stat, "none") == 0)
    die_line(conf_at("CBASE_VM CBASE_STATIC"), "CBASE_VM and CBASE_STATIC are both none: an app has a VM, static files, or both");
  const char *vmkeys = "CBASE_VM_STACK CBASE_VM_ORIGIN CBASE_VM_PORT CBASE_VM_DATA CBASE_VM_REPLACES CBASE_BACKUP_KEY CBASE_VM_SETTINGS CBASE_VM_ARTIFACTS CBASE_BACKUP_BUCKET";
  if ((*cbc_conf.vm_port || *cbc_conf.vm_data) && (!*cbc_conf.vm_port || !*cbc_conf.vm_data))
    die_line(conf_at(vmkeys), "CBASE_VM_PORT and CBASE_VM_DATA go together: a grandfathered member's port and its data directory");
  if (*cbc_conf.vm_replaces && !*cbc_conf.vm_port)
    die_line(conf_at(vmkeys), "CBASE_VM_REPLACES is for a grandfathered member (CBASE_VM_PORT and CBASE_VM_DATA): the unit that served it before");
  if (strcmp(cbc_conf.vm, "none") == 0) {
    conf_refuses(vmkeys, "the CBASE_VM_* and CBASE_BACKUP_BUCKET keys are for an app with a VM, and CBASE_VM is none");
    conf_refuses("CBASE_MEMORY", "CBASE_MEMORY is for an app with a VM, and CBASE_VM is none");
    conf_refuses("CBASE_API_HOST", "CBASE_API_HOST sends every request to the VM, and CBASE_VM is none");
  } else {
    if (memory_set) {
      char num[32];
      snprintf(num, sizeof num, "%lld", memory);
      cbc_conf.memory = cbc_dup(num);
    } else {
      cbc_conf.memory = "";
    }
    cbc_conf.memory_estimated = !conf_at("CBASE_MEMORY");
  }
  for (size_t i = 0; i < cbc_conf.routes.n; i++) {
    cbc_route *r = &cbc_conf.routes.v[i];
    if (strcmp(r->origin, "static") == 0 && strcmp(cbc_conf.stat, "none") == 0)
      die_line(r->line, "routes to static, and CBASE_STATIC is none");
    if (strcmp(r->origin, "static") != 0 && strcmp(cbc_conf.vm, "none") == 0)
      die_line(r->line, "routes to the VM (%s), and CBASE_VM is none", r->origin);
  }
  conf_requires("CBASE_API_HOST", "CBASE_DOMAIN", "CBASE_API_HOST is a second hostname, and there is no CBASE_DOMAIN (the first)");
  conf_requires("CBASE_API_ROUTE", "CBASE_API_HOST", "CBASE_API_ROUTE is a route of CBASE_API_HOST's, and there is none");
  if (*cbc_conf.api_host) {
    for (size_t i = 0; i < cbc_conf.domains.n; i++) {
      if (strcmp(cbc_conf.domains.v[i], cbc_conf.api_host) == 0)
        die_line(conf_at("CBASE_API_HOST"), "CBASE_API_HOST %s is one of CBASE_DOMAIN's hostnames: a hostname is on one distribution", cbc_conf.domains.v[i]);
    }
  }
  conf_requires("CBASE_CERT_ARN", "CBASE_DOMAIN", "CBASE_CERT_ARN is for CBASE_DOMAIN, and there is none (without a domain the app is at its cloudfront.net address)");
  conf_requires("CBASE_API_CERT_ARN", "CBASE_API_HOST", "CBASE_API_CERT_ARN is for CBASE_API_HOST, and there is none");
  conf_requires("CBASE_ALIASES", "CBASE_DOMAIN", "CBASE_ALIASES is for CBASE_DOMAIN's hostnames, and there is none");
  conf_requires("CBASE_ZONE", "CBASE_DOMAIN", "CBASE_ZONE is where CBASE_DOMAIN's record goes, and there is no CBASE_DOMAIN");
  if (strcmp(cbc_conf.stat, "none") == 0) {
    conf_refuses("CBASE_STATIC_DIR", "CBASE_STATIC_DIR is the static files' directory, and CBASE_STATIC is none");
    conf_refuses("CBASE_HASHED", "CBASE_HASHED names static files, and CBASE_STATIC is none");
    conf_refuses("CBASE_SHELL", "CBASE_SHELL is a page of the static files, and CBASE_STATIC is none");
    conf_refuses("CBASE_STATIC_BUCKET CBASE_STATIC_PREFIX", "CBASE_STATIC_BUCKET and CBASE_STATIC_PREFIX are for static files, and CBASE_STATIC is none");
    cbc_conf.static_dir = "";
    cbc_conf.static_dir_default = false;
  } else if (*dir_set) {
    cbc_conf.static_dir = dir_set;
    cbc_conf.static_dir_default = false;
  } else {
    cbc_conf.static_dir = strcmp(cbc_conf.vm, "none") == 0 ? "build" : "ui/out";
    cbc_conf.static_dir_default = true;
  }
  /* No route written: the defaults for the app's shape. */
  if (cbc_conf.routes.n == 0 && strcmp(cbc_conf.vm, "none") != 0 && strcmp(cbc_conf.stat, "none") != 0) {
    cbc_route r = { cbc_dup("/api/*"), cbc_dup("vm"), cbc_dup("nocache"), cbc_dup("ALL"), 0 };
    routes_push(&cbc_conf.routes, r);
  }
  bool has_default = false;
  for (size_t i = 0; i < cbc_conf.routes.n; i++) has_default = has_default || strcmp(cbc_conf.routes.v[i].pattern, "*") == 0;
  if (!has_default) {
    cbc_route r = strcmp(cbc_conf.stat, "none") != 0
                    ? (cbc_route){ cbc_dup("*"), cbc_dup("static"), cbc_dup("cache"), cbc_dup("GET,HEAD"), 0 }
                    : (cbc_route){ cbc_dup("*"), cbc_dup("vm"), cbc_dup("nocache"), cbc_dup("ALL"), 0 };
    routes_push(&cbc_conf.routes, r);
  } else {
    /* CloudFront's default behaviour goes last, wherever it was written. */
    cbc_routes ordered = { 0 };
    cbc_route def = { 0 };
    for (size_t i = 0; i < cbc_conf.routes.n; i++) {
      if (strcmp(cbc_conf.routes.v[i].pattern, "*") == 0) def = cbc_conf.routes.v[i];
      else routes_push(&ordered, cbc_conf.routes.v[i]);
    }
    routes_push(&ordered, def);
    cbc_conf.routes = ordered;
  }
}

/* ---- the printed form ------------------------------------------------------------------------- */

static void row(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void row(const char *label, const char *fmt, ...) {
  char val[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(val, sizeof val, fmt, ap);
  va_end(ap);
  printf("%-10s %s\n", label, val);
}

static void join_words(const cbc_strs *s, char *out, size_t n) {
  out[0] = '\0';
  for (size_t i = 0; i < s->n; i++) {
    if (i) strncat(out, " ", n - strlen(out) - 1);
    strncat(out, s->v[i], n - strlen(out) - 1);
  }
}

void cbc_conf_print(void) {
  const cbc_conf_t *c = &cbc_conf;
  row("app", "%s", c->app);
  if (strcmp(c->vm, "none") == 0) row("vm", "none");
  else if (c->memory_estimated) row("vm", "%s (%s: memory not declared, counted as %dM)", c->vm, c->app, CBC_MEMORY_ESTIMATE);
  else row("vm", "%s (memory %s MiB)", c->vm, c->memory);
  if (*c->vm_stack) row("vm-stack", "%s (adopted: found by its stack, no group-init)", c->vm_stack);
  if (*c->vm_origin) row("vm-origin", "%s (IPv6)", c->vm_origin);
  if (*c->vm_port) row("legacy", "port %s, data %s (grandfathered: kept as it is)", c->vm_port, c->vm_data);
  if (*c->vm_settings) row("settings", "SSM %s", c->vm_settings);
  if (*c->vm_artifacts) row("artifacts", "%s", c->vm_artifacts);
  if (*c->vm_replaces) row("replaces", "%s (stopped and disabled by the first deploy)", c->vm_replaces);
  if (*c->backup_bucket || *c->backup_key) {
    char default_key[256];
    snprintf(default_key, sizeof default_key, "backups/%s/cbase.sqlite", c->app);
    row("backups", "s3://%s/%s", *c->backup_bucket ? c->backup_bucket : "(the state bucket)",
        *c->backup_key ? c->backup_key : default_key);
  }
  if (*c->static_bucket) row("static", "s3://%s/%s/ (adopted bucket)", c->static_bucket, *c->static_prefix ? c->static_prefix : c->app);
  else row("static", "%s", c->stat);
  if (!*c->static_bucket && *c->static_prefix) row("folder", "%s/", c->static_prefix);
  if (*c->static_dir) {
    if (c->static_dir_default) row("files", "%s/ (the default)", c->static_dir);
    else row("files", "%s/", c->static_dir);
  }
  if (c->hashed.n) {
    char words[4096];
    join_words(&c->hashed, words, sizeof words);
    row("hashed", "%s", words);
  }
  if (*c->shell) row("shell", "%s", c->shell);
  if (*c->domain) {
    char words[4096];
    join_words(&c->domains, words, sizeof words);
    row("domain", "%s", words);
  }
  if (*c->api_host) row("api-host", "%s", c->api_host);
  if (*c->cert_arn) row("cert", "%s", c->cert_arn);
  else if (*c->domain) row("cert", "(found or requested by deploy)");
  else row("address", "(the distribution's cloudfront.net name)");
  if (*c->api_cert_arn) row("api-cert", "%s", c->api_cert_arn);
  if (*c->zone) row("zone", "%s", c->zone);
  if (strcmp(c->aliases, "on") != 0) row("aliases", "off (the distributions carry no hostname: a staged attach)");
  for (size_t i = 0; i < c->routes.n; i++) {
    const cbc_route *r = &c->routes.v[i];
    row("route", "%s %s %s %s", r->pattern, r->origin, r->cache, r->methods);
  }
  for (size_t i = 0; i < c->api_routes.n; i++) {
    const cbc_route *r = &c->api_routes.v[i];
    row("api-route", "%s %s %s %s", r->pattern, r->origin, r->cache, r->methods);
  }
}

/* cbc_conf_mem (conf.sh's conf_mem): the app's mem field in a state file, the declared MiB or "~128"
 * when it is the estimate; "" for an app with no VM. */
const char *cbc_conf_mem(void) {
  static char buf[32];
  if (cbc_conf.memory_estimated) snprintf(buf, sizeof buf, "~%d", CBC_MEMORY_ESTIMATE);
  else snprintf(buf, sizeof buf, "%s", cbc_conf.memory ? cbc_conf.memory : "");
  return buf;
}
