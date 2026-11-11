/* stack.c - the tool's CloudFormation stacks (stack.sh): the templates rendered for an app, the route
 * behaviours, the router's size check, and the stack's status and outputs. The change-set flow (drift,
 * the guards, execute, discard, delete) is still a stub: the rest of milestone 4.
 */
#include "stack.h"

#include "aws.h"
#include "cli.h"
#include "conf.h"

#include <regex.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The embedded templates: build/gen/tpl_*.c, made by tools/embed.c from tools/cbase.d/templates. */
extern const char tpl_app_yaml[];
extern const unsigned long tpl_app_yaml_len;
extern const char tpl_group_vm_yaml[];
extern const unsigned long tpl_group_vm_yaml_len;
extern const char tpl_group_static_yaml[];
extern const unsigned long tpl_group_static_yaml_len;

char *stk_name = "";
char *stk_region = "";
char *stk_tag = "";
char *stk_caps = "";
bool stk_app = false;
char *stk_template = "";
char *stk_status = "";
char *stk_type = "";
char *stk_cs = "cbase-deploy";
char *stk_changes = "";
char *stk_cert = "";
char *stk_api_cert = "";
char *stk_bucket = "";
char *stk_bucket_region = "";
char *stk_prefix = "";
bool stk_no_rollback = false;
char *stk_files = "";
char *stk_addr = "";

/* The CloudFront policies and the router's size limit (stack.sh's constants). */
#define CF_CACHING_OPTIMIZED "658327ea-f89d-4fab-a63d-7e88639e58f6"
#define CF_CACHING_DISABLED "4135ea2d-6df8-44a3-9df3-4b5a84be39ad"
#define CF_ALL_VIEWER_EXCEPT_HOST "b689b0a8-53d0-40ab-baf2-68738e2966ac"
#define CF_USE_ORIGIN_CC_QS "4cc15a8a-d715-48a4-82b8-cc0b614638fe"
#define STACK_FUNCTION_MAX 10240

/* ---- the stack's identity ---------------------------------------------------------------------- */

static cbc_strs stack_params; /* "ParameterKey=K,ParameterValue=V" each */

void stack_use(const char *name, const char *region, const char *tag) {
  stk_name = cbc_dup(name);
  stk_region = cbc_dup(region);
  stk_tag = cbc_dup(tag);
  stk_caps = "";
  stack_params.n = 0;
  stk_app = false;
  stk_template = cbc_tmp_path("stack.yaml");
}

void stack_app_use(void) {
  char name[256], tag[256];
  snprintf(name, sizeof name, "cbase-app-%s", cbc_conf.app);
  snprintf(tag, sizeof tag, "Key=cbase:app,Value=%s", cbc_conf.app);
  stack_use(name, "us-east-1", tag);
  stk_app = true;
}

void stack_params_clear(void) { stack_params.n = 0; }

void stack_param_add(const char *key, const char *value) {
  size_t need = strlen(key) + strlen(value) + 40;
  char *p = cbc_xmalloc(need);
  snprintf(p, need, "ParameterKey=%s,ParameterValue=%s", key, value);
  cbc_strs_push(&stack_params, p);
}

void stack_plan(void) {
  stack_render(stk_template);
  stack_changeset();
}

void stack_param_previous(const char *key) {
  char *p = cbc_xmalloc(strlen(key) + 40);
  sprintf(p, "ParameterKey=%s,UsePreviousValue=true", key);
  cbc_strs_push(&stack_params, p);
}

/* cfn_run: one CloudFormation call in the stack's region, with the words as a NULL-terminated list
 * (defined with the change-set flow below). */
static int cfn_run(cbc_buf *out, ...);

void stack_status(void) {
  cbc_buf out = { 0 };
  if (cfn_run(&out, "describe-stacks", "--stack-name", stk_name, "--query", "Stacks[0].StackStatus",
              "--output", "text", NULL) == 0) {
    cbc_strip_newlines(&out);
    stk_status = out.data ? out.data : "";
    return;
  }
  if (!strcmp(aws_code(), "ValidationError") && aws_err_contains("does not exist")) {
    stk_status = "NONE";
    return;
  }
  char what[256];
  snprintf(what, sizeof what, "reading the %s stack", stk_name);
  aws_fail(what);
}

char *stack_output(const char *key) {
  cbc_buf out = { 0 };
  char query[256];
  snprintf(query, sizeof query, "Stacks[0].Outputs[?OutputKey=='%s'].OutputValue", key);
  if (cfn_run(&out, "describe-stacks", "--stack-name", stk_name, "--query", query, "--output", "text", NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "reading %s's outputs", stk_name);
    aws_fail(what);
  }
  cbc_strip_newlines(&out);
  if (!out.data || !strcmp(out.data, "None")) return "";
  return out.data;
}


/* ---- memory files ------------------------------------------------------------------------------ */

typedef struct {
  FILE *f;
  char *data;
  size_t len;
} memfile_t;

/* memfile_open: M is set up IN PLACE. open_memstream keeps the addresses of m->data and m->len and
 * writes the buffer through them when the stream is flushed or closed, so M must be the caller's own
 * variable, never a local that is copied out: returned by value, the stream wrote into a dead stack
 * slot, the caller's copy kept a NULL buffer, and a GCC build (no named return value optimisation in
 * C, unlike clang) rendered every memfile as empty text (the app template lost its routes). */
static void memfile_open(memfile_t *m) {
  *m = (memfile_t){ 0 };
  m->f = open_memstream(&m->data, &m->len);
  if (!m->f) cbc_fail(CBC_EXIT_FAIL, "out of memory");
}

/* memfile_close: the text written, with its trailing newlines removed (as $(...) removes them). */
static char *memfile_close(memfile_t *m) {
  fclose(m->f);
  char *s = m->data ? m->data : cbc_dup("");
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == '\n') s[--n] = '\0';
  return s;
}

/* ---- templates --------------------------------------------------------------------------------- */

typedef struct {
  char *name;
  char *value;
} tvar_t;
static tvar_t *tvars;
static size_t tvars_n, tvars_cap;

