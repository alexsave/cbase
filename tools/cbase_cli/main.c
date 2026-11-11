/* main.c - the deploy tool's entry: option and command dispatch, in the order tools/cbase checks them
 * (README.md "./cbase deploy tool", tools/cbase's header). Every usage error is checked before any
 * setting is read or any AWS call is made, as the shell does it.
 *
 * Milestone 1 (this file's state): the dispatch, the usage text and every argument error are the
 * shell's. The commands whose work is not ported yet stop with status 3 and a message that says so;
 * the shell tools/cbase is still the default and still does them all.
 */
#include "aws.h"
#include "cli.h"
#include "conf.h"
#include "deploy.h"
#include "group.h"
#include "stack.h"
#include "state.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

/* conf_groups (conf.sh's, tools/cbase): the groups cbase.conf names, the VM first. */
static int conf_groups(const char *kinds[2], const char *names[2]) {
  int n = 0;
  if (strcmp(cbc_conf.vm, "none") != 0) {
    kinds[n] = "vm";
    names[n] = cbc_conf.vm;
    n++;
  }
  if (strcmp(cbc_conf.stat, "none") != 0) {
    kinds[n] = "static";
    names[n] = cbc_conf.stat;
    n++;
  }
  return n;
}

static const char *conf_file = "cbase.conf";

/* file_exists_in: whether DIR/NAME is a regular file (the shell's [ -f ]). */
static bool file_exists_in(const char *dir, const char *name) {
  char *path = cbc_fmt("%s/%s", dir, name);
  bool file = cbc_is_file(path);
  free(path);
  return file;
}

/* arg_error: an argument the shell refuses (its "echo ... >&2; exit 2"): the message, no usage. */
static void arg_error(const char *msg) __attribute__((noreturn));
static void arg_error(const char *msg) { cbc_fail(CBC_EXIT_USAGE, "%s", msg); }

static void usage_block_then_exit(void) __attribute__((noreturn));
static void usage_block_then_exit(void) {
  cbc_usage(stderr);
  exit(CBC_EXIT_USAGE);
}

static bool is_one_of(const char *s, const char *a, const char *b) { return strcmp(s, a) == 0 || strcmp(s, b) == 0; }

