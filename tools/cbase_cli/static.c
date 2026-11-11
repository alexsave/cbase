/* static.c - an app's static files into its static group's bucket (static.h; static.sh's header says
 * what each rule is for). The per-file headers come from static_rules.tsv, which the Makefile turns
 * into the binary (build/gen/static_rules_tsv.c), so the tool needs no file beside it at run time.
 */
#include "static.h"

#include "aws.h"
#include "cli.h"
#include "conf.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *glob;
  char *type;
} static_rule;

/* The rules table, embedded at build time (build/gen/static_rules_tsv.c, from tools/cbase.d/static_rules.tsv,
 * as the templates are). Parsed once, the way the shell parses the file when it loads it. */
extern const char static_rules_tsv[];

static static_rule *rules;
static size_t rules_n;

#define CACHE_IMMUTABLE "public, max-age=31536000, immutable"
#define CACHE_NOCACHE "no-cache"

/* ---- small helpers ---------------------------------------------------------------------------- */

/* make_dir: DIR made (and noted, for the run's removal: the static tree is inside the temporary directory),
 * or already there. */
static void make_dir(const char *dir) {
  if (mkdir(dir, 0777) == 0) cbc_tmp_note(dir);
  else if (errno != EEXIST) cbc_fail(CBC_EXIT_FAIL, "cannot make %s", dir);
}

static void mkdir_p(const char *path) {
  char *p = cbc_dup(path);
  for (char *s = p + 1; *s; s++) {
    if (*s != '/') continue;
    *s = '\0';
    make_dir(p);
    *s = '/';
  }
  make_dir(p);
  free(p);
}

static bool strs_has(const cbc_strs *s, const char *v) {
  size_t lo = 0, hi = s->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = strcmp(s->v[mid], v);
    if (c == 0) return true;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

/* copy_file: SRC's bytes and mode into DST (cp -p's content, for a file a link cannot reach). */
static void copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", src);
  struct stat st;
  fstat(in, &st);
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
  if (out < 0) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", dst);
  char buf[65536];
  ssize_t r;
  while ((r = read(in, buf, sizeof buf)) > 0) {
    if (write(out, buf, (size_t)r) != r) cbc_fail(CBC_EXIT_FAIL, "cannot write %s", dst);
  }
  close(in);
  close(out);
}

/* ---- the scan --------------------------------------------------------------------------------- */

/* walk: every regular file under ROOT/REL, named relative to the static dir (find -type f: a symbolic
 * link is neither followed nor named). A directory that cannot be read stops the tool, as find's
 * failure did. */
static void walk(const char *root, const char *rel, cbc_strs *out) {
  char *dirpath = rel[0] ? cbc_fmt("%s%s%s", root, "/", rel) : cbc_dup(root);
  DIR *d = opendir(dirpath);
  if (!d) cbc_fail(CBC_EXIT_FAIL, "could not list every file under %s; nothing was uploaded", root);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char *child = rel[0] ? cbc_fmt("%s%s%s", rel, "/", e->d_name) : cbc_dup(e->d_name);
    char *full = cbc_fmt("%s%s%s", dirpath, "/", e->d_name);
    struct stat st;
    if (lstat(full, &st) != 0) cbc_fail(CBC_EXIT_FAIL, "could not list every file under %s; nothing was uploaded", root);
    if (S_ISDIR(st.st_mode)) {
      walk(root, child, out);
      free(child);
    } else if (S_ISREG(st.st_mode)) {
      cbc_strs_push(out, child);
    } else {
      free(child);
    }
    free(full);
  }
  closedir(d);
  free(dirpath);
}

/* hidden: find's dot rule as the shell wrote it, on "/" + the path: a part that starts with '.' is
 * left out, .well-known/ excepted (static.sh, WHAT IS UPLOADED). */
static bool hidden(const char *f) {
  char *p = cbc_fmt("%s%s%s", "/", "", f);
  bool skip = !strstr(p, "/.well-known/") && strstr(p, "/.") != NULL;
  free(p);
  return skip;
}

