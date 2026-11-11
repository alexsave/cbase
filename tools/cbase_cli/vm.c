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

#include "aws.h"
#include "cli.h"
#include "conf.h"
#include "deploy.h"
#include "group.h"
#include "state.h"
#include "stack.h"
#include "vm_remote.h"
#include "../../lib/num_parse.h"

#include <errno.h>
#include <stdarg.h>
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


/* ---- the AWS and SSM steps: aws.h's calls, state.h's join and leave ----------------------------- */

cbc_vm_t cbc_vm = { .id = "", .dns = "", .type = "", .bin = "", .sha = "", .key = "", .port = "",
                    .origin_secret = "", .ssm_status = "", .ssm_code = "" };
static char *fmt_dup(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *fmt_dup(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *p = NULL;
  if (vasprintf(&p, fmt, ap) < 0) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  va_end(ap);
  return p;
}

/* Splits TEXT at its first tab; the rest is what follows it ("" when there is no tab). */
static char *tab_split(char *text, char **rest) {
  char *tab = strchr(text, '\t');
  if (!tab) {
    *rest = "";
    return text;
  }
  *tab = '\0';
  *rest = tab + 1;
  return text;
}

/* The first line of TEXT, with its trailing newlines gone (a line of CLI text output). */
static char *first_line(cbc_buf *out) {
  cbc_strip_newlines(out);
  char *s = out->data ? cbc_dup(out->data) : cbc_dup("");
  char *nl = strchr(s, '\n');
  if (nl) *nl = '\0';
  return s;
}

/* The whitespace-separated words of TEXT (read -r -a). */
static cbc_strs words_of(cbc_buf *out) {
  cbc_strs w = { 0 };
  cbc_strip_newlines(out);
  char *p = out->data ? out->data : "";
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) break;
    char *e = p;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
    cbc_strs_push(&w, cbc_dupn(p, (size_t)(e - p)));
    p = e;
  }
  return w;
}

/* vm_stack_instance: the running instance an adopted stack holds: its AWS::EC2::Instance, or the
 * in-service instance of its auto-scaling group of one. */
static char *vm_stack_instance(const char *stack) {
  cbc_buf out = { 0 };
  if (aws_call(&out, "cloudformation", "describe-stack-resources", "--stack-name", stack,
               "--query", "StackResources[?ResourceType=='AWS::EC2::Instance' || ResourceType=='AWS::AutoScaling::AutoScalingGroup'].[ResourceType,PhysicalResourceId]",
               "--output", "text", NULL) != 0)
    aws_fail(fmt_dup("reading the stack %s (CBASE_VM_STACK)", stack));
  char *rest = NULL;
  char *first = first_line(&out);
  char *type = tab_split(first, &rest);
  /* read -r type id: the first line's two words; a tab or a space between them. */
  while (*rest == ' ') rest++;
  char *id = cbc_dup(rest);
  if (strcmp(type, "AWS::EC2::Instance") == 0) return id;
  if (strcmp(type, "AWS::AutoScaling::AutoScalingGroup") == 0) {
    cbc_buf ag = { 0 };
    if (aws_call(&ag, "autoscaling", "describe-auto-scaling-groups", "--auto-scaling-group-names", id,
                 "--query", "AutoScalingGroups[0].Instances[?LifecycleState=='InService'].InstanceId",
                 "--output", "text", NULL) != 0)
      aws_fail(fmt_dup("reading the auto-scaling group %s of %s", id, stack));
    cbc_strs in = words_of(&ag);
    if (in.n != 1 || strcmp(in.v[0], "None") == 0)
      cbc_fail(CBC_EXIT_FAIL, "the auto-scaling group %s of %s (CBASE_VM_STACK) has %zu instances in service, not one: a group is one VM",
               id, stack, in.n);
    return in.v[0];
  }
  cbc_fail(CBC_EXIT_FAIL, "the stack %s (CBASE_VM_STACK) holds no EC2 instance or auto-scaling group", stack);
}