int main(int argc, char **argv) {
  cbc_flags_from_env();
  int i = 1;
  while (i < argc) {
    const char *a = argv[i];
    if (strcmp(a, "-f") == 0) {
      if (i + 1 >= argc) cbc_fail(CBC_EXIT_USAGE, "-f wants a file");
      conf_file = argv[i + 1];
      i += 2;
    } else if (is_one_of(a, "-h", "--help") || strcmp(a, "help") == 0) {
      cbc_usage(stdout);
      return 0;
    } else if (a[0] == '-') {
      fprintf(stderr, "cbase: unknown option %s\n", a);
      usage_block_then_exit();
    } else {
      break;
    }
  }
  if (i >= argc) usage_block_then_exit();

  const char *cmd = argv[i];
  int nargs = argc - i - 1;
  char **args = argv + i + 1;

  if (strcmp(cmd, "config") == 0) {
    if (nargs != 1 || strcmp(args[0], "check") != 0)
      arg_error("config check is the one config command");
    cbc_conf_load(conf_file);
    printf("%s: ok\n", conf_file);
    cbc_conf_print();
    return 0;
  }
  if (strcmp(cmd, "doctor") == 0) {
    if (nargs != 0) arg_error("doctor takes no arguments");
    cbc_conf_load(conf_file);
    printf("%s: ok\n", conf_file);
    cbc_conf_print();
    aws_init();
    printf("account   %s (%s)\n", aws_account, aws_region());
    int rc = 0;
    const char *kinds[2], *names[2];
    int n = conf_groups(kinds, names);
    for (int g = 0; g < n; g++) {
      char *id = NULL;
      if (aws_group_find(kinds[g], names[g], &id) != 0) {
        rc = 1;
        continue;
      }
      printf("%s group %s: %s\n", kinds[g], names[g], id);
      int lrc = state_list(kinds[g], names[g]);
      if (lrc == 3) printf("%s group %s: no state file yet (no app has joined)\n", kinds[g], names[g]);
    }
    return rc;
  }
  if (strcmp(cmd, "apps") == 0) {
    if (nargs > 1) arg_error("apps takes one group name at most");
    if (nargs == 1 && !cbc_conf_name_ok(args[0])) {
      fprintf(stderr, "cbase: %s is not a group name\n", args[0]);
      exit(CBC_EXIT_USAGE);
    }
    const char *kinds[2], *names[2];
    int n;
    if (nargs == 1) {
      kinds[0] = "vm";
      names[0] = args[0];
      kinds[1] = "static";
      names[1] = args[0];
      n = 2;
    } else {
      cbc_conf_load(conf_file);
      n = conf_groups(kinds, names);
    }
    aws_init();
    bool found = false;
    for (int g = 0; g < n; g++) {
      int rc = state_list(kinds[g], names[g]);
      if (rc == 0) found = true;
      else if (rc != 3) exit(rc);
    }
    if (!found) {
      char who[512] = "";
      for (int g = 0; g < n; g++) {
        char piece[256];
        snprintf(piece, sizeof piece, "%s%s %s group", g ? ", " : "", kinds[g], names[g]);
        strncat(who, piece, sizeof who - strlen(who) - 1);
      }
      cbc_fail(CBC_EXIT_FAIL, "no state file for %s: no app has joined it yet", who);
    }
    return 0;
  }
  if (strcmp(cmd, "state") == 0) {
    const char *op = nargs > 0 ? args[0] : "";
    if (strcmp(op, "join") != 0 && strcmp(op, "leave") != 0) arg_error("state join or state leave");
    if (nargs != 1) {
      fprintf(stderr, "cbase: state %s takes no more arguments\n", op);
      exit(CBC_EXIT_USAGE);
    }
    cbc_conf_load(conf_file);
    aws_init();
    const char *kinds[2], *names[2];
    int n = conf_groups(kinds, names);
    for (int g = 0; g < n; g++) {
      char *id = NULL;
      if (aws_group_find(kinds[g], names[g], &id) != 0) exit(CBC_EXIT_FAIL);
      state_origin_in = cbc_conf.vm_origin;
      state_change(kinds[g], names[g], cbc_conf.app, cbc_conf_mem(), op);
    }
    return 0;
  }
  if (strcmp(cmd, "template") == 0) {
    if (nargs != 0) arg_error("template takes no arguments");
    cbc_conf_load(conf_file);
    /* Placeholders where deploy puts what it finds: the static group's bucket and region, and the
     * certificate (found or requested). */
    char bucket[256], prefix[256];
    snprintf(bucket, sizeof bucket, "%s", *cbc_conf.static_bucket ? cbc_conf.static_bucket : "");
    if (!*cbc_conf.static_bucket) snprintf(bucket, sizeof bucket, "%s-bucket", cbc_conf.stat);
    snprintf(prefix, sizeof prefix, "%s", *cbc_conf.static_prefix ? cbc_conf.static_prefix : cbc_conf.app);
    stk_bucket = cbc_dup(bucket);
    stk_prefix = cbc_dup(prefix);
    stk_bucket_region = "us-east-1";
    if (strcmp(cbc_conf.stat, "none") != 0 && file_exists_in(cbc_conf.static_dir, "index.html")) {
      /* The router's page list is the build's, when there is one to read. */
      cbc_strs files = { 0, 0, 0 };
      cbc_deploy_scan(cbc_conf.static_dir, &files);
    }
    if (*cbc_conf.domain) {
      stk_cert = *cbc_conf.cert_arn ? cbc_conf.cert_arn
                                   : "arn:aws:acm:us-east-1:111122223333:certificate/00000000-0000-0000-0000-000000000000";
      stk_api_cert = *cbc_conf.api_cert_arn ? cbc_conf.api_cert_arn : stk_cert;
    }
    char *file = cbc_tmp_path("stack.yaml");
    stack_render(file);
    printf("# PLACEHOLDERS: the bucket (%s) is static group %s's%s.\n", bucket, cbc_conf.stat,
           *cbc_conf.domain ? ", the certificate is the one deploy finds or requests" : "");
    char *text;
    size_t len;
    if (!cbc_read_file(file, &text, &len)) cbc_fail(CBC_EXIT_FAIL, "cannot read %s", file);
    fwrite(text, 1, len, stdout);
    return 0;
  }
  if (strcmp(cmd, "group-template") == 0) {
    const char *kind = nargs > 0 ? args[0] : "";
    const char *group = nargs > 1 ? args[1] : "";
    if (!is_one_of(kind, "vm", "static"))
      arg_error("group-template vm GROUP or group-template static GROUP");
    int n = nargs;
    if (strcmp(kind, "vm") == 0 && n == 3 && strcmp(args[2], "--ipv4") == 0) n = 2;
    if (!cbc_conf_name_ok(group) || n != 2) {
      cbc_fail(CBC_EXIT_USAGE, "group-template %s wants one group name%s", kind,
               strcmp(kind, "vm") == 0 ? ", and --ipv4 or nothing" : "");
    }
    cbc_group_template(kind, group, nargs == 3);
    return 0;
  }
  if (strcmp(cmd, "deploy") == 0) {
    for (int k = 0; k < nargs; k++) {
      const char *a = args[k];
      if (!is_one_of(a, "--dry-run", "--accept-drift") && strcmp(a, "--prune") != 0 &&
          strcmp(a, "--allow-downtime") != 0 && strcmp(a, "--no-aliases") != 0 && strcmp(a, "--attach") != 0)
        cbc_fail(CBC_EXIT_USAGE, "deploy takes --dry-run, --accept-drift, --prune, --allow-downtime, --no-aliases and --attach, not %s", a);
    }
    cbc_deploy(conf_file, nargs, args);
    return 0;
  }
  if (strcmp(cmd, "undeploy") == 0) {
    for (int k = 0; k < nargs; k++) {
      const char *a = args[k];
      if (!is_one_of(a, "--delete-files", "--delete-data"))
        cbc_fail(CBC_EXIT_USAGE, "undeploy takes --delete-files and --delete-data, not %s", a);
    }
    cbc_undeploy(conf_file, nargs, args);
    return 0;
  }
  if (strcmp(cmd, "group-init") == 0 || strcmp(cmd, "group-destroy") == 0) {
    const char *kind = nargs > 0 ? args[0] : "";
    const char *group = nargs > 1 ? args[1] : "";
    if (!is_one_of(kind, "vm", "static")) cbc_fail(CBC_EXIT_USAGE, "%s vm GROUP or %s static GROUP", cmd, cmd);
    if (!cbc_conf_name_ok(group)) {
      char shown[41];
      snprintf(shown, sizeof shown, "%s", group);
      cbc_fail(CBC_EXIT_USAGE, "%s %s wants a group name (a lower-case letter, then lower-case letters, digits and '-', at most 32 characters), not \"%s\"",
               cmd, kind, shown);
    }
    cbc_group_opts o = { 0 };
    cbc_group_args(cmd, kind, nargs - 2, args + 2, &o);
    aws_init();
    if (strcmp(cmd, "group-init") == 0) cbc_group_init(kind, group, &o);
    else cbc_group_destroy(kind, group, &o);
    return 0;
  }
  fprintf(stderr, "cbase: unknown command %s\n", cmd);
  usage_block_then_exit();
}