void cbc_static_scan(const char *dir, cbc_strs *files) {
  files->n = 0;
  if (!cbc_is_dir(dir))
    cbc_fail(CBC_EXIT_FAIL, "the static dir %s does not exist: build the app first, or set CBASE_STATIC_DIR in cbase.conf (the directory with index.html)", dir);
  char *index = cbc_fmt("%s%s%s", dir, "/", "index.html");
  if (!cbc_is_file(index))
    cbc_fail(CBC_EXIT_FAIL, "%s has no index.html, so it is not a built site: nothing was uploaded or deleted (CBASE_STATIC_DIR names the directory with index.html)", dir);
  free(index);
  cbc_strs all = { 0, 0, 0 };
  walk(dir, "", &all);
  for (size_t i = 0; i < all.n; i++) {
    char *f = all.v[i];
    if (hidden(f)) {
      free(f);
      continue;
    }
    for (const unsigned char *c = (const unsigned char *)f; *c; c++) {
      if (iscntrl(*c))
        cbc_fail(CBC_EXIT_FAIL, "%s holds a file whose name has a control character (a tab, a newline): rename it; nothing was uploaded", dir);
    }
    cbc_strs_push(files, f);
  }
  free(all.v);
  if (files->n > 1) qsort(files->v, files->n, sizeof *files->v, cbc_cmp_str);
}

/* ---- the headers ------------------------------------------------------------------------------ */

/* hashed: PATH is under one of CBASE_HASHED's globs (a case-sensitive match, as the shell's). */
static bool hashed(const char *path) {
  const cbc_conf_t *c = &cbc_conf;
  for (size_t i = 0; i < c->hashed.n; i++)
    if (fnmatch(c->hashed.v[i], path, 0) == 0) return true;
  return false;
}

/* rules_load: the table, parsed as static_rules_load reads it: a line is a glob and a Content-Type,
 * TAB-separated; blank and '#' lines are skipped; the last rule must be the fallback "*". The message
 * names the file by its repository path (the shell names it by its install path: the only difference). */
static void rules_load(void) {
  if (rules) return;
  const char *f = "tools/cbase.d/static_rules.tsv";
  char *copy = cbc_dup(static_rules_tsv);
  size_t cap = 0;
  for (char *line = copy; *line;) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    if (*line && *line != '#') {
      char *tab = strchr(line, '\t');
      char *g = line, *t = NULL, *extra = NULL;
      if (tab) {
        *tab = '\0';
        t = tab + 1;
        char *tab2 = strchr(t, '\t');
        if (tab2) {
          *tab2 = '\0';
          extra = tab2 + 1;
        }
      }
      if (!t || !*t || (extra && *extra)) {
        fprintf(stderr, "cbase: %s: the rule for '%s' is not two TAB-separated fields\n", f, g);
        exit(CBC_EXIT_FAIL);
      }
      if (rules_n == cap) {
        cap = cap ? cap * 2 : 32;
        rules = cbc_xrealloc(rules, cap * sizeof *rules);
      }
      rules[rules_n].glob = cbc_dup(g);
      rules[rules_n].type = cbc_dup(t);
      rules_n++;
    }
    if (!eol) break;
    line = eol + 1;
  }
  free(copy);
  if (rules_n == 0 || strcmp(rules[rules_n - 1].glob, "*") != 0) {
    fprintf(stderr, "cbase: %s does not end with the fallback rule \"*\"\n", f);
    exit(CBC_EXIT_FAIL);
  }
}

/* classify: the file's Content-Type (the first rule whose glob matches, ignoring case) and its
 * Cache-Control: html is always no-cache, a content-hashed file is a year, the rest are no-cache. */
static void classify(const char *path, const char **type, const char **cache) {
  rules_load();
  *type = "";
  for (size_t i = 0; i < rules_n; i++) {
    if (fnmatch(rules[i].glob, path, FNM_CASEFOLD) == 0) {
      *type = rules[i].type;
      break;
    }
  }
  *cache = CACHE_NOCACHE;
  if (strncmp(*type, "text/html", 9) != 0 && hashed(path)) *cache = CACHE_IMMUTABLE;
}

/* ---- the plan --------------------------------------------------------------------------------- */

static void group_push(cbc_static_plan *p, int phase, const char *type, const char *cache) {
  if (p->n == p->cap) {
    size_t cap = p->cap ? p->cap * 2 : 8;
    cbc_static_group *nv = cbc_xrealloc(p->v, cap * sizeof *nv);
    p->v = nv;
    p->cap = cap;
  }
  cbc_static_group *g = &p->v[p->n];
  g->phase = phase;
  g->type = cbc_dup(type);
  g->cache = cbc_dup(cache);
  g->count = 0;
  char *num = cbc_xmalloc(32);
  snprintf(num, 32, "g%zu", p->n);
  g->tree = cbc_fmt("%s%s%s", p->root, "/", num);
  free(num);
  p->n++;
}

