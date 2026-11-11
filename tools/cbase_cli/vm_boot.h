/* vm_boot.h - the group's boot script (tools/cbase.d/vm_boot.sh), the instance's user-data. The
 * script itself stays bash: it runs on the VM at every boot. The tool only reads it, to put its
 * code into the group's template (group.sh's T_BOOT).
 */
#ifndef CBC_VM_BOOT_H
#define CBC_VM_BOOT_H

/* cbc_vm_boot_userdata: vm_boot.sh without its comment and blank lines, the shebang kept (awk
 * 'NR == 1 || (!/^[[:space:]]*#/ && !/^$/)'). The template step (the ${ escapes, the indent and the
 * origin's address) is the group's. */
char *cbc_vm_boot_userdata(void);

#endif