void template_clear(void) { tvars_n = 0; }

void template_set(const char *name, const char *value) {
  for (size_t i = 0; i < tvars_n; i++) {
    if (!strcmp(tvars[i].name, name)) {
      tvars[i].value = cbc_dup(value);
      return;
    }
  }
  if (tvars_n == tvars_cap) {
    size_t cap = tvars_cap ? tvars_cap * 2 : 16;
    tvar_t *nv = cbc_xrealloc(tvars, cap * sizeof *nv);
    tvars = nv;
    tvars_cap = cap;
  }
  tvars[tvars_n].name = cbc_dup(name);
  tvars[tvars_n].value = cbc_dup(value);
  tvars_n++;
}

static const char *template_value(const char *name) {
  for (size_t i = 0; i < tvars_n; i++) {
    if (!strcmp(tvars[i].name, name)) return tvars[i].value;
  }
  return "";
}

static const char *template_bytes(const char *file, unsigned long *len) {
  if (!strcmp(file, "app.yaml")) { *len = tpl_app_yaml_len; return tpl_app_yaml; }
  if (!strcmp(file, "group-vm.yaml")) { *len = tpl_group_vm_yaml_len; return tpl_group_vm_yaml; }
  if (!strcmp(file, "group-static.yaml")) { *len = tpl_group_static_yaml_len; return tpl_group_static_yaml; }
  cbc_fail(CBC_EXIT_FAIL, "internal: no template %s", file);
}

/* flag_in: whether FLAG is one of the blank-separated FLAGS. */
static bool flag_in(const char *flag, const char *flags) {
  size_t fl = strlen(flag);
  const char *p = flags;
  while (*p) {
    while (*p == ' ') p++;
    const char *b = p;
    while (*p && *p != ' ') p++;
    if ((size_t)(p - b) == fl && !memcmp(b, flag, fl)) return true;
  }
  return false;
}

void template_render(const char *file, const char *flags, FILE *out) {
  unsigned long len;
  const char *text = template_bytes(file, &len);
  int skip = 0, depth = 0;
  size_t start = 0;
  regex_t re;
  if (regcomp(&re, "__([A-Z0-9]+)__", REG_EXTENDED) != 0) cbc_fail(CBC_EXIT_FAIL, "internal: bad pattern");
  for (size_t i = 0; i <= len; i++) {
    if (i < len && text[i] != '\n') continue;
    if (i == start && i == len) break;
    char *line = cbc_dupn(text + start, i - start);
    start = i + 1;
    if (!strncmp(line, "#if ", 4)) {
      depth++;
      const char *f = line + 4;
      if (skip == 0) {
        if (f[0] == '!') {
          if (flag_in(f + 1, flags)) skip = depth;
        } else if (!flag_in(f, flags)) {
          skip = depth;
        }
      }
      continue;
    }
    if (!strcmp(line, "#end")) {
      if (skip == depth) skip = 0;
      depth--;
      continue;
    }
    if (skip) continue;
    /* each __NAME__ replaced by its value; a value is never read again */
    const char *rest = line;
    regmatch_t m[2];
    while (regexec(&re, rest, 2, m, 0) == 0) {
      char name[128] = "";
      size_t nl = (size_t)(m[1].rm_eo - m[1].rm_so);
      if (nl >= sizeof name) nl = sizeof name - 1;
      memcpy(name, rest + m[1].rm_so, nl);
      fwrite(rest, 1, (size_t)m[0].rm_so, out);
      fputs(template_value(name), out);
      rest += m[0].rm_eo;
    }
    fputs(rest, out);
    fputc('\n', out);
  }
  regfree(&re);
}

/* ---- the routes -------------------------------------------------------------------------------- */

static const char *stack_methods(const char *methods) {
  char padded[256];
  snprintf(padded, sizeof padded, ",%s,", methods);
  if (strstr(padded, ",ALL,") || strstr(padded, ",PUT,") || strstr(padded, ",POST,") || strstr(padded, ",PATCH,") ||
      strstr(padded, ",DELETE,"))
    return "[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE]";
  if (strstr(padded, ",OPTIONS,")) return "[GET, HEAD, OPTIONS]";
  return "[GET, HEAD]";
}

/* stack_behaviour: one route's behaviour, into F. RHP is the response headers policy's name, or NULL. */
static void stack_behaviour(FILE *f, const cbc_route *r, const char *rhp) {
  const char *pad = "          ";
  char pad_buf[64];
  if (strcmp(r->pattern, "*") != 0) {
    fprintf(f, "%s- PathPattern: \"%s\"\n", pad, r->pattern);
    snprintf(pad_buf, sizeof pad_buf, "%s  ", pad);
    pad = pad_buf;
  }
  fprintf(f, "%sTargetOriginId: %s\n", pad, r->origin);
  fprintf(f, "%sViewerProtocolPolicy: redirect-to-https\n", pad);
  fprintf(f, "%sAllowedMethods: %s\n", pad, stack_methods(r->methods));
  fprintf(f, "%sCachedMethods: [GET, HEAD]\n", pad);
  if (strcmp(r->origin, "ws") == 0) fprintf(f, "%sCompress: false\n", pad);
  else fprintf(f, "%sCompress: true\n", pad);
  const char *policy;
  if (strcmp(r->cache, "cache") == 0) policy = CF_CACHING_OPTIMIZED "   # Managed-CachingOptimized";
  else if (strcmp(r->cache, "origin") == 0) policy = CF_USE_ORIGIN_CC_QS "   # Managed-UseOriginCacheControlHeaders-QueryStrings";
  else policy = CF_CACHING_DISABLED "   # Managed-CachingDisabled";
  fprintf(f, "%sCachePolicyId: %s\n", pad, policy);
  if (strcmp(r->origin, "static") != 0) {
    fprintf(f, "%sOriginRequestPolicyId: %s   # Managed-AllViewerExceptHostHeader\n", pad, CF_ALL_VIEWER_EXCEPT_HOST);
    if (rhp && *rhp) fprintf(f, "%sResponseHeadersPolicyId: !Ref %s\n", pad, rhp);
  } else {
    fprintf(f, "%sFunctionAssociations:\n", pad);
    fprintf(f, "%s  - EventType: viewer-request\n", pad);
    fprintf(f, "%s    FunctionARN: !GetAtt Router.FunctionMetadata.FunctionARN\n", pad);
  }
}