/* vm_type_mib: the instance type's memory in MiB (describe-instance-types). */
static long long vm_type_mib(const char *type) {
  cbc_buf out = { 0 };
  if (aws_call(&out, "ec2", "describe-instance-types", "--instance-types", type,
               "--query", "InstanceTypes[0].MemoryInfo.SizeInMiB", "--output", "text", NULL) != 0)
    aws_fail(fmt_dup("reading instance type %s", type));
  cbc_strip_newlines(&out);
  return cbc_vm_mib_text(type, out.data ? out.data : "");
}

void cbc_vm_find(const char *group) {
  if (*cbc_conf.vm_stack) {
    cbc_vm.id = vm_stack_instance(cbc_conf.vm_stack);
  } else {
    char *id = NULL;
    /* aws_group_find prints its own message; the shell's exit inside its $(...) stops here. */
    if (aws_group_find("vm", group, &id) != 0) exit(CBC_EXIT_FAIL);
    cbc_vm.id = id;
  }
  cbc_buf out = { 0 };
  if (aws_call(&out, "ec2", "describe-instances", "--instance-ids", cbc_vm.id,
               "--query", "Reservations[0].Instances[0].[State.Name,PublicDnsName,InstanceType,Architecture]",
               "--output", "text", NULL) != 0)
    aws_fail(fmt_dup("reading instance %s", cbc_vm.id));
  char *text = first_line(&out);
  /* IFS=$'\t' read -r state VM_DNS VM_TYPE arch */
  char *state = tab_split(text, &text);
  char *dns = tab_split(text, &text);
  char *type = tab_split(text, &text);
  char *arch = text;
  cbc_vm.dns = strcmp(dns, "None") == 0 ? "" : dns;
  cbc_vm.type = type;
  if (strcmp(state, "running") != 0)
    cbc_fail(CBC_EXIT_FAIL, "vm group %s's instance %s is %s, not running: start it (aws ec2 start-instances --instance-ids %s) and run this again",
             group, cbc_vm.id, state, cbc_vm.id);
  if (strcmp(arch, "arm64") != 0)
    cbc_fail(CBC_EXIT_FAIL, "vm group %s's instance %s is %s, and cbase builds the server for linux arm64 alone (make serve-binary)",
             group, cbc_vm.id, arch);
  cbc_vm.mib = vm_type_mib(cbc_vm.type);
  printf("vm group %s: %s (%s, %lld MiB), %s", group, cbc_vm.id, cbc_vm.type, cbc_vm.mib,
         *cbc_vm.dns ? cbc_vm.dns : "no public DNS name");
  if (*cbc_conf.vm_stack) printf(", adopted from the stack %s", cbc_conf.vm_stack);
  printf("\n");
}

/* vm_fit_set: the fit check's limit for the join (state.h's state_fit_limit) and its sentence. */
void cbc_vm_fit_set(void) {
  long long limit = 0;
  char *what = NULL;
  cbc_vm_fit(cbc_vm.type, cbc_vm.mib, &limit, &what);
  state_fit_limit = (long)limit;
  state_fit_what = what;
}

/* vm_origin_secret: the app's origin secret, SSM /<settings>CBASE_ORIGIN_SECRET (a SecureString).
 * Its value is never printed: it is masked wherever it appears, and a bad one is refused without it. */
#define CBC_VM_DRY_SECRET "cbase-dry-run-stand-in-for-the-missing-origin-secret"

