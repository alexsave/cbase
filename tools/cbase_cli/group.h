/* group.h - a group's own CloudFormation stack: group-init and group-destroy, and the group-template
 * render (group.sh; README.md "./cbase deploy tool"). One stack per group in the CLI's region: a VM
 * group's instance (cbase-group-vm-<group>), or a static group's bucket (cbase-group-static-<group>),
 * and the account's state bucket and the group's state file. A VM costs money, so its change set runs
 * only with --yes or a "yes" typed at a terminal.
 */
#ifndef CBC_GROUP_H
#define CBC_GROUP_H

#include <stdbool.h>
#include <stdio.h>

/* The flags group-init and group-destroy take (group_args). TYPE is "" when none was given. */
typedef struct {
  bool yes;
  char *type;
  bool ipv4;
  bool delete_data;
} cbc_group_opts;

/* cbc_group_args: CMD's flags (ARGV[0..ARGC)) after KIND and GROUP, as group_args reads them. Stops the
 * tool (exit 2) on a flag the command does not take, with the shell's message. */
void cbc_group_args(const char *cmd, const char *kind, int argc, char **argv, cbc_group_opts *o);

/* cbc_group_init: makes or updates the group's stack (a VM's instance, or a static group's bucket), the
 * state bucket when the account has none, and the group's empty state file. Stops the tool on a refusal,
 * having changed nothing when the refusal is before the change set runs. */
void cbc_group_init(const char *kind, const char *group, const cbc_group_opts *o);

/* cbc_group_destroy: deletes an empty group's stack (refused while it has members, or a VM holds data
 * unless DELETE_DATA), and its state file. */
void cbc_group_destroy(const char *kind, const char *group, const cbc_group_opts *o);

/* cbc_group_render: the group's template for KIND (group.sh's group_render): its T_ values set, the boot
 * script for a VM group, the template written to F; IPV4 is group-init --ipv4's flag. */
void cbc_group_render(const char *kind, const char *group, FILE *f, bool ipv4);

/* cbc_group_template: a group's template as group-init would render it, to stdout (no AWS). */
void cbc_group_template(const char *kind, const char *group, bool ipv4);

/* cbc_group_state_bucket_found: whether the account has its state bucket (cbase-state-<account>). Stops
 * the tool on a failed read. */
bool cbc_group_state_bucket_found(void);

/* cbc_group_state_bucket_adopted: deploy's, for an adopted VM group (CBASE_VM_STACK), which no group-init
 * made: the state bucket made when the account has none (private, versioned, public access blocked).
 * With DRY, makes nothing and says what a deploy would make. Returns true when the state bucket does not
 * exist (a dry run: the caller reads no state file, as STATE_NO_BUCKET does). */
bool cbc_group_state_bucket_adopted(const char *region, const char *group, bool dry, const char *app,
                                    const char *vm_port, const char *vm_data);

#endif