/* stack_routes: a distribution's behaviours into F: DEFAULT, then ROUTES in order. */
static void stack_routes(FILE *f, const cbc_route *def, const cbc_routes *routes, const char *rhp) {
  fprintf(f, "        DefaultCacheBehavior:\n");
  stack_behaviour(f, def, rhp);
  if (routes->n) fprintf(f, "        CacheBehaviors:\n");
  for (size_t i = 0; i < routes->n; i++) stack_behaviour(f, &routes->v[i], rhp);
}

static char *routes_text(const cbc_route *def, const cbc_routes *routes, const char *rhp) {
  memfile_t m;
  memfile_open(&m);
  stack_routes(m.f, def, routes, rhp);
  return memfile_close(&m);
}

/* stack_pages: the build's page names, as the router takes them: each html file of stk_files less its
 * .html, blank-separated; a name of other characters, and a .well-known one, is left out. */
static char *stack_pages(void) {
  if (!*stk_files) return cbc_dup("");
  char *data;
  size_t len;
  if (!cbc_read_file(stk_files, &data, &len)) return cbc_dup("");
  memfile_t m;
  memfile_open(&m);
  bool first = true;
  char *p = data;
  for (;;) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (cbc_re("^[A-Za-z0-9_.~/-]+\\.html$", p) && strncmp(p, ".well-known/", 12) != 0) {
      size_t n = strlen(p) - 5;
      fprintf(m.f, "%s%.*s", first ? "" : " ", (int)n, p);
      first = false;
    }
    if (!nl) break;
    p = nl + 1;
  }
  return memfile_close(&m);
}

/* router_size: the router's code in bytes as CloudFront counts it (stack.sh's awk): its lines less the
 * template's eight-space indent, one newline between each. */
static long router_size(const char *text) {
  const char *marker = "      FunctionCode: |\n";
  const char *p = strstr(text, marker);
  if (!p) return 0;
  p += strlen(marker);
  long n = 0;
  int c = 0;
  while (*p && *p != '\n') {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    n += (long)len - 8 + (c++ ? 1 : 0);
    if (!nl) break;
    p = nl + 1;
  }
  return n;
}

void stack_render(const char *file) {
  bool has_static = false, has_vm = false, has_ws = false;
  for (size_t i = 0; i < cbc_conf.routes.n; i++) {
    const char *o = cbc_conf.routes.v[i].origin;
    if (!strcmp(o, "static")) has_static = true;
    else if (!strcmp(o, "vm")) has_vm = true;
    else if (!strcmp(o, "ws")) has_ws = true;
  }
  if (*cbc_conf.api_host) has_vm = true;
  bool apiws = has_ws;
  for (size_t i = 0; i < cbc_conf.api_routes.n; i++) {
    if (!strcmp(cbc_conf.api_routes.v[i].origin, "ws")) apiws = true;
  }
  memfile_t fm;
  memfile_open(&fm);
  if (has_static) fprintf(fm.f, " static");
  if (has_vm || has_ws) fprintf(fm.f, " vm");
  if (has_vm) fprintf(fm.f, " vmroute");
  if (has_ws) fprintf(fm.f, " ws");
  if (*stk_addr) fprintf(fm.f, " addr");
  if (*stk_cert && *cbc_conf.domain) fprintf(fm.f, " cert");
  if (*cbc_conf.api_host) fprintf(fm.f, " api");
  if (apiws) fprintf(fm.f, " apiws");
  if (*stk_api_cert) fprintf(fm.f, " apicert");
  if (strcmp(cbc_conf.aliases, "off") != 0) fprintf(fm.f, " aliases");
  char *flags = memfile_close(&fm);

  const char *shell = *cbc_conf.shell ? cbc_conf.shell : "/index.html";
  char *pages = cbc_dup("");
  if (has_static) {
    if (*stk_files) {
      char *data;
      size_t len;
      if (cbc_read_file(stk_files, &data, &len)) {
        bool found = false;
        char *p = data;
        for (;;) {
          char *nl = strchr(p, '\n');
          if (nl) *nl = '\0';
          if (!strcmp(p, shell + 1)) found = true;
          if (!nl) break;
          p = nl + 1;
        }
        if (!found)
          cbc_fail(CBC_EXIT_FAIL, "the app shell is %s (CBASE_SHELL), and the static dir has no %s: nothing was changed", shell, shell + 1);
      }
    }
    pages = stack_pages();
  }

  memfile_t am;

  memfile_open(&am);
  for (size_t i = 0; i < cbc_conf.domains.n; i++) fprintf(am.f, "%s%s", i ? ", " : "", cbc_conf.domains.v[i]);
  char *aliases = memfile_close(&am);

  /* The distribution's routes: every route but the last, the default being the last (conf.sh). */
  size_t nroutes = cbc_conf.routes.n;
  cbc_routes others = { 0 };
  for (size_t i = 0; i + 1 < nroutes; i++) {
    if (others.n == others.cap) {
      size_t cap = others.cap ? others.cap * 2 : 8;
      cbc_route *nv = cbc_xrealloc(others.v, cap * sizeof *nv);
      others.v = nv;
      others.cap = cap;
    }
    others.v[others.n++] = cbc_conf.routes.v[i];
  }
  char *routes = routes_text(&cbc_conf.routes.v[nroutes - 1], &others, NULL);

  /* The API host's: CBASE_API_ROUTE's, then the ws routes (CBASE_ROUTE's) it does not name. */
  cbc_routes api = { 0 };
  char seen[4096] = " ";
  for (size_t i = 0; i < cbc_conf.api_routes.n; i++) {
    cbc_route r = cbc_conf.api_routes.v[i];
    if (api.n == api.cap) {
      size_t cap = api.cap ? api.cap * 2 : 8;
      cbc_route *nv = cbc_xrealloc(api.v, cap * sizeof *nv);
      api.v = nv;
      api.cap = cap;
    }
    api.v[api.n++] = r;
    strncat(seen, r.pattern, sizeof seen - strlen(seen) - 2);
    strncat(seen, " ", sizeof seen - strlen(seen) - 1);
  }
  for (size_t i = 0; i < cbc_conf.routes.n; i++) {
    cbc_route r = cbc_conf.routes.v[i];
    char needle[300];
    snprintf(needle, sizeof needle, " %s ", r.pattern);
    if (strcmp(r.origin, "ws") != 0 || strstr(seen, needle)) continue;
    if (api.n == api.cap) {
      size_t cap = api.cap ? api.cap * 2 : 8;
      cbc_route *nv = cbc_xrealloc(api.v, cap * sizeof *nv);
      api.v = nv;
      api.cap = cap;
    }
    api.v[api.n++] = r;
  }
  cbc_route api_def = { cbc_dup("*"), cbc_dup("vm"), cbc_dup("nocache"), cbc_dup("ALL"), 0 };
  char *apiroutes = routes_text(&api_def, &api, "ServerTimingPolicy");

  template_clear();
  template_set("APP", cbc_conf.app);
  template_set("APIHOST", *cbc_conf.api_host ? cbc_conf.api_host : "");
  template_set("CERT", stk_cert);
  template_set("APICERT", stk_api_cert);
  template_set("ALIASES", aliases);
  template_set("BUCKET", stk_bucket);
  template_set("BUCKETREGION", stk_bucket_region);
  template_set("PREFIX", stk_prefix);
  template_set("SHELL", shell);
  template_set("PAGES", pages);
  template_set("ROUTES", routes);
  template_set("APIROUTES", apiroutes);
  template_set("ADDR", stk_addr);

  FILE *out = fopen(file, "w");
  if (!out) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", file);
  template_render("app.yaml", flags, out);
  fclose(out);

  /* The router's code, as CloudFront counts it: over its limit, the build has too many pages. */
  char *text;
  size_t len;
  if (!cbc_read_file(file, &text, &len)) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", file);
  long n = router_size(text);
  if (n > STACK_FUNCTION_MAX)
    cbc_fail(CBC_EXIT_FAIL, "the router's code is %ld bytes with the build's page list, over CloudFront's %d: the build has too many pages; nothing was changed", n, STACK_FUNCTION_MAX);
}