void cbc_vm_origin_secret(void) {
  char *settings = cbc_vm_settings();
  char *name = fmt_dup("%sCBASE_ORIGIN_SECRET", settings);
  cbc_buf out = { 0 };
  if (aws_call(&out, "ssm", "get-parameter", "--name", name, "--with-decryption",
               "--query", "Parameter.Value", "--output", "text", NULL) != 0) {
    cbc_vm.origin_secret = "";
    if (strcmp(aws_code(), "ParameterNotFound") == 0) {
      fprintf(stderr, "cbase: there is no SSM parameter %s, the origin secret CloudFront sends the app as X-Cbase-Origin (and serve checks). Make it once, then deploy again; nothing was changed:\n", name);
      fprintf(stderr, "  aws ssm put-parameter --type SecureString --name %s --value \"$(openssl rand -hex 32)\"\n", name);
      dry_stop(fmt_dup("no SSM parameter %s (the origin secret)", name), NULL);
      cbc_vm.origin_secret = CBC_VM_DRY_SECRET;
      return;
    }
    aws_fail(fmt_dup("reading the SSM parameter %s", name));
  }
  cbc_strip_newlines(&out);
  char *value = out.data ? out.data : "";
  if (!cbc_re("^[A-Za-z0-9_-]{32,128}$", value)) {
    cbc_vm.origin_secret = "";
    fprintf(stderr, "cbase: the SSM parameter %s is not 32 to 128 letters, digits, _ and - (its value is not shown): openssl rand -hex 32 makes one\n", name);
    dry_stop(fmt_dup("the SSM parameter %s is not an origin secret", name), NULL);
    cbc_vm.origin_secret = CBC_VM_DRY_SECRET;
    return;
  }
  cbc_vm.origin_secret = value;
  aws_mask_add(value);
  printf("origin secret: %s (X-Cbase-Origin; its value is not shown)\n", name);
}

/* vm_join: the app joins the group's state file, the port claimed and the memory fit checked in the
 * one conditional write (state_change). */
void cbc_vm_join(const char *group) {
  cbc_vm_fit_set();
  state_legacy_port = cbc_conf.vm_port;
  state_legacy_data = cbc_conf.vm_data;
  state_origin_in = cbc_conf.vm_origin;
  state_origin_need = true;
  state_change("vm", group, cbc_conf.app, cbc_conf_mem(), "join");
  cbc_vm.port = state_port;
  cbc_vm.new_member = state_joined_new;
}

/* base64 of N bytes, one line with no newline (the shell's base64 | tr -d '\n'). */
static char *base64_one_line(const char *in, size_t n) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *o = malloc(((n + 2) / 3) * 4 + 1);
  if (!o) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  size_t k = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned char)in[i] << 16;
    if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned char)in[i + 2];
    o[k++] = tbl[(v >> 18) & 63];
    o[k++] = tbl[(v >> 12) & 63];
    o[k++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
    o[k++] = i + 2 < n ? tbl[v & 63] : '=';
  }
  o[k] = '\0';
  return o;
}

/* Writes TEXT to PATH (the run's temporary files). */
static void write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f || fputs(text, f) == EOF || fclose(f) != 0) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", path);
}

static long vm_polls(void) {
  const char *env = getenv("CBASE_VM_POLLS");
  return env && *env ? atol(env) : 180;   /* get-command-invocation reads before the wait gives up */
}

static char *ssm_command_id = "";

static cbc_sbuf last_lines;   /* the masked CBASE lines of the last ssm_run (ssm-lines) */

/* vm_ssm_read: one read of the command; true once it has ended. */
static bool ssm_read(void) {
  cbc_buf out = { 0 };
  if (aws_call(&out, "ssm", "get-command-invocation", "--command-id", ssm_command_id, "--instance-id", cbc_vm.id,
               "--query", "[Status,ResponseCode]", "--output", "text", NULL) == 0) {
    char *rest = "";
    cbc_vm.ssm_status = tab_split(first_line(&out), &rest);
    cbc_vm.ssm_code = rest;
    if (strcmp(cbc_vm.ssm_status, "Pending") != 0 && strcmp(cbc_vm.ssm_status, "InProgress") != 0 &&
        strcmp(cbc_vm.ssm_status, "Delayed") != 0)
      return true;
  } else if (strcmp(aws_code(), "InvocationDoesNotExist") != 0) {
    aws_fail(fmt_dup("reading SSM command %s", ssm_command_id));
  }
  cbc_vm.ssm_status = "";
  return false;
}

/* vm_ssm_run: SCRIPT (the text) run as root on VM through SSM's AWS-RunShellScript, waited for with a
 * bounded poll. Prints only its "CBASE: " lines (masked), as "vm: " lines. Sets ssm_status and
 * ssm_code. The script travels base64, decoded to a file on the instance and run. */