void cbc_static_plan_make(cbc_static_plan *p, const char *dir, const cbc_strs *files, const char *tmp) {
  p->n = p->cap = 0;
  p->v = NULL;
  p->root = cbc_fmt("%s%s%s", tmp, "/", "static-tree");
  mkdir_p(p->root);
  for (size_t i = 0; i < files->n; i++) {
    const char *f = files->v[i];
    const char *type, *cache;
    classify(f, &type, &cache);
    int phase = strcmp(cache, CACHE_IMMUTABLE) == 0 ? 1 : (strncmp(type, "text/html", 9) == 0 ? 3 : 2);
    size_t g = p->n;
    for (size_t k = 0; k < p->n; k++) {
      if (p->v[k].phase == phase && strcmp(p->v[k].type, type) == 0 && strcmp(p->v[k].cache, cache) == 0) {
        g = k;
        break;
      }
    }
    if (g == p->n) group_push(p, phase, type, cache);
    p->v[g].count++;
    /* A link (ln) in the group's tree, its directories made first; a copy where no link can be made. */
    char *src = cbc_fmt("%s%s%s", dir, "/", f);
    char *dst = cbc_fmt("%s%s%s", p->v[g].tree, "/", f);
    char *slash = strrchr(dst, '/');
    *slash = '\0';
    mkdir_p(dst);
    *slash = '/';
    if (link(src, dst) != 0) copy_file(src, dst);
    cbc_tmp_note(dst);
    free(src);
    free(dst);
  }
}

void cbc_static_print_plan(const cbc_static_plan *p) {
  for (int phase = 1; phase <= 3; phase++) {
    for (size_t i = 0; i < p->n; i++) {
      const cbc_static_group *g = &p->v[i];
      if (g->phase != phase) continue;
      printf("  %4zu file%s  %s  %s\n", g->count, g->count == 1 ? " " : "s", g->type, g->cache);
    }
  }
}

void cbc_static_upload(const cbc_static_plan *p, const char *bucket, const char *region, const char *app) {
  size_t n = 0;
  char *dest = cbc_xmalloc(strlen(bucket) + strlen(app) + 16);
  sprintf(dest, "s3://%s/%s/", bucket, app);
  char *what_uploading = NULL;
  cbc_asprintf(&what_uploading, "uploading to %s", dest);
  for (int phase = 1; phase <= 3; phase++) {
    for (size_t i = 0; i < p->n; i++) {
      const cbc_static_group *g = &p->v[i];
      if (g->phase != phase) continue;
      char *argv_tree = g->tree;
      if (aws_call(NULL, "s3", "cp", argv_tree, dest, "--recursive", "--only-show-errors", "--region", region,
                   "--content-type", g->type, "--cache-control", g->cache, NULL) != 0) {
        aws_fail(what_uploading);
      }
      n += g->count;
    }
  }
  printf("uploaded %zu file%s to %s (html last)\n", n, n == 1 ? "" : "s", dest);
  free(dest);
}