/* aws_fail_describe: a failed read of the change set (the shell's aws_fail "describing change set ..."). */
static void aws_fail_describe(void) {
  char what[256];
  snprintf(what, sizeof what, "describing change set %s", stk_cs);
  aws_fail(what);
}

static char *stack_details(const char *lid, const char *query);
static void stack_discard_impl(void);

/* ---- the change-set flow (stack.sh) ------------------------------------------------------------ */

/* cfn_head: the words every CloudFormation call starts with, in the stack's region. */
static const char *const *cfn_head(void) {
  static const char *head[4];
  head[0] = "cloudformation";
  head[1] = "--region";
  head[2] = stk_region;
  head[3] = NULL;
  return head;
}

/* cfn_run: one CloudFormation call in the stack's region, the words as a NULL-terminated varargs list. */
static int cfn_run(cbc_buf *out, ...) {
  va_list ap;
  va_start(ap, out);
  int rc = aws_callv(out, cfn_head(), ap);
  va_end(ap);
  return rc;
}

/* cfn_list: the same with the words in W (borrowed, NULL-terminated by this call). */
static int cfn_list(cbc_buf *out, cbc_strs *w) {
  cbc_strs_push(w, NULL);
  return aws_exec(out, cfn_head(), (const char *const *)w->v);
}

/* cs_run: a change set's call on STACK_NAME with STACK_CS (the shell's cs()), the words as varargs. */
static int cs_run(cbc_buf *out, ...) {
  const char *head[] = { "cloudformation", "--region", stk_region, "describe-change-set", "--stack-name", stk_name,
                         "--change-set-name", stk_cs, NULL };
  va_list ap;
  va_start(ap, out);
  int rc = aws_callv(out, head, ap);
  va_end(ap);
  return rc;
}

/* Output text: the shell's $(...) strips trailing newlines, and "None" is the CLI's empty answer. */
/* line_next: the next line of S in *cursor (NUL-terminated copy of it), or NULL at the end. */
static char *line_next(char **cursor) {
  char *p = *cursor;
  if (!p || !*p) return NULL;
  char *nl = strchr(p, '\n');
  if (nl) {
    *nl = '\0';
    *cursor = nl + 1;
  } else {
    *cursor = p + strlen(p);
  }
  return p;
}

/* stack_where: a property path's place, as the origin header and parameter lines name it. */
static char *stack_where(const char *p) {
  const char *q = p;
  if (!strncmp(q, "/Properties", 11)) q += 11;
  if (!strncmp(q, "/DistributionConfig/", 20)) q += 20;
  return cbc_dup(*q ? q : "the property");
}

/* stack_value: a value on one line, at most 160 bytes (a longer one cut to 157 and "..."). */
static char *stack_value(const char *v) {
  char *s = cbc_dup(v);
  for (char *p = s; *p; p++) {
    if (*p == '\n' || *p == '\t') *p = ' ';
  }
  if (strlen(s) > 160) {
    s[157] = '\0';
    size_t n = strlen(s);
    char *t = cbc_xmalloc(n + 4);
    snprintf(t, n + 4, "%s...", s);
    return t;
  }
  return s;
}

/* stack_drifts: the property differences of the drifted resource LID that QUERY selects (the shell's
 * stack_drifts): the query's tail is QUERY, the list's end included in it. */
