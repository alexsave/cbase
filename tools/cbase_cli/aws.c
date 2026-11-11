/* aws.c - every AWS call the deploy tool makes (aws.sh): each one an AWS CLI call with its own
 * credential chain, its output read with --query and --output text, its failure classified by the
 * error code the CLI prints, and what of its text reaches the person masked.
 */
#include "aws.h"

#include "conf.h"

#include <fnmatch.h>
#include <regex.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *aws_account = "";

static char *err_path = "";   /* AWS_ERR: the last failed call's stderr */
static cbc_strs mask_values;   /* aws_mask_add's values */

void aws_mask_add(const char *value) { cbc_strs_push(&mask_values, cbc_dup(value)); }

static const int AWS_ATTEMPTS = 5;

void aws_init(void) {
  if (!cbc_on_path("aws"))
    cbc_fail(CBC_EXIT_FAIL, "the AWS CLI (aws) is not on PATH: install AWS CLI v2 (https://docs.aws.amazon.com/cli/)");
  err_path = cbc_tmp_path("aws.err");
  setenv("AWS_PAGER", "", 1);
  cbc_buf out = { 0 };
  if (aws_call(&out, "sts", "get-caller-identity", "--query", "Account", "--output", "text", NULL) != 0) {
    aws_fail("sts get-caller-identity");
  }
  cbc_strip_newlines(&out);
  aws_account = out.data ? out.data : "";
  if (!cbc_re("^[0-9]{12}$", aws_account)) {
    cbc_fail(CBC_EXIT_FAIL, "STS named the account \"%.40s\", which is not twelve digits", aws_account);
  }
}

/* aws_stagger_ms: 0-99, the name's cksum mod 100. Deterministic (state.sh says why). */
static long aws_stagger_ms(const char *name) { return (long)(cbc_cksum(name) % 100); }

/* aws_throttled: the last failure was AWS's throttling (aws.sh's pattern, case-insensitively). */
static bool aws_throttled(void) {
  return cbc_file_matches(err_path,
                          "Throttling|TooManyRequests|Too Many Requests|RequestLimitExceeded|"
                          "(HTTP|status|code)[^0-9A-Za-z]{0,8}429([^0-9]|$)|\\(429\\)",
                          true);
}

int aws_exec(cbc_buf *out, const char *const *head, const char *const *tail) {
  size_t n = 3;   /* "aws", "--no-cli-pager", the NULL */
  for (const char *const *p = head; p && *p; p++) n++;
  for (const char *const *p = tail; p && *p; p++) n++;
  const char **av = cbc_xmalloc(n * sizeof *av);
  size_t i = 0;
  av[i++] = "aws";
  av[i++] = "--no-cli-pager";
  for (const char *const *p = head; p && *p; p++) av[i++] = *p;
  for (const char *const *p = tail; p && *p; p++) av[i++] = *p;
  av[i] = NULL;
  cbc_buf discard = { 0 };
  cbc_buf *sink = out ? out : &discard;
  int rc;
  for (int k = 1;; k++) {
    rc = cbc_run_err((char *const *)av, sink, err_path);
    if (rc == 0 || !(k < AWS_ATTEMPTS && aws_throttled())) break;
    long ms = (1000L << (k - 1)) + aws_stagger_ms(cbc_conf.app ? cbc_conf.app : "");
    cbc_sleep_ms(ms);
  }
  free(av);
  return rc;
}

int aws_callv(cbc_buf *out, const char *const *head, va_list ap) {
  size_t n = 1;
  va_list count;
  va_copy(count, ap);
  while (va_arg(count, const char *)) n++;
  va_end(count);
  const char **tail = cbc_xmalloc(n * sizeof *tail);
  size_t i = 0;
  for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) tail[i++] = a;
  tail[i] = NULL;
  int rc = aws_exec(out, head, tail);
  free(tail);
  return rc;
}

int aws_call(cbc_buf *out, ...) {
  va_list ap;
  va_start(ap, out);
  int rc = aws_callv(out, NULL, ap);
  va_end(ap);
  return rc;
}

bool aws_err_contains(const char *text) {
  char *data;
  size_t len;
  if (!cbc_read_file(err_path, &data, &len)) return false;
  return strstr(data, text) != NULL;
}

char *aws_code(void) {
  char *data;
  size_t len;
  static char code[256];
  code[0] = '\0';
  if (!cbc_read_file(err_path, &data, &len)) return code;
  regex_t re;
  regcomp(&re, ".*An error occurred \\(([A-Za-z0-9.]*)\\).*", REG_EXTENDED);
  char *p = data;
  for (;;) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    regmatch_t m[2];
    if (regexec(&re, p, 2, m, 0) == 0) {
      size_t n = (size_t)(m[1].rm_eo - m[1].rm_so);
      if (n >= sizeof code) n = sizeof code - 1;
      memcpy(code, p + m[1].rm_so, n);
      code[n] = '\0';
      break;
    }
    if (!nl) break;
    p = nl + 1;
  }
  regfree(&re);
  return code;
}