static void ssm_run(const char *script, const char *what) {
  char *b64 = base64_one_line(script, strlen(script));
  char *cmd = fmt_dup("f=$(mktemp) && echo %s | base64 -d > $f && bash $f; rc=$?; rm -f $f; exit $rc", b64);
  char *param = fmt_dup("commands=%s", cmd);
  cbc_buf out = { 0 };
  if (aws_call(&out, "ssm", "send-command", "--instance-ids", cbc_vm.id, "--document-name", "AWS-RunShellScript",
               "--comment", fmt_dup("cbase %s", what), "--timeout-seconds", "600", "--parameters", param,
               "--query", "Command.CommandId", "--output", "text", NULL) != 0) {
    if (strcmp(aws_code(), "InvalidInstanceId") == 0)
      cbc_fail(CBC_EXIT_FAIL, "%s is not an SSM managed instance yet (a new instance takes a few minutes; its role must hold AmazonSSMManagedInstanceCore, as group-init's does)", cbc_vm.id);
    aws_fail(fmt_dup("sending the %s to %s (ssm send-command)", what, cbc_vm.id));
  }
  char *id = first_line(&out);
  printf("vm: the %s runs on %s (SSM command %s)\n", what, cbc_vm.id, id);
  fflush(stdout);
  ssm_command_id = id;
  if (!aws_poll((int)vm_polls() + 1, ssm_read))
    cbc_fail(CBC_EXIT_FAIL, "the %s on %s (SSM command %s) did not finish in time; its output may name the app's settings, so cbase does not print it (aws ssm get-command-invocation --command-id %s --instance-id %s)",
             what, cbc_vm.id, id, id, cbc_vm.id);
  cbc_buf res = { 0 };
  if (aws_call(&res, "ssm", "get-command-invocation", "--command-id", id, "--instance-id", cbc_vm.id,
               "--query", "StandardOutputContent", "--output", "text", NULL) != 0)
    aws_fail(fmt_dup("reading SSM command %s's output", id));
  /* grep '^CBASE: ', then aws_mask, then the sed that makes them "vm: " lines. */
  cbc_sbuf lines = { 0 };
  char *p = res.data ? res.data : "";
  while (*p) {
    char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= 8 && strncmp(p, "CBASE: ", 7) == 0) {
      char *line = cbc_dupn(p, len);
      cbc_sb_str(&lines, aws_mask(line));
      cbc_sb_str(&lines, "\n");
    }
    p = nl ? nl + 1 : p + len;
  }
  if (!lines.d) cbc_sb_str(&lines, "");
  write_text(cbc_tmp_path("ssm-lines"), lines.d);
  last_lines = lines;
  p = lines.d;
  while (*p) {
    char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= 7 && strncmp(p, "CBASE: ", 7) == 0) printf("vm: %.*s\n", (int)(len - 7), p + 7);
    else printf("%.*s\n", (int)len, p);
    p = nl ? nl + 1 : p + len;
  }
}

/* The exit status shown in the messages: the command's, or "?" when it has none. */
static const char *ssm_code_shown(void) { return *cbc_vm.ssm_code ? cbc_vm.ssm_code : "?"; }

/* vm_ship: the artifact uploaded (its sha256's key) and the service (re)started on VM. False when the
 * service was not started (said why): a port taken on the VM releases the claim this deploy made. */