static char *stack_drifts(const char *lid, const char *query) {
  char q[1024];
  snprintf(q, sizeof q, "StackResourceDrifts[?LogicalResourceId=='%s'] | [0].PropertyDifferences[%s", lid, query);
  cbc_buf out = { 0 };
  if (cfn_run(&out, "describe-stack-resource-drifts", "--stack-name", stk_name, "--stack-resource-drift-status-filters",
              "MODIFIED", "--query", q, "--output", "text", NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "reading %s's drift", lid);
    aws_fail(what);
  }
  return cbc_buf_text(&out);
}

/* ---- drift ------------------------------------------------------------------------------------- */

static char *drift_id = "";
static char *drift_ds = "";
static char *drift_sds = "";

/* drift_read: one read of drift detection drift_id; true once it is no longer running. */
static bool drift_read(void) {
  cbc_buf out = { 0 };
  if (cfn_run(&out, "describe-stack-drift-detection-status", "--stack-drift-detection-id", drift_id, "--query",
              "[DetectionStatus,StackDriftStatus]", "--output", "text", NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "reading drift detection %s", drift_id);
    aws_fail(what);
  }
  char *s = cbc_buf_text(&out);
  char *tab = strchr(s, '\t');
  if (tab) {
    *tab = '\0';
    drift_ds = s;
    drift_sds = tab + 1;
  } else {
    drift_ds = s;
    drift_sds = "";
  }
  return strcmp(drift_ds, "DETECTION_IN_PROGRESS") != 0;
}

/* stack_drift_guard: stops on a drifted stack unless CBASE_ACCEPT_DRIFT=1 (--accept-drift). */
static void stack_drift_guard(void) {
  printf("drift: detecting on %s\n", stk_name);
  cbc_buf id = { 0 };
  if (cfn_run(&id, "detect-stack-drift", "--stack-name", stk_name, "--query", "StackDriftDetectionId", "--output", "text", NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "detecting drift on %s", stk_name);
    aws_fail(what);
  }
  drift_id = cbc_buf_text(&id);
  if (!aws_poll(121, drift_read)) {
    fprintf(stderr, "cbase: drift detection %s on %s did not finish\n", drift_id, stk_name);
    exit(CBC_EXIT_FAIL);
  }
  cbc_buf dr = { 0 };
  if (cfn_run(&dr, "describe-stack-resource-drifts", "--stack-name", stk_name, "--stack-resource-drift-status-filters",
              "MODIFIED", "DELETED", "--query", "StackResourceDrifts[].[LogicalResourceId,ResourceType,StackResourceDriftStatus]",
              "--output", "text", NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "reading %s's drift", stk_name);
    aws_fail(what);
  }
  char *drifted = cbc_buf_text(&dr);
  if (!strcmp(drifted, "None")) drifted = "";
  if (!strcmp(drift_ds, "DETECTION_COMPLETE") && !strcmp(drift_sds, "IN_SYNC") && !*drifted) {
    printf("drift: none\n");
    return;
  }
  if (strcmp(drift_ds, "DETECTION_COMPLETE") != 0)
    fprintf(stderr, "drift: detection ended %s, so the stack could not be shown to be in sync\n", drift_ds);
  char *filter = aws_secret_filter("PropertyPath", "ExpectedValue", "ActualValue", NULL);
  char *cursor = drifted;
  char *line;
  while ((line = line_next(&cursor)) != NULL) {
    char *f[3];
    cbc_read_tab_fields(line, f, 3);
    const char *lid = f[0], *type = f[1], *status = f[2];
    if (!*lid) continue;
    fprintf(stderr, "drift: %s (%s) %s\n", lid, type, status);
    if (strcmp(status, "MODIFIED") != 0) continue;
    char q1[2048];
    snprintf(q1, sizeof q1, "?PropertyPath && !(%s)].[PropertyPath,DifferenceType,ExpectedValue || '',ActualValue || '']", filter);
    char *text = stack_drifts(lid, q1);
    char *tc = text;
    while ((line = line_next(&tc)) != NULL) {
      char *g[4];
      cbc_read_tab_fields(line, g, 4);
      if (!*g[0]) continue;
      char *w = stack_where(g[0]);
      char *vb = stack_value(g[2]);
      char *va = stack_value(g[3]);
      fprintf(stderr, "drift:   %s %s: expected %s, actual %s\n", w, g[1], vb, va);
    }
    char q2[2048];
    snprintf(q2, sizeof q2, "?%s].PropertyPath", filter);
    char *paths = stack_drifts(lid, q2);
    char *pc = paths;
    while ((line = line_next(&pc)) != NULL) {
      /* read -r -a: the words of the line */
      char *p = line;
      for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *b = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char saved = *p;
        *p = '\0';
        if (strcmp(b, "None") != 0) {
          char *w = stack_where(b);
          fprintf(stderr, "drift:   origin header %s changed (its value is not shown)\n", w);
        }
        if (!saved) break;
        p++;
      }
    }
  }
  if (cbc_flags.accept_drift) {
    fprintf(stderr, "drift: accepted (--accept-drift): this deploy makes the stack's resources match the template again\n");
    return;
  }
  fprintf(stderr, "cbase: the %s stack drifted from its template (above): a deploy would undo those changes. Put the change in cbase.conf, or run ./cbase deploy --accept-drift (CBASE_ACCEPT_DRIFT=1) to overwrite it. Nothing was changed.\n", stk_name);
  exit(CBC_EXIT_FAIL);
}

/* ---- the change set ---------------------------------------------------------------------------- */

static void stack_discard_impl(void);