/* aws_mask_line: a line naming an origin's headers or a parameter's value, or one holding a value this
 * run holds (aws_mask_values), is replaced: its text is not shown. */
static const char *MASKED = "(a line naming an origin header or a parameter value, not shown)";
const char *aws_mask(const char *line) {
  if (cbc_re_icase("OriginCustomHeaders|HeaderValue|ParameterValue", line)) return MASKED;
  for (size_t i = 0; i < mask_values.n; i++) {
    if (strstr(line, mask_values.v[i])) return MASKED;
  }
  return line;
}

void aws_show(int lines) {
  char *data;
  size_t len;
  if (!cbc_read_file(err_path, &data, &len)) return;
  char *p = data;
  int shown = 0;
  while (*p && shown < lines) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    fprintf(stderr, "  %s\n", aws_mask(p));
    shown++;
    if (!nl) break;
    p = nl + 1;
  }
}

static void aws_print_failure(const char *what) {
  char *code = aws_code();
  if (cbc_file_matches(err_path, "Unable to locate credentials|Token has expired|token.*expired|expired.*token|ExpiredToken|The SSO session|InvalidClientTokenId|refresh failed|no valid credential", false)) {
    fprintf(stderr, "cbase: %s: no usable AWS credentials. Run aws login (or aws sso login), or set AWS_PROFILE.\n", what);
  } else if (cbc_file_matches(err_path, "Unknown options: --if-", false)) {
    fprintf(stderr, "cbase: %s: this AWS CLI is too old for S3 conditional writes (put-object --if-match): update AWS CLI v2.\n", what);
  } else if (!strcmp(code, "AccessDenied") || !strcmp(code, "AccessDeniedException") || !strcmp(code, "UnauthorizedOperation")) {
    const char *profile = getenv("AWS_PROFILE");
    if (profile && *profile)
      fprintf(stderr, "cbase: %s: AWS refused it (AccessDenied) for profile %s. The deploy role needs the permission.\n", what, profile);
    else
      fprintf(stderr, "cbase: %s: AWS refused it (AccessDenied) for these credentials. The deploy role needs the permission.\n", what);
  } else {
    fprintf(stderr, "cbase: %s failed:\n", what);
    aws_show(5);
  }
}

char *aws_region_get(void) {
  const char *r = getenv("AWS_REGION");
  if (r && *r) return cbc_dup(r);
  r = getenv("AWS_DEFAULT_REGION");
  if (r && *r) return cbc_dup(r);
  char *argv[] = { "aws", "configure", "get", "region", NULL };
  cbc_buf out = { 0 };
  cbc_run_err(argv, &out, "/dev/null");
  if (!out.data) return cbc_dup("");
  cbc_strip_newlines(&out);
  return out.data;
}

char *aws_region(void) {
  char *r = aws_region_get();
  return *r ? r : "the configured region";
}

char *aws_region_required(void) {
  char *r = aws_region_get();
  if (!cbc_re("^[a-z]{2}(-[a-z]+)+-[0-9]$", r))
    cbc_fail(CBC_EXIT_FAIL, "no AWS region is set: set AWS_REGION (us-east-1), or the profile's region (aws configure set region us-east-1)");
  return r;
}

int aws_group_find(const char *kind, const char *name, char **id) {
  const char *tag = strcmp(kind, "vm") == 0 ? "cbase:vm-group" : "cbase:static-group";
  const char *type = strcmp(kind, "vm") == 0 ? "ec2:instance" : "s3";
  char *filter = NULL;
  size_t need = strlen(tag) + strlen(name) + 16;
  filter = cbc_xmalloc(need);
  snprintf(filter, need, "Key=%s,Values=%s", tag, name);
  cbc_buf out = { 0 };
  char what[256];
  snprintf(what, sizeof what, "finding %s group %s (tag:GetResources)", kind, name);
  if (aws_call(&out, "resourcegroupstaggingapi", "get-resources", "--tag-filters", filter,
               "--resource-type-filters", type, "--query", "ResourceTagMappingList[].ResourceARN",
               "--output", "text", NULL) != 0) {
    aws_print_failure(what);
    return 1;
  }
  cbc_strs arns = { 0 };
  cbc_split_on(out.data ? out.data : "", " \t\n", &arns);
  if (arns.n == 0) {
    fprintf(stderr, "cbase: there is no %s group named %s (nothing tagged %s=%s in %s). Create it with: ./cbase group-init %s %s\n",
            kind, name, tag, name, aws_region(), kind, name);
    return 1;
  }
  if (arns.n > 1) {
    fprintf(stderr, "cbase: %zu resources are tagged %s=%s, and a %s group is one %s: ", arns.n, tag, name, kind,
            strcmp(kind, "vm") == 0 ? "instance" : "bucket");
    for (size_t i = 0; i < arns.n; i++) fprintf(stderr, "%s%s", i ? " " : "", arns.v[i]);
    fprintf(stderr, "\n");
    return 1;
  }
  const char *arn = arns.v[0];
  char kind_arn[512];
  snprintf(kind_arn, sizeof kind_arn, "%s:%s", kind, arn);
  if (strcmp(kind, "vm") == 0 && fnmatch("vm:arn:aws*:ec2:*:instance/i-*", kind_arn, 0) == 0) {
    *id = cbc_dup(strrchr(arn, '/') + 1);
    return 0;
  }
  if (strcmp(kind, "static") == 0 && fnmatch("static:arn:aws*:s3:::*", kind_arn, 0) == 0) {
    const char *last = NULL;
    for (const char *p = arn; (p = strstr(p, ":::")) != NULL; p += 3) last = p;
    *id = cbc_dup(last + 3);
    return 0;
  }
  fprintf(stderr, "cbase: %s=%s is on %s, which is not a %s\n", tag, name, arn, type);
  return 1;
}

