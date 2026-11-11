/* vm.c - the VM side of a deploy and an undeploy (tools/cbase.d/vm.sh, the header there has the
 * whole design: the port is the origin, the socket holds the port across a restart, the secrets
 * never pass this tool). This file is the part that needs no AWS call and no state read: the
 * paths, the two units (vm_unit.service and vm_unit.socket, filled in), the memory fit, the
 * server build and its checks.
 *
 * NOT YET PORTED (they need aws.sh, state.sh and stack.sh, which the aws, state and stack
 * modules provide): vm_stack_instance, vm_find, vm_origin, vm_origin_secret, vm_type_mib's read
 * (cbc_vm_mib_text takes the text AWS printed), vm_plan_dry, vm_join, vm_ssm_run, vm_ship and
 * vm_undeploy. The remote scripts themselves are in vm_remote.c, and vm_remote.sh and vm_boot.sh
 * stay bash files that run on the VM.
 */
#include "vm.h"

#include "cli.h"
#include "conf.h"
#include "../../lib/num_parse.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CBC_TOOLS_DIR
#define CBC_TOOLS_DIR "tools"
#endif

const char *cbc_tools_dir(void) { return CBC_TOOLS_DIR; }

char *cbc_vm_file(const char *name) {
  char *p;
  if (asprintf(&p, "%s/cbase.d/%s", cbc_tools_dir(), name) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return p;
}

char *cbc_vm_read(const char *path) {
  char *data = NULL;
  size_t len = 0;
  if (!cbc_read_file(path, &data, &len)) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", path);
  return data;
}

/* A growing string: the shell's sed-style text transforms are built in one of these. */
void cbc_sb_add(cbc_sbuf *b, const char *p, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + n + 1) cap *= 2;
    char *nd = realloc(b->d, cap);
    if (!nd) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    b->d = nd;
    b->cap = cap;
  }
  memcpy(b->d + b->n, p, n);
  b->n += n;
  b->d[b->n] = '\0';
}
void cbc_sb_str(cbc_sbuf *b, const char *s) { cbc_sb_add(b, s, strlen(s)); }

/* replace_all: S with every FROM replaced by TO (a new string). */
static char *replace_all(const char *s, const char *from, const char *to) {
  cbc_sbuf b = { 0 };
  size_t fl = strlen(from);
  const char *p = s;
  const char *hit;
  while ((hit = strstr(p, from)) != NULL) {
    cbc_sb_add(&b, p, (size_t)(hit - p));
    cbc_sb_str(&b, to);
    p = hit + fl;
  }
  cbc_sb_str(&b, p);
  if (!b.d) cbc_sb_str(&b, "");
  return b.d;
}

/* transform_lines: TEXT run line by line through FN (which may edit the line in place, or return
 * NULL to drop it). A line keeps its newline when the input's did (sed's behaviour). */
typedef char *(*line_fn)(char *line, void *ctx);
static char *transform_lines(const char *text, line_fn fn, void *ctx) {
  cbc_sbuf out = { 0 };
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char *line = cbc_dupn(p, len);
    char *res = fn(line, ctx);
    if (res) {
      cbc_sb_str(&out, res);
      if (nl) cbc_sb_str(&out, "\n");
    }
    p = nl ? nl + 1 : p + len;
  }
  if (!out.d) cbc_sb_str(&out, "");
  return out.d;
}

/* The app's unit values, as vm_unit's sed script takes them. */
typedef struct {
  const char *port, *region, *backup_bucket;
  char *app, *user, *data, *home, *backup_key, *memory_line;
  char *mem_placeholder_value; /* NULL: the MemoryMax line is deleted (an undeclared app) */
} unit_ctx;

static char *unit_line(char *line, void *vctx) {
  unit_ctx *c = vctx;
  char *s = line;
  s = replace_all(s, "@APP@", c->app);
  s = replace_all(s, "@USER@", c->user);
  s = replace_all(s, "@PORT@", c->port);
  s = replace_all(s, "@DATA@", c->data);
  s = replace_all(s, "@HOME@", c->home);
  s = replace_all(s, "@REGION@", c->region);
  s = replace_all(s, "@BACKUP_BUCKET@", c->backup_bucket);
  s = replace_all(s, "@BACKUP_KEY@", c->backup_key);
  if (strcmp(s, "@MEMORY_MAX@") == 0) {
    if (!c->mem_placeholder_value) return NULL;
    return c->mem_placeholder_value;
  }
  return s;
}

