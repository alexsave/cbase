/* vm_remote.h - the scripts the tool sends to a VM through SSM (tools/cbase.d/vm.sh's
 * vm_remote_deploy, vm_remote_undeploy and vm_remote_data). Each returns the script's text; the
 * script runs on the instance as bash, as root, and is the same bytes the shell renders.
 */
#ifndef CBC_VM_REMOTE_H
#define CBC_VM_REMOTE_H

#include <stdbool.h>

/* cbc_vm_remote_deploy: the deploy script of the app in cbc_conf on the VM, on PORT, with the
 * artifact at BUCKET/KEY (sha256 SHA), REGION, and STATE_BUCKET (the backup bucket's default): the
 * values in its header, then tools/cbase.d/vm_remote.sh with its unit (cbc_vm_unit) and socket
 * (cbc_vm_socket) spliced in. */
char *cbc_vm_remote_deploy(const char *port, const char *bucket, const char *key, const char *sha, const char *region,
                           const char *state_bucket);

/* cbc_vm_remote_undeploy: the script that stops and removes the app's service; DELETE_DATA also
 * deletes its folder and user. */
char *cbc_vm_remote_undeploy(bool delete_data);

/* cbc_vm_remote_data: the script that names each app whose data is on the instance (group-destroy). */
char *cbc_vm_remote_data(void);

#endif