void cbc_static_prune(const char *bucket, const char *region, const char *app, const cbc_strs *files, bool dry) {
  cbc_buf text = { 0, 0, 0 };
  char *prefix = cbc_fmt("%s%s%s", app, "/", "");
  char *what_listing = NULL;
  cbc_asprintf(&what_listing, "listing s3://%s/%s", bucket, prefix);
  if (aws_call(&text, "s3api", "list-objects-v2", "--bucket", bucket, "--prefix", prefix, "--region", region,
               "--query", "Contents[].Key", "--output", "text", NULL) != 0) {
    aws_fail(what_listing);
  }
  size_t n = 0, cap = 0;
  char **gone = NULL;   /* the keys to delete: each held to the folder BEFORE it joins (and a batch) */
  char *body = cbc_buf_text(&text);
  /* The listing: a page of keys a line, tab-separated (the CLI's text output; no key holds a tab). */
  for (char *line = body; *line;) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    for (char *k = line; k;) {
      char *tab = strchr(k, '\t');
      if (tab) *tab = '\0';
      if (*k && strcmp(k, "None") != 0) {
        /* THE FOLDER GUARD: whatever the listing holds, only a key under "<app>/" is ever deleted. */
        size_t pl = strlen(prefix);
        if (strncmp(k, prefix, pl) != 0 || k[pl] == '\0') {
          fprintf(stderr, "cbase: prune: s3://%s/%s is outside %s/, and is left alone\n", bucket, k, app);
        } else if (!strs_has(files, k + pl)) {
          if (dry) {
            printf("would prune s3://%s/%s\n", bucket, k);
          } else {
            if (n == cap) {
              cap = cap ? cap * 2 : 64;
              gone = cbc_xrealloc(gone, cap * sizeof *gone);
            }
            gone[n] = cbc_dup(k);
          }
          n++;
        }
      }
      k = tab ? tab + 1 : NULL;
    }
    line = eol ? eol + 1 : line + strlen(line);
  }
  if (!dry) {
    aws_s3_delete_keys(bucket, region, gone, n);   /* batched; prints one "pruned" line a key */
    for (size_t i = 0; i < n; i++) free(gone[i]);
    free(gone);
  }
  printf("%s %zu file%s the build no longer has, under s3://%s/%s/ alone\n", dry ? "would prune" : "pruned", n, n == 1 ? "" : "s", bucket, app);
  free(body);
  free(prefix);
  free(text.data);
}

void cbc_static_bucket(const char *static_bucket, const char *group, char **bucket, char **region) {
  if (static_bucket && *static_bucket) {
    *bucket = cbc_dup(static_bucket);
  } else {
    char *id = NULL;
    if (aws_group_find("static", group, &id) != 0) exit(CBC_EXIT_FAIL);
    *bucket = id;
  }
  char *what_region = NULL;
  cbc_asprintf(&what_region, "reading s3://%s's region", *bucket);
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "s3api", "get-bucket-location", "--bucket", *bucket, "--query", "LocationConstraint", "--output", "text", NULL) != 0) {
    aws_fail(what_region);
  }
  char *loc = cbc_buf_text(&out);
  if (strcmp(loc, "None") == 0 || *loc == '\0') *region = cbc_dup("us-east-1");
  else if (strcmp(loc, "EU") == 0) *region = cbc_dup("eu-west-1");
  else *region = loc;
  free(out.data);
}

bool cbc_static_policy_ok(const char *bucket, const char *account) {
  char *what_policy = NULL;
  cbc_asprintf(&what_policy, "reading s3://%s's policy", bucket);
  cbc_buf out = { 0, 0, 0 };
  if (aws_call(&out, "s3api", "get-bucket-policy", "--bucket", bucket, "--query", "Policy", "--output", "text", NULL) == 0) {
    char *pol = cbc_buf_text(&out);
    bool ok = strstr(pol, "cloudfront.amazonaws.com") && strstr(pol, "aws:SourceAccount") && strstr(pol, account);
    free(pol);
    free(out.data);
    if (ok) {
      printf("static bucket s3://%s: its policy lets the account's distributions read it\n", bucket);
      return true;
    }
  } else {
    free(out.data);
    if (strcmp(aws_code(), "NoSuchBucketPolicy") != 0) {
      aws_fail(what_policy);
    }
  }
  fprintf(stderr,
          "cbase: s3://%1$s's policy does not let this account's distributions read it, so the app's new distribution would answer 403.\n"
          "  Add this statement to the policy's Statement list, keeping every statement it has (S3 console: the bucket, Permissions, Bucket policy, Edit):\n"
          "    {\"Sid\": \"TheAccountsDistributionsRead\", \"Effect\": \"Allow\", \"Principal\": {\"Service\": \"cloudfront.amazonaws.com\"},\n"
          "     \"Action\": [\"s3:GetObject\", \"s3:ListBucket\"], \"Resource\": [\"arn:aws:s3:::%1$s\", \"arn:aws:s3:::%1$s/*\"],\n"
          "     \"Condition\": {\"StringEquals\": {\"aws:SourceAccount\": \"%2$s\"}}}\n"
          "  It lets any CloudFront distribution of account %2$s read the bucket (a cbase static group's own statement), and removes nothing.\n",
          bucket, account);
  return false;
}