bool cbc_vm_ship(const char *group) {
  char *bucket = NULL, *prefix = NULL;
  cbc_vm_artifacts(aws_account, aws_region_required(), state_bucket(), &bucket, &prefix);
  cbc_vm.key = fmt_dup("%s%s", prefix, cbc_vm.sha);
  char *region = aws_region_required();
  if (aws_call(NULL, "s3api", "head-object", "--bucket", bucket, "--key", cbc_vm.key,
               "--query", "ETag", "--output", "text", NULL) == 0) {
    printf("artifact: s3://%s/%s is already this server\n", bucket, cbc_vm.key);
  } else {
    const char *code = aws_code();
    if (strcmp(code, "404") != 0 && strcmp(code, "NoSuchKey") != 0 && strcmp(code, "NotFound") != 0)
      aws_fail(fmt_dup("reading s3://%s/%s", bucket, cbc_vm.key));
    if (aws_call(NULL, "s3api", "put-object", "--bucket", bucket, "--key", cbc_vm.key, "--body", cbc_vm.bin,
                 "--content-type", "application/octet-stream", "--query", "ETag", "--output", "text", NULL) != 0)
      aws_fail(fmt_dup("uploading s3://%s/%s", bucket, cbc_vm.key));
    printf("artifact: s3://%s/%s\n", bucket, cbc_vm.key);
  }
  char *script = cbc_vm_remote_deploy(cbc_vm.port, bucket, cbc_vm.key, cbc_vm.sha, region, state_bucket());
  write_text(cbc_tmp_path("vm-deploy.sh"), script);
  ssm_run(script, fmt_dup("deploy of %s", cbc_conf.app));
  if (strcmp(cbc_vm.ssm_status, "Success") == 0) return true;
  if (strcmp(cbc_vm.ssm_code, "3") == 0) {
    if (cbc_vm.new_member) {
      state_change("vm", group, cbc_conf.app, "", "leave");
      fprintf(stderr, "cbase: port %s is taken on %s by another process, so %s was not started, and its claim on the port is released. Free the port (or stop what holds it), then deploy again.\n",
              cbc_vm.port, cbc_vm.id, cbc_conf.app);
    } else {
      fprintf(stderr, "cbase: port %s is taken on %s by another process, so %s was not started; it keeps its port (it was a member before this deploy). Free the port, then deploy again.\n",
              cbc_vm.port, cbc_vm.id, cbc_conf.app);
    }
    return false;
  }
  fprintf(stderr, "cbase: the deploy of %s on %s ended %s (exit %s), above; the service's own log is on the instance: journalctl -u cbase-%s (aws ssm start-session --target %s)\n",
          cbc_conf.app, cbc_vm.id, cbc_vm.ssm_status, ssm_code_shown(), cbc_conf.app, cbc_vm.id);
  return false;
}

/* vm_undeploy: the app's service stopped and removed, then its membership left. */
void cbc_vm_undeploy(const char *group, bool delete_data) {
  cbc_vm_find(group);
  char *script = cbc_vm_remote_undeploy(delete_data);
  write_text(cbc_tmp_path("vm-undeploy.sh"), script);
  ssm_run(script, fmt_dup("undeploy of %s", cbc_conf.app));
  if (strcmp(cbc_vm.ssm_status, "Success") != 0)
    cbc_fail(CBC_EXIT_FAIL, "the undeploy of %s on %s ended %s (exit %s); it still holds its port, and nothing else was changed",
             cbc_conf.app, cbc_vm.id, cbc_vm.ssm_status, ssm_code_shown());
  state_change("vm", group, cbc_conf.app, "", "leave");
}

/* ---- vm_origin, vm_plan_dry and the entry points deploy, undeploy and group-destroy call ---- */

/* vm_origin GROUP: the group's origin (the state file's, or this app's CBASE_VM_ORIGIN recorded now),
 * read before anything is changed. A dry run with neither lists the problem and goes on with the
 * placeholder. */
void cbc_vm_origin(const char *group) {
  state_origin_in = cbc_conf.vm_origin;
  if (*cbc_conf.vm_stack && !cbc_group_state_bucket_found()) {
    state_no_bucket = true;
    state_read("vm", group);
    state_no_bucket = false;
  } else {
    state_read("vm", group);
  }
  char *held = state_member_origin;
  if (state_origin(group)) {
    cbc_vm.dns = state_member_origin;
    cbc_vm.origin_addr = state_member_origin_addr;
    if (*held)
      printf("vm origin: %s (%s), the group's (its state file)\n", cbc_vm.dns, cbc_vm.origin_addr);
    else
      printf("vm origin: %s (%s), the group's: CBASE_VM_ORIGIN, recorded in its state file by this deploy\n",
             cbc_vm.dns, cbc_vm.origin_addr);
    return;
  }
  char *what = fmt_dup("vm group %s has no origin recorded and %s names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once",
                       group, cbc_conf.app);
  dry_stop(what, fmt_dup("cbase: %s, in this app's cbase.conf. Nothing was changed.", what));
  cbc_vm.dns = *cbc_vm.dns ? cbc_vm.dns : "unrecorded-origin.invalid";
  cbc_vm.origin_addr = "";
}