/* stack_print_changes: the change set, resource by resource, a secret's value never shown. */
static void stack_print_changes(void) {
  cbc_buf out = { 0 };
  if (cs_run(&out, "--query", "Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement]", "--output", "text", NULL) != 0)
    aws_fail_describe();
  char *lines = cbc_buf_text(&out);
  printf("change set %s on %s (%s):\n", stk_cs, stk_name, stk_type);
  char *filter = aws_secret_filter("Target.Path", "Target.BeforeValue", "Target.AfterValue", "cause");
  char *cursor = lines, *line;
  while ((line = line_next(&cursor)) != NULL) {
    char *f[4];
    cbc_read_tab_fields(line, f, 4);
    const char *action = f[0], *lid = f[1], *type = f[2], *repl = f[3];
    if (!*action) continue;
    if (strcmp(action, "Modify") != 0) {
      printf("  %-7s %-16s %s\n", action, lid, type);
      continue;
    }
    printf("  %-7s %-16s %s (replacement: %s)\n", action, lid, type, repl);
    char q1[2048];
    snprintf(q1, sizeof q1, "?Target.Name && !(%s)].[Target.Path || Target.Name,Target.BeforeValue || '',Target.AfterValue || '']", filter);
    char *text = stack_details(lid, q1);
    char *tc = text, *l2;
    while ((l2 = line_next(&tc)) != NULL) {
      char *g[3];
      cbc_read_tab_fields(l2, g, 3);
      if (!*g[0]) continue;
      char *w = stack_where(g[0]);
      if (!*g[1] && !*g[2]) printf("      %s changes\n", w);
      else printf("      %s: %s -> %s\n", w, stack_value(g[1]), stack_value(g[2]));
    }
    char q2[2048];
    snprintf(q2, sizeof q2, "?%s].[Target.Path || Target.Name || '',CausingEntity || '']", filter);
    char *text2 = stack_details(lid, q2);
    char *t2 = text2;
    while ((l2 = line_next(&t2)) != NULL) {
      char *g[2];
      cbc_read_tab_fields(l2, g, 2);
      if (!*g[0]) continue;
      char *w = stack_where(g[0]);
      if (strcmp(g[1], "None") != 0 && *g[1] && !strstr(g[0], "OriginCustomHeaders") && !strstr(g[0], "HeaderValue"))
        printf("      parameter %s changed %s (its value is not shown)\n", g[1], w);
      else
        printf("      origin header %s changed (its value is not shown)\n", w);
    }
  }
}

/* stack_details: the change set's details of the resource LID that QUERY selects. */
static char *stack_details(const char *lid, const char *query) {
  char q[2048];
  snprintf(q, sizeof q, "Changes[?ResourceChange.LogicalResourceId=='%s'] | [0].ResourceChange.Details[%s", lid, query);
  cbc_buf out = { 0 };
  if (cs_run(&out, "--include-property-values", "--query", q, "--output", "text", NULL) != 0) aws_fail_describe();
  return cbc_buf_text(&out);
}

/* stack_refusals: the change set's removals and replacements of the resources whose type is one of
 * TYPES (a JMESPath condition), "<logical id>\t<type>\t<action>\t<replacement>" a line; "" when none. */
static char *stack_refusals(const char *types) {
  char q[1024];
  snprintf(q, sizeof q,
           "Changes[?(%s) && (ResourceChange.Action=='Remove' || ResourceChange.Replacement=='True' || "
           "ResourceChange.Replacement=='Conditional')].ResourceChange.[LogicalResourceId,ResourceType,Action,Replacement]",
           types);
  cbc_buf out = { 0 };
  if (cs_run(&out, "--query", q, "--output", "text", NULL) != 0) aws_fail_describe();
  return cbc_buf_text(&out);
}

/* stack_aliases: each distribution's hostnames in a template, "<logical id> <host>" a line (stack.sh's awk). */
static char *stack_aliases(const char *text) {
  memfile_t m;
  memfile_open(&m);
  char *copy = cbc_dup(text);
  char *cursor = copy, *line;
  char lid[256] = "";
  while ((line = line_next(&cursor)) != NULL) {
    size_t n = strlen(line);
    /* /^  [A-Za-z]+:$/ */
    if (n >= 4 && line[0] == ' ' && line[1] == ' ' && line[n - 1] == ':') {
      bool letters = true;
      for (size_t i = 2; i + 1 < n; i++) {
        char c = line[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) letters = false;
      }
      if (letters && n > 3) {
        size_t k = 0;
        for (size_t i = 2; i + 1 < n && k < sizeof lid - 1; i++) lid[k++] = line[i];
        lid[k] = '\0';
      }
    }
    /* /^        Aliases: \[/ */
    if (!strncmp(line, "        Aliases: [", 18)) {
      char *s = line + 18;
      size_t sl = strlen(s);
      if (sl && s[sl - 1] == ']') s[sl - 1] = '\0';
      if (*s) {
        char *p = s;
        for (;;) {
          char *sep = strstr(p, ", ");
          if (sep) *sep = '\0';
          fprintf(m.f, "%s %s\n", lid, p);
          if (!sep) break;
          p = sep + 2;
        }
      }
    }
  }
  return memfile_close(&m);
}

/* sorted_lines: the lines of TEXT, sorted in byte order (sort with LC_ALL=C), a NULL-terminated array. */
static char **sorted_lines(const char *text, size_t *count) {
  char *copy = cbc_dup(text);
  cbc_strs v = { 0 };
  char *cursor = copy, *line;
  while ((line = line_next(&cursor)) != NULL) {
    if (*line) cbc_strs_push(&v, cbc_dup(line));
  }
  qsort(v.v, v.n, sizeof *v.v, cbc_cmp_str);
  *count = v.n;
  return v.v;
}

/* comm_only_left: the lines of A not in B, both sorted (comm -23), with multiplicity. */
static void comm_only_left(char **a, size_t na, char **b, size_t nb, cbc_strs *out) {
  size_t i = 0, j = 0;
  while (i < na) {
    if (j >= nb) {
      cbc_strs_push(out, cbc_dup(a[i++]));
      continue;
    }
    int c = strcmp(a[i], b[j]);
    if (c == 0) {
      i++;
      j++;
    } else if (c < 0) {
      cbc_strs_push(out, cbc_dup(a[i++]));
    } else {
      j++;
    }
  }
}

