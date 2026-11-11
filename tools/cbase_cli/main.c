/* main.c - the deploy tool's entry: option and command dispatch, in the order tools/cbase checks them
 * (README.md "./cbase deploy tool", tools/cbase's header). Every usage error is checked before any
 * setting is read or any AWS call is made, as the shell does it.
 *
 * Milestone 1 (this file's state): the dispatch, the usage text and every argument error are the
 * shell's. The commands whose work is not ported yet stop with status 3 and a message that says so;
 * the shell tools/cbase is still the default and still does them all.
 */
#include "cli.h"
#include "conf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *conf_file = "cbase.conf";

/* not_ported: the command is not in the C tool yet. Status 3 is the C tool's own, never the shell's. */
static void not_ported(const char *cmd) __attribute__((noreturn));
static void not_ported(const char *cmd) {
  cbc_fail(3, "%s is not in the C tool yet: run it with the shell tool (tools/cbase %s)", cmd, cmd);
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
    not_ported(cmd);
  }
  if (strcmp(cmd, "apps") == 0) {
    if (nargs > 1) arg_error("apps takes one group name at most");
    if (nargs == 1 && !cbc_conf_name_ok(args[0])) {
      fprintf(stderr, "cbase: %s is not a group name\n", args[0]);
      exit(CBC_EXIT_USAGE);
    }
    not_ported(cmd);
  }
  if (strcmp(cmd, "state") == 0) {
    const char *op = nargs > 0 ? args[0] : "";
    if (strcmp(op, "join") != 0 && strcmp(op, "leave") != 0) arg_error("state join or state leave");
    if (nargs != 1) {
      fprintf(stderr, "cbase: state %s takes no more arguments\n", op);
      exit(CBC_EXIT_USAGE);
    }
    not_ported(cmd);
  }
  if (strcmp(cmd, "template") == 0) {
    if (nargs != 0) arg_error("template takes no arguments");
    not_ported(cmd);
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
    not_ported(cmd);
  }
  if (strcmp(cmd, "deploy") == 0) {
    for (int k = 0; k < nargs; k++) {
      const char *a = args[k];
      if (!is_one_of(a, "--dry-run", "--accept-drift") && strcmp(a, "--prune") != 0 &&
          strcmp(a, "--allow-downtime") != 0 && strcmp(a, "--no-aliases") != 0 && strcmp(a, "--attach") != 0)
        cbc_fail(CBC_EXIT_USAGE, "deploy takes --dry-run, --accept-drift, --prune, --allow-downtime, --no-aliases and --attach, not %s", a);
    }
    not_ported(cmd);
  }
  if (strcmp(cmd, "undeploy") == 0) {
    for (int k = 0; k < nargs; k++) {
      const char *a = args[k];
      if (!is_one_of(a, "--delete-files", "--delete-data"))
        cbc_fail(CBC_EXIT_USAGE, "undeploy takes --delete-files and --delete-data, not %s", a);
    }
    not_ported(cmd);
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
    not_ported(cmd);
  }
  fprintf(stderr, "cbase: unknown command %s\n", cmd);
  usage_block_then_exit();
}
