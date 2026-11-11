/* vm.h - the VM side of a deploy and an undeploy (tools/cbase.d/vm.sh): the paths, the units, the
 * memory fit and the server build, as the shell does them. The SSM and AWS steps are not here yet
 * (they need aws.sh, state.sh and stack.sh: see vm.c). Every name and every message is the shell's.
 */
#ifndef CBC_VM_H
#define CBC_VM_H

#include "conf.h"

#include <stdbool.h>
#include <stddef.h>

/* MiB an instance keeps for itself: the kernel, systemd, journald and the SSM agent. */
#define CBC_VM_OS_RESERVE 192
/* The server's name in the build's dist/ directory (cbase.mk's serve-binary). */
#define CBC_VM_ARTIFACT_NAME "cbase-serve-linux-arm64"

/* cbc_sbuf: a growing NUL-terminated string (d is NULL until something is added). */
typedef struct {
  char *d;
  size_t n, cap;
} cbc_sbuf;

/* cbc_sb_add, cbc_sb_str: append N bytes, or a string, to B. */
void cbc_sb_add(cbc_sbuf *b, const char *p, size_t n);
void cbc_sb_str(cbc_sbuf *b, const char *s);

/* cbc_vm_read: the whole of PATH (cli.h's cbc_read_file), or the tool stops ("cbase: cannot read PATH"). */
char *cbc_vm_read(const char *path);

/* cbc_tools_dir: the directory that holds cbase.d/ (tools/), fixed when the tool is built
 * (the Makefile's CBC_TOOLS_DIR). */
const char *cbc_tools_dir(void);

/* cbc_vm_file: the path of a file under cbase.d/ (tools/cbase.d/NAME), a new string. */
char *cbc_vm_file(const char *name);

/* cbc_vm_settings: the SSM path of the app's settings, ending in '/' (vm_settings). */
char *cbc_vm_settings(void);

/* cbc_vm_data_dir: the app's data directory on the VM (vm_data_dir). */
char *cbc_vm_data_dir(void);

/* cbc_vm_artifacts: where the app's servers go, as bucket and prefix (the prefix ends in '/'),
 * from cbase.conf, else the account's code bucket in an adopted group, else the state bucket
 * (vm_artifacts). ACCOUNT and REGION are the CLI's; STATE_BUCKET is state.sh's. */
void cbc_vm_artifacts(const char *account, const char *region, const char *state_bucket, char **bucket, char **prefix);

/* cbc_vm_backup_bucket: where serve puts the app's backups (vm_backup_bucket). */
char *cbc_vm_backup_bucket(const char *state_bucket);

/* cbc_vm_unit: the app's service unit for PORT (vm_unit): REGION and BACKUP_BUCKET are the
 * group's region and the backup bucket. */
char *cbc_vm_unit(const char *port, const char *region, const char *backup_bucket);

/* cbc_vm_socket: the app's socket unit for PORT (vm_socket). */
char *cbc_vm_socket(const char *port);

/* cbc_vm_fit: the fit check's limit for an instance of TYPE with MIB memory (vm_fit_set): MIB less
 * the OS's reserve, never below 0, and the sentence the report names it by. */
void cbc_vm_fit(const char *type, long long mib, long long *limit, char **what);

/* cbc_vm_mib_text: the instance type's memory from what describe-instance-types printed for
 * TYPE (vm_type_mib). Stops the tool when AWS named no size. */
long long cbc_vm_mib_text(const char *type, const char *text);

/* cbc_vm_b64: TEXT as one line of standard base64 with '=' padding and no newline, the shell's
 * `base64 | tr -d '\n'` (how a script travels to the instance in an SSM command). */
char *cbc_vm_b64(const char *text);

/* cbc_vm_sha256: the lower-case hex sha256 of the file at PATH (vm_sha256), or NULL when it
 * could not be read. */
char *cbc_vm_sha256(const char *path);