/* stack_downtime_guard: NO DOWNTIME after the first deploy (stack.sh's stack_downtime_guard). */
static void stack_downtime_guard(void) {
  cbc_strs why = { 0 };
  char *refused = stack_refusals("ResourceChange.ResourceType=='AWS::CloudFront::Distribution'");
  char *cursor = refused, *line;
  while ((line = line_next(&cursor)) != NULL) {
    char *f[4];
    cbc_read_tab_fields(line, f, 4);
    if (!*f[0]) continue;
    char buf[512];
    if (!strcmp(f[2], "Remove")) snprintf(buf, sizeof buf, "the distribution %s is removed", f[0]);
    else snprintf(buf, sizeof buf, "the distribution %s is replaced: a new one, with a new cloudfront.net name, and the old one deleted", f[0]);
    cbc_strs_push(&why, cbc_dup(buf));
  }
  cbc_buf old = { 0 };
  if (cfn_run(&old, "get-template", "--stack-name", stk_name, "--query", "TemplateBody", "--output", "text", NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "reading %s's template", stk_name);
    aws_fail(what);
  }
  char *old_text = old.data ? old.data : cbc_dup("");
  char *new_text;
  size_t nlen;
  if (!cbc_read_file(stk_template, &new_text, &nlen)) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", stk_template);
  char *old_aliases = stack_aliases(old_text);
  char *new_aliases = stack_aliases(new_text);
  size_t na, nb;
  char **a = sorted_lines(old_aliases, &na);
  char **b = sorted_lines(new_aliases, &nb);
  cbc_strs gone = { 0 };
  comm_only_left(a, na, b, nb, &gone);
  for (size_t i = 0; i < gone.n; i++) {
    char *g = gone.v[i];
    char *sp = strchr(g, ' ');
    if (!sp || !sp[1]) continue;
    *sp = '\0';
    char buf[512];
    snprintf(buf, sizeof buf, "the hostname %s is taken off the distribution %s", sp + 1, g);
    cbc_strs_push(&why, cbc_dup(buf));
  }
  if (why.n == 0) return;
  for (size_t i = 0; i < why.n; i++) fprintf(stderr, "downtime: %s\n", why.v[i]);
  if (cbc_flags.allow_downtime) {
    fprintf(stderr, "downtime: allowed (--allow-downtime): the app is unreachable at those hostnames until the change is done and DNS follows\n");
    return;
  }
  stack_discard_impl();
  fprintf(stderr, "cbase: this change set would make %s unreachable for a while (above), and after an app's first deploy no deploy may: refused, and the change set deleted; nothing was executed. Run ./cbase deploy --allow-downtime (CBASE_ALLOW_DOWNTIME=1) to accept it.\n", cbc_conf.app);
  exit(CBC_EXIT_FAIL);
}

/* stack_keep_guard: THE ONE HARD STOP. A bucket or a VM group's instance replaced or removed is refused. */
static void stack_keep_guard(void) {
  char *refused = stack_refusals("ResourceChange.ResourceType=='AWS::S3::Bucket' || ResourceChange.ResourceType=='AWS::EC2::Instance'");
  if (!*refused) return;
  char *cursor = refused, *line;
  while ((line = line_next(&cursor)) != NULL) {
    char *f[4];
    cbc_read_tab_fields(line, f, 4);
    if (!*f[0]) continue;
    const char *what = !strcmp(f[1], "AWS::S3::Bucket") ? "bucket" : "instance";
    if (!strcmp(f[2], "Remove")) fprintf(stderr, "refused: the %s %s would be removed\n", what, f[0]);
    else fprintf(stderr, "refused: the %s %s would be replaced (Replacement: %s)\n", what, f[0], f[3]);
  }
  stack_discard_impl();
  fprintf(stderr, "cbase: the change set would replace or remove a bucket (and every file in it) or a VM group's instance (and every app's data on its disk): refused, and the change set deleted; nothing was executed\n");
  exit(CBC_EXIT_FAIL);
}

/* stack_wait: CloudFormation's wait on STACK_NAME, or the stack's failed resources (masked) and the tool
 * stops. WHAT names the step for the message. */
static void stack_wait(const char *waiter, const char *what) {
  if (cfn_run(NULL, "wait", waiter, "--stack-name", stk_name, NULL) == 0) return;
  fprintf(stderr, "cbase: the %s stack %s (%s). Its failed resources:\n", stk_name, what, waiter);
  cbc_buf ev = { 0 };
  (void)cfn_run(&ev, "describe-stack-events", "--stack-name", stk_name, "--query",
                "StackEvents[?ends_with(ResourceStatus, 'FAILED')].[LogicalResourceId,ResourceStatusReason]", "--output", "text", NULL);
  aws_set_err_text(ev.data ? ev.data : "");
  aws_show(10);
  exit(CBC_EXIT_FAIL);
}

