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
  char *p = malloc(need);
  if (!p) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  snprintf(p, need, "ParameterKey=%s,ParameterValue=%s", key, value);
  cbc_strs_push(&stack_params, p);
}

/* cfn_call: one CloudFormation call in the stack's region with the words ARGS (NULL-terminated). */
static int cfn_call(cbc_buf *out, const char *const *args) {
  cbc_strs words = { 0 };
  cbc_strs_push(&words, cbc_dup("cloudformation"));
  cbc_strs_push(&words, cbc_dup("--region"));
  cbc_strs_push(&words, cbc_dup(stk_region));
  for (const char *const *a = args; *a; a++) cbc_strs_push(&words, cbc_dup(*a));
  cbc_strs_push(&words, NULL);
  return aws_call_list(out, (const char *const *)words.v);
}

void stack_status(void) {
  cbc_buf out = { 0 };
  const char *args[] = { "describe-stacks", "--stack-name", stk_name, "--query", "Stacks[0].StackStatus",
                         "--output", "text", NULL };
  if (cfn_call(&out, args) == 0) {
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
  const char *args[] = { "describe-stacks", "--stack-name", stk_name, "--query", query, "--output", "text", NULL };
  if (cfn_call(&out, args) != 0) {
    char what[256];
    snprintf(what, sizeof what, "reading %s's outputs", stk_name);
    aws_fail(what);
  }
  cbc_strip_newlines(&out);
  if (!out.data || !strcmp(out.data, "None")) return "";
  return out.data;
}

void stack_changeset(void) { cbc_fail(CBC_EXIT_FAIL, "stack_changeset is not implemented in the C tool yet"); }
void stack_execute(void) { cbc_fail(CBC_EXIT_FAIL, "stack_execute is not implemented in the C tool yet"); }
void stack_discard(void) { cbc_fail(CBC_EXIT_FAIL, "stack_discard is not implemented in the C tool yet"); }
void stack_delete(void) { cbc_fail(CBC_EXIT_FAIL, "stack_delete is not implemented in the C tool yet"); }

/* ---- memory files ------------------------------------------------------------------------------ */

typedef struct {
  FILE *f;
  char *data;
  size_t len;
} memfile_t;

static memfile_t memfile_open(void) {
  memfile_t m = { 0 };
  m.f = open_memstream(&m.data, &m.len);
  if (!m.f) cbc_fail(CBC_EXIT_FAIL, "out of memory");
  return m;
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
    tvar_t *nv = realloc(tvars, cap * sizeof *nv);
    if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
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
  memfile_t m = memfile_open();
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
  memfile_t m = memfile_open();
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
  memfile_t fm = memfile_open();
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

  memfile_t am = memfile_open();
  for (size_t i = 0; i < cbc_conf.domains.n; i++) fprintf(am.f, "%s%s", i ? ", " : "", cbc_conf.domains.v[i]);
  char *aliases = memfile_close(&am);

  /* The distribution's routes: every route but the last, the default being the last (conf.sh). */
  size_t nroutes = cbc_conf.routes.n;
  cbc_routes others = { 0 };
  for (size_t i = 0; i + 1 < nroutes; i++) {
    if (others.n == others.cap) {
      size_t cap = others.cap ? others.cap * 2 : 8;
      cbc_route *nv = realloc(others.v, cap * sizeof *nv);
      if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
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
      cbc_route *nv = realloc(api.v, cap * sizeof *nv);
      if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
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
      cbc_route *nv = realloc(api.v, cap * sizeof *nv);
      if (!nv) cbc_fail(CBC_EXIT_FAIL, "out of memory");
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
