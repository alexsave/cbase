/* vm.h - the VM side of a deploy and an undeploy (tools/cbase.d/vm.sh): the paths, the units, the
 * memory fit and the server build, as the shell does them. The SSM and AWS steps are not here yet
 * (they need aws.sh, state.sh and stack.sh: see vm.c). Every name and every message is the shell's.
 */
#ifndef CBC_VM_H
#define CBC_VM_H

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

/* cbc_read_file: the whole of PATH, or the tool stops ("cbase: cannot read PATH"). */
char *cbc_read_file(const char *path);

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

#endif