/* stack_discard_impl: deletes the change set, and the empty stack a first change set made. */
static void stack_discard_impl(void) {
  if (cfn_run(NULL, "delete-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs, NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "deleting change set %s", stk_cs);
    aws_fail(what);
  }
  if (!strcmp(stk_type, "CREATE") && !strcmp(stk_status, "NONE")) {
    if (cfn_run(NULL, "delete-stack", "--stack-name", stk_name, NULL) != 0) {
      char what[256];
      snprintf(what, sizeof what, "deleting the empty %s stack", stk_name);
      aws_fail(what);
    }
  }
}

void stack_discard(void) { stack_discard_impl(); }

/* stack_changeset: THE CHANGE-SET FLOW (stack.sh's stack_changeset) for STACK_NAME from STACK_TEMPLATE. */
void stack_changeset(void) {
  stack_status();
  if (!strcmp(stk_status, "NONE") || !strcmp(stk_status, "REVIEW_IN_PROGRESS")) {
    stk_type = "CREATE";
    printf("stack: %s is new\n", stk_name);
  } else if (!strcmp(stk_status, "ROLLBACK_COMPLETE")) {
    fprintf(stderr, "cbase: the %s stack's first create failed and rolled back (ROLLBACK_COMPLETE), and CloudFormation updates no such stack: delete it (aws cloudformation delete-stack --region %s --stack-name %s; it holds nothing), then deploy again\n",
            stk_name, stk_region, stk_name);
    exit(CBC_EXIT_FAIL);
  } else if (strlen(stk_status) >= 12 && !strcmp(stk_status + strlen(stk_status) - 12, "_IN_PROGRESS")) {
    fprintf(stderr, "cbase: the %s stack is %s: another deploy is running, or CloudFormation is still working; wait for it, then deploy again\n",
            stk_name, stk_status);
    exit(CBC_EXIT_FAIL);
  } else {
    stk_type = "UPDATE";
    printf("stack: %s is %s\n", stk_name, stk_status);
    stack_drift_guard();
  }
  /* A change set of this name left by a run that stopped: gone before the new one. */
  if (strcmp(stk_status, "NONE") != 0 &&
      cfn_run(NULL, "delete-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs, NULL) != 0) {
    if (strcmp(aws_code(), "ChangeSetNotFound") != 0) {
      char what[256];
      snprintf(what, sizeof what, "clearing change set %s", stk_cs);
      aws_fail(what);
    }
  }
  /* the words are borrowed: the list is never freed, and holds nothing that does not outlive the call */
  cbc_strs cw = { 0 };
  cbc_strs_push(&cw, (char *)"create-change-set");
  cbc_strs_push(&cw, (char *)"--stack-name");
  cbc_strs_push(&cw, (char *)stk_name);
  cbc_strs_push(&cw, (char *)"--change-set-name");
  cbc_strs_push(&cw, (char *)stk_cs);
  cbc_strs_push(&cw, (char *)"--change-set-type");
  cbc_strs_push(&cw, (char *)stk_type);
  size_t tn = strlen(stk_template) + 8;
  char *tbody = cbc_xmalloc(tn);
  snprintf(tbody, tn, "file://%s", stk_template);
  cbc_strs_push(&cw, (char *)"--template-body");
  cbc_strs_push(&cw, tbody);
  cbc_strs_push(&cw, (char *)"--tags");
  cbc_strs_push(&cw, (char *)stk_tag);
  if (*stk_caps) {
    cbc_strs_push(&cw, (char *)"--capabilities");
    cbc_strs_push(&cw, (char *)stk_caps);
  }
  for (size_t i = 0; i < stack_params.n; i++) {
    if (i == 0) cbc_strs_push(&cw, (char *)"--parameters");
    cbc_strs_push(&cw, (char *)stack_params.v[i]);
  }
  cbc_strs_push(&cw, (char *)"--query");
  cbc_strs_push(&cw, (char *)"Id");
  cbc_strs_push(&cw, (char *)"--output");
  cbc_strs_push(&cw, (char *)"text");
  cbc_buf discard = { 0 };
  if (cfn_list(&discard, &cw) != 0) {
    char what[256];
    snprintf(what, sizeof what, "creating a change set on %s", stk_name);
    aws_fail(what);
  }
  if (cfn_run(NULL, "wait", "change-set-create-complete", "--stack-name", stk_name, "--change-set-name", stk_cs, NULL) != 0) {
    cbc_buf why_b = { 0 };
    if (cs_run(&why_b, "--query", "StatusReason", "--output", "text", NULL) != 0) aws_fail_describe();
    char *why = cbc_buf_text(&why_b);
    if (strstr(why, "didn't contain changes") || strstr(why, "No updates are to be performed")) {
      printf("stack: %s is up to date (no changes)\n", stk_name);
      stk_changes = "";
      if (cfn_run(NULL, "delete-change-set", "--stack-name", stk_name, "--change-set-name", stk_cs, NULL) != 0) {
        char what[256];
        snprintf(what, sizeof what, "deleting change set %s", stk_cs);
        aws_fail(what);
      }
      return;
    }
    size_t wl = strlen(why);
    char *withnl = cbc_xmalloc(wl + 2);
    snprintf(withnl, wl + 2, "%s\n", why);
    aws_set_err_text(withnl);
    fprintf(stderr, "cbase: change set %s on %s failed:\n", stk_cs, stk_name);
    aws_show(5);
    stack_discard_impl();
    exit(CBC_EXIT_FAIL);
  }
  stk_changes = "1";
  stack_print_changes();
  stack_keep_guard();
  if (stk_app && !strcmp(stk_type, "UPDATE")) stack_downtime_guard();
  if (stk_app && cbc_flags.dry_run) stack_discard_impl();
}

void stack_execute(void) {
  cbc_strs w = { 0 };
  cbc_strs_push(&w, (char *)"execute-change-set");
  cbc_strs_push(&w, (char *)"--stack-name");
  cbc_strs_push(&w, (char *)stk_name);
  cbc_strs_push(&w, (char *)"--change-set-name");
  cbc_strs_push(&w, (char *)stk_cs);
  if (stk_no_rollback) cbc_strs_push(&w, (char *)"--disable-rollback");
  cbc_buf discard = { 0 };
  if (cfn_list(&discard, &w) != 0) {
    char what[256];
    snprintf(what, sizeof what, "executing change set %s on %s", stk_cs, stk_name);
    aws_fail(what);
  }
  printf("stack: executing %s (a new distribution takes some minutes)\n", stk_cs);
  const char *wname = "stack-update-complete";
  if (strcmp(stk_type, "UPDATE") != 0) wname = "stack-create-complete";
  stack_wait(wname, "did not finish");
  /* ${w%-complete}: the waiter's name without its "-complete" */
  char done[64];
  snprintf(done, sizeof done, "%s", wname);
  size_t dl = strlen(done);
  if (dl > 9 && !strcmp(done + dl - 9, "-complete")) done[dl - 9] = '\0';
  printf("stack: %s is %s complete\n", stk_name, done);
}

void stack_delete(void) {
  stack_status();
  if (!strcmp(stk_status, "NONE")) {
    printf("stack: there is no %s stack: nothing to delete\n", stk_name);
    return;
  }
  if (cfn_run(NULL, "delete-stack", "--stack-name", stk_name, NULL) != 0) {
    char what[256];
    snprintf(what, sizeof what, "deleting the %s stack", stk_name);
    aws_fail(what);
  }
  printf("stack: deleting %s (a distribution takes some minutes to go)\n", stk_name);
  stack_wait("stack-delete-complete", "was not deleted");
  printf("stack: %s is deleted\n", stk_name);
}