/* cbc_vm_check_elf: stops the tool with the shell's message unless PATH is a 64-bit little-endian
 * Linux ELF binary for aarch64 (vm_build's check). */
void cbc_vm_check_elf(const char *path);

/* The server vm_build made or was given: its path and its sha256. */
typedef struct {
  char *bin;
  char *sha;
} cbc_vm_artifact;

/* cbc_vm_build: the linux-arm64 server, built by `make serve-binary` in . or server/, or named by
 * CBASE_SERVE_BIN (vm_build). Stops the tool on any failure, as the shell does. */
void cbc_vm_build(cbc_vm_artifact *out);


/* The VM this run deploys to or undeploys from (vm.sh's VM_* variables). */
typedef struct {
  char *id;             /* VM_ID: the instance */
  char *dns;            /* VM_DNS: its public DNS name, or the group's origin */
  char *origin_addr;    /* VM_ORIGIN_ADDR: ipv6 or dualstack, "" for none */
  char *type;           /* VM_TYPE */
  long long mib;        /* VM_MIB */
  char *bin;            /* VM_BIN: the server file */
  char *sha;            /* VM_SHA */
  char *key;            /* VM_KEY: the artifact's key in its bucket */
  char *port;           /* VM_PORT: the port the app holds */
  bool new_member;      /* VM_NEW: the join added the app */
  char *origin_secret;  /* VM_ORIGIN_SECRET: never printed */
  char *ssm_status;     /* VM_SSM_STATUS: Success, Failed, ... */
  char *ssm_code;       /* VM_SSM_CODE */
} cbc_vm_t;
extern cbc_vm_t cbc_vm;

/* cbc_vm_find: VM_ID, VM_DNS, VM_TYPE and VM_MIB for the group's running arm64 instance (vm_find). */
void cbc_vm_find(const char *group);

/* cbc_vm_origin_secret: VM_ORIGIN_SECRET from SSM, masked (vm_origin_secret). */
void cbc_vm_origin_secret(void);

/* cbc_vm_fit_set: the fit check's limit for the join (vm_fit_set). */
void cbc_vm_fit_set(void);

/* cbc_vm_join: the app joins the group's state file (vm_join). */
void cbc_vm_join(const char *group);

/* cbc_vm_ship: the artifact uploaded and the service started or restarted (vm_ship). False when the
 * service was not started. */
bool cbc_vm_ship(const char *group);

/* cbc_vm_undeploy: the service stopped and removed, then the membership left (vm_undeploy). */
void cbc_vm_undeploy(const char *group, bool delete_data);

/* cbc_vm_origin: the group's origin, read before anything is changed (vm_origin): sets dns and
 * origin_addr. */
void cbc_vm_origin(const char *group);

/* cbc_vm_plan_dry: what a deploy would claim on the group, from the state file, writing nothing
 * (vm_plan_dry). Sets cbc_vm.port. Exits 1 when the app would not fit. */
void cbc_vm_plan_dry(const char *group);

/* ENTRY POINTS for deploy, undeploy and group-destroy (see the comment above them in vm.c for the
 * order). cbc_vm_deploy_find: deploy step 1 for a VM group (vm_find, the paths line, vm_origin, the
 * adopted group's state bucket, vm_origin_secret). */
void cbc_vm_deploy_find(const char *group);

/* cbc_vm_deploy_build: deploy step 3 (vm_build): sets cbc_vm.bin and cbc_vm.sha. Not on a dry run. */
void cbc_vm_deploy_build(void);

/* cbc_vm_deploy_join: deploy step 5 (vm_join, or vm_plan_dry when DRY is true). */
void cbc_vm_deploy_join(const char *group, bool dry);

/* cbc_vm_data_check: group-destroy's check of which apps' data is on INSTANCE: fills APPS and returns
 * whether the SSM command ended Success (cbc_vm.ssm_status). */
bool cbc_vm_data_check(const char *instance, const char *group, cbc_strs *apps);

#endif