char *cbc_vm_settings(void) {
  char *p;
  if (*cbc_conf.vm_settings) return cbc_dup(cbc_conf.vm_settings);
  if (*cbc_conf.vm_stack) {
    if (asprintf(&p, "/%s/%s/", cbc_conf.vm_stack, cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  } else {
    if (asprintf(&p, "/cbase/%s/", cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  }
  return p;
}

char *cbc_vm_data_dir(void) {
  char *p;
  if (*cbc_conf.vm_data) return cbc_dup(cbc_conf.vm_data);
  if (asprintf(&p, "/srv/cbase/%s/data", cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return p;
}

void cbc_vm_artifacts(const char *account, const char *region, const char *state_bucket, char **bucket, char **prefix) {
  char *a;
  if (*cbc_conf.vm_artifacts) {
    a = cbc_dup(cbc_conf.vm_artifacts);
  } else if (*cbc_conf.vm_stack) {
    if (asprintf(&a, "s3://cbase-code-%s-%s/vm/%s/", account, region, cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  } else {
    if (asprintf(&a, "s3://%s/artifacts/%s/", state_bucket, cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  }
  if (strncmp(a, "s3://", 5) == 0) a += 5;
  const char *slash = strchr(a, '/');
  if (slash) {
    *bucket = cbc_dupn(a, (size_t)(slash - a));
    *prefix = cbc_dup(slash + 1);
  } else {
    *bucket = cbc_dup(a);
    *prefix = cbc_dup(a);
  }
}

char *cbc_vm_backup_bucket(const char *state_bucket) {
  return *cbc_conf.backup_bucket ? cbc_dup(cbc_conf.backup_bucket) : cbc_dup(state_bucket);
}

char *cbc_vm_unit(const char *port, const char *region, const char *backup_bucket) {
  char *text = cbc_vm_read(cbc_vm_file("vm_unit.service"));
  unit_ctx c = { 0 };
  c.port = port;
  c.region = region;
  c.backup_bucket = backup_bucket;
  c.app = cbc_conf.app;
  if (asprintf(&c.user, "cbase-%s", cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  c.data = cbc_vm_data_dir();
  if (asprintf(&c.home, "/srv/cbase/%s", cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  if (*cbc_conf.backup_key) c.backup_key = cbc_conf.backup_key;
  else if (asprintf(&c.backup_key, "backups/%s/cbase.sqlite", cbc_conf.app) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  if (!cbc_conf.memory_estimated) {
    if (asprintf(&c.mem_placeholder_value, "MemoryMax=%sM", cbc_conf.memory) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  }
  return transform_lines(text, unit_line, &c);
}

static char *socket_line(char *line, void *vctx) {
  const char *const *v = vctx;
  return replace_all(replace_all(line, "@APP@", v[0]), "@PORT@", v[1]);
}

char *cbc_vm_socket(const char *port) {
  char *text = cbc_vm_read(cbc_vm_file("vm_unit.socket"));
  const char *v[2] = { cbc_conf.app, port };
  return transform_lines(text, socket_line, v);
}

void cbc_vm_fit(const char *type, long long mib, long long *limit, char **what) {
  *limit = mib - CBC_VM_OS_RESERVE;
  if (*limit < 0) *limit = 0;
  if (asprintf(what, "%s: %lld MiB less %d for the OS", type, mib, CBC_VM_OS_RESERVE) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
}

long long cbc_vm_mib_text(const char *type, const char *text) {
  /* The shell's $(...) keeps no trailing newline; the text is one value. */
  char *m = cbc_dup(text);
  size_t n = strlen(m);
  while (n > 0 && (m[n - 1] == '\n' || m[n - 1] == ' ' || m[n - 1] == '\t')) m[--n] = '\0';
  int64_t v = 0;
  if (!cbc_re("^[0-9]+$", m) || !num_parse_i64(m, n, 0, INT64_MAX, &v))
    cbc_fail(CBC_EXIT_FAIL, "instance type %s has no memory size AWS names", type);
  return (long long)v;
}

char *cbc_vm_sha256(const char *path) {
  char *argv1[] = { "sha256sum", (char *)path, NULL };
  cbc_buf out = { 0 };
  int rc = cbc_run_err(argv1, &out, NULL);
  if (rc == 127 || rc == -1) {
    free(out.data);
    out = (cbc_buf){ 0 };
    char *argv2[] = { "shasum", "-a", "256", (char *)path, NULL };
    rc = cbc_run_err(argv2, &out, NULL);
  }
  if (rc != 0 || !out.data) {
    free(out.data);
    return NULL;
  }
  size_t n = 0;
  while (out.data[n] && out.data[n] != ' ' && out.data[n] != '\n') n++;
  return cbc_dupn(out.data, n);
}

static bool is_regular(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void cbc_vm_check_elf(const char *path) {
  /* The ELF header: \177ELF, class 2 (64-bit), data 1 (little-endian), e_machine 0xb7 (aarch64). */
  unsigned char head[20];
  size_t got = 0;
  FILE *f = fopen(path, "rb");
  if (f) {
    got = fread(head, 1, sizeof head, f);
    fclose(f);
  }
  char magic[sizeof head * 2 + 1];
  for (size_t i = 0; i < got; i++) snprintf(magic + 2 * i, 3, "%02x", head[i]);
  magic[got * 2] = '\0';
  if (strncmp(magic, "7f454c460201", 12) == 0) {
    if (strlen(magic) < 40 || strncmp(magic + 36, "b700", 4) != 0)
      cbc_fail(CBC_EXIT_FAIL, "%s is not an arm64 (aarch64) Linux binary, and the VM is arm64: build it with make serve-binary", path);
    return;
  }
  cbc_fail(CBC_EXIT_FAIL, "%s is not a 64-bit Linux (ELF) binary: build it with make serve-binary", path);
}

void cbc_vm_build(cbc_vm_artifact *out) {
  const char *env = getenv("CBASE_SERVE_BIN");
  if (env && *env) {
    out->bin = cbc_dup(env);
    if (!is_regular(out->bin)) cbc_fail(CBC_EXIT_FAIL, "CBASE_SERVE_BIN names %s, which is not a file", out->bin);
    printf("server: %s (CBASE_SERVE_BIN)\n", out->bin);
  } else {
    const char *dir = NULL;
    if (is_regular("./Makefile")) dir = ".";
    else if (is_regular("server/Makefile")) dir = "server";
    if (!dir)
      cbc_fail(CBC_EXIT_FAIL, "no Makefile at . or server/, so there is no server to build: the server's Makefile includes cbase.mk, whose make serve-binary builds it (or set CBASE_SERVE_BIN to a linux-arm64 binary). Nothing was changed.");
    printf("server: make -C %s serve-binary (linux arm64, static, in cbase's Alpine container)\n", dir);
    fflush(stdout);
    char *argv[] = { "make", "-C", (char *)dir, "serve-binary", NULL };
    if (cbc_run_err(argv, NULL, NULL) != 0)
      cbc_fail(CBC_EXIT_FAIL, "make -C %s serve-binary failed, so there is no linux-arm64 server to ship. It is cbase.mk's target for this build (ops/build_static.sh serve), and it needs Docker; run it until it passes, or set CBASE_SERVE_BIN to a linux-arm64 serve binary, then deploy again. Nothing was changed.", dir);
    if (asprintf(&out->bin, "%s/dist/%s", dir, CBC_VM_ARTIFACT_NAME) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
    if (!is_regular(out->bin))
      cbc_fail(CBC_EXIT_FAIL, "make -C %s serve-binary made no %s (cbase.mk's serve-binary writes dist/%s)", dir, out->bin, CBC_VM_ARTIFACT_NAME);
  }
  cbc_vm_check_elf(out->bin);
  out->sha = cbc_vm_sha256(out->bin);
  if (!out->sha) cbc_fail(CBC_EXIT_FAIL, "cannot read %s to hash it", out->bin);
  printf("server: %s, sha256 %s\n", out->bin, out->sha);
}