void aws_no_state_bucket(const char *bucket) {
  cbc_fail(CBC_EXIT_FAIL, "there is no state bucket s3://%s in this account: ./cbase group-init makes it with the first group", bucket);
}

int aws_s3_get(const char *bucket, const char *key, const char *file, char **etag) {
  cbc_buf out = { 0 };
  if (aws_call(&out, "s3api", "get-object", "--bucket", bucket, "--key", key, file,
               "--query", "ETag", "--output", "text", NULL) == 0) {
    cbc_strip_newlines(&out);
    *etag = out.data ? out.data : cbc_dup("");
    return 0;
  }
  char *code = aws_code();
  if (!strcmp(code, "NoSuchKey")) return 3;
  if (!strcmp(code, "NoSuchBucket")) aws_no_state_bucket(bucket);
  char what[512];
  snprintf(what, sizeof what, "reading s3://%s/%s", bucket, key);
  aws_fail(what);
}

int aws_s3_put(const char *bucket, const char *key, const char *file, const char *cond) {
  cbc_buf discard = { 0 };
  int rc;
  if (strcmp(cond, "*") == 0)
    rc = aws_call(&discard, "s3api", "put-object", "--bucket", bucket, "--key", key, "--body", file,
                  "--content-type", "text/plain; charset=utf-8", "--if-none-match", "*",
                  "--query", "ETag", "--output", "text", NULL);
  else
    rc = aws_call(&discard, "s3api", "put-object", "--bucket", bucket, "--key", key, "--body", file,
                  "--content-type", "text/plain; charset=utf-8", "--if-match", cond,
                  "--query", "ETag", "--output", "text", NULL);
  if (rc == 0) return 0;
  char *code = aws_code();
  if (!strcmp(code, "PreconditionFailed") || !strcmp(code, "ConditionalRequestConflict") || !strcmp(code, "NoSuchKey"))
    return 3;
  if (!strcmp(code, "NoSuchBucket")) aws_no_state_bucket(bucket);
  char what[512];
  snprintf(what, sizeof what, "writing s3://%s/%s", bucket, key);
  aws_fail(what);
}

void aws_fail(const char *what) {
  aws_print_failure(what);
  exit(CBC_EXIT_FAIL);
}

bool aws_poll(int tries, bool (*fn)(void)) {
  const char *env = getenv("CBASE_POLL_SECONDS");
  long secs = env && *env ? atol(env) : 5;
  for (int n = 1;; n++) {
    if (fn()) return true;
    if (n >= tries) return false;
    cbc_sleep_ms(secs * 1000L);
  }
}

void aws_set_err_text(const char *text) {
  FILE *f = fopen(err_path, "w");
  if (!f) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", err_path);
  fputs(text, f);
  fclose(f);
}

/* The plain parameters, whose values may be printed (aws.sh's AWS_PLAIN_PARAMS). */
static const char *AWS_PLAIN_PARAMS[] = { "VmOrigin", "VmPort", "InstanceType", "ImageId", "CloudFrontPrefixList", "ArtifactBucket", NULL };

char *aws_secret_filter(const char *path, const char *before, const char *after, const char *cause) {
  cbc_strs parts = { 0 };
  static const char *const ws[] = { "OriginCustomHeaders", "HeaderValue", NULL };
  size_t total = 0;
  char *f;
  for (const char *const *w = ws; *w; w++) {
    size_t need = strlen(path) + strlen(before) + strlen(after) + strlen(*w) * 3 + 160;
    char *t = cbc_xmalloc(need);
    snprintf(t, need, "contains(%s || '', '%s') || contains(%s || '', '%s') || contains(%s || '', '%s')",
             path, *w, before, *w, after, *w);
    cbc_strs_push(&parts, t);
    total += strlen(t) + 4;
  }
  size_t cap = total + 512;
  f = cbc_xmalloc(cap);
  f[0] = '\0';
  for (size_t i = 0; i < parts.n; i++) {
    if (i) strcat(f, " || ");
    strcat(f, parts.v[i]);
  }
  if (cause && *cause) {
    strcat(f, " || (ChangeSource == 'ParameterReference'");
    for (const char *const *q = AWS_PLAIN_PARAMS; *q; q++) {
      size_t n = strlen(f);
      snprintf(f + n, cap - n, " && CausingEntity != '%s'", *q);
    }
    strcat(f, ")");
  }
  return f;
}