/* vm_plan_dry GROUP: what a deploy would claim, from the state file, writing nothing. */
void cbc_vm_plan_dry(const char *group) {
  cbc_vm_fit_set();
  state_read("vm", group);
  long at = state_at(cbc_conf.app);
  state_port = (char *)state_port_of(group, cbc_conf.vm_port, at);
  char *line = state_vm_line(cbc_conf.app, state_port, cbc_conf_mem(), cbc_conf.vm_data);
  if (!state_fit(cbc_conf.app, line)) {
    fprintf(stderr, "%s\n", state_fit_report);
    fprintf(stderr, "cbase: %s would not fit on vm group %s (%s)\n", cbc_conf.app, group, state_fit_what);
    exit(CBC_EXIT_FAIL);
  }
  printf("vm: would join vm group %s as %s\n", group, line);
  printf("%s\n", state_fit_report);
  cbc_vm.port = state_port;
}

/* ---- the entry points ----
 * tools/cbase's deploy calls, in this order: cbc_vm_deploy_find (step 1), cbc_vm_deploy_build (step
 * 3, not on a dry run), cbc_vm_deploy_join (step 5, with the stack's VmOrigin = cbc_vm.dns, VmPort =
 * cbc_vm.port and OriginSecret = cbc_vm.origin_secret), then after the stack executes cbc_vm_ship
 * (step 7; on false it calls stack_discard and exits 1). undeploy calls cbc_vm_undeploy. group-destroy
 * calls cbc_vm_data_check on the instance it found. */

void cbc_vm_deploy_find(const char *group) {
  cbc_vm_find(group);
  char *bucket = NULL, *prefix = NULL;
  cbc_vm_artifacts(aws_account, aws_region_required(), state_bucket(), &bucket, &prefix);
  printf("vm paths: settings SSM %s, servers s3://%s/%s\n", cbc_vm_settings(), bucket, prefix);
  cbc_vm_origin(group);
  if (*cbc_conf.vm_stack &&
      cbc_group_state_bucket_adopted(aws_region_required(), group, cbc_dry_run, cbc_conf.app, cbc_conf.vm_port, cbc_conf.vm_data))
    state_no_bucket = true;   /* a dry run: the account has no state bucket, so no file is read (STATE_NO_BUCKET=1) */
  cbc_vm_origin_secret();
}

void cbc_vm_deploy_build(void) {
  cbc_vm_artifact a = { 0 };
  cbc_vm_build(&a);
  cbc_vm.bin = a.bin;
  cbc_vm.sha = a.sha;
}

void cbc_vm_deploy_join(const char *group, bool dry) {
  if (dry) cbc_vm_plan_dry(group);
  else cbc_vm_join(group);
}

/* group-destroy's data check: the instance INSTANCE is checked for the apps whose data is on it.
 * Fills APPS with each name and returns whether the command ended Success. The caller prints the
 * refusal and decides (its status is cbc_vm.ssm_status). */
bool cbc_vm_data_check(const char *instance, const char *group, cbc_strs *apps) {
  cbc_vm.id = cbc_dup(instance);
  char *script = cbc_vm_remote_data();
  write_text(cbc_tmp_path("vm-data.sh"), script);
  ssm_run(script, fmt_dup("data check of vm group %s", group));
  char *p = last_lines.d ? last_lines.d : "";
  while (*p) {
    char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len >= 13 && strncmp(p, "CBASE: data ", 12) == 0) cbc_strs_push(apps, cbc_dupn(p + 12, len - 12));
    p = nl ? nl + 1 : p + len;
  }
  return strcmp(cbc_vm.ssm_status, "Success") == 0;
}
