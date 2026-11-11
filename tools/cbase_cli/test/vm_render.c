/* vm_render.c - the differential harness for the VM renders (vm_render_test.sh): loads a cbase.conf
 * the way the tool does, renders one of the VM texts and prints it to stdout. The shell's
 * functions render the same texts from the same file, and the test compares them byte for byte.
 *
 *   vm_render CONF unit PORT REGION STATE_BUCKET
 *   vm_render CONF socket PORT
 *   vm_render CONF deploy PORT BUCKET KEY SHA REGION STATE_BUCKET
 *   vm_render CONF undeploy 0|1
 *   vm_render CONF data
 *   vm_render CONF boot
 *   vm_render CONF artifacts ACCOUNT REGION STATE_BUCKET
 *   vm_render CONF fit TYPE MIB
 *   vm_render CONF b64 TEXT
 */
#include "../cli.h"
#include "../conf.h"
#include "../vm.h"
#include "../vm_boot.h"
#include "../vm_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void need(int argc, int n, const char *what) {
  if (argc < n) cbc_fail(CBC_EXIT_USAGE, "vm_render: %s wants more arguments", what);
}

int main(int argc, char **argv) {
  if (argc < 3) cbc_fail(CBC_EXIT_USAGE, "vm_render CONF MODE ARGS...");
  cbc_conf_load(argv[1]);
  const char *mode = argv[2];
  char *out = NULL;
  if (strcmp(mode, "unit") == 0) {
    need(argc, 6, mode);
    out = cbc_vm_unit(argv[3], argv[4], cbc_vm_backup_bucket(argv[5]));
  } else if (strcmp(mode, "socket") == 0) {
    need(argc, 4, mode);
    out = cbc_vm_socket(argv[3]);
  } else if (strcmp(mode, "deploy") == 0) {
    need(argc, 9, mode);
    out = cbc_vm_remote_deploy(argv[3], argv[4], argv[5], argv[6], argv[7], argv[8]);
  } else if (strcmp(mode, "undeploy") == 0) {
    need(argc, 4, mode);
    out = cbc_vm_remote_undeploy(strcmp(argv[3], "1") == 0);
  } else if (strcmp(mode, "data") == 0) {
    out = cbc_vm_remote_data();
  } else if (strcmp(mode, "boot") == 0) {
    out = cbc_vm_boot_userdata();
  } else if (strcmp(mode, "artifacts") == 0) {
    need(argc, 6, mode);
    char *bucket = NULL, *prefix = NULL;
    cbc_vm_artifacts(argv[3], argv[4], argv[5], &bucket, &prefix);
    printf("%s %s", bucket, prefix);
    return 0;
  } else if (strcmp(mode, "b64") == 0) {
    need(argc, 4, mode);
    fputs(cbc_vm_b64(argv[3]), stdout);
    return 0;
  } else if (strcmp(mode, "fit") == 0) {
    need(argc, 5, mode);
    long long limit = 0;
    char *what = NULL;
    cbc_vm_fit(argv[3], atoll(argv[4]), &limit, &what);
    printf("%lld\n%s\n", limit, what);
    return 0;
  } else {
    cbc_fail(CBC_EXIT_USAGE, "vm_render: no mode %s", mode);
  }
  fputs(out, stdout);
  return 0;
}
