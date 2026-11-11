/* state.h - who is in a group: the state file in S3 (state.sh; README.md "./cbase deploy tool"). Its
 * format, its conditional write and its memory fit are state.sh's, rule for rule.
 */
#ifndef CBC_STATE_H
#define CBC_STATE_H

#include <stdbool.h>

/* The joining app's CBASE_VM_ORIGIN, or "" (the state command sets it from cbase.conf). */
extern char *state_origin_in;
/* true when a join with no origin recorded or named is refused (deploy's vm step sets it). */
extern bool state_origin_need;
/* The MiB the vm group's instance leaves for apps, or -1 for no fit check (vm.sh sets it), and how the
 * limit came about, for the report. */
extern long state_fit_limit;
extern char *state_fit_what;
/* A grandfathered member's own port and data directory, or "" (deploy's vm step sets them). */
extern char *state_legacy_port;
extern char *state_legacy_data;
/* STATE_PORT: the port the last vm join holds (set by state_port_of's caller, state_change). */
extern char *state_port;
/* true when the last join added the app (it was not a member). */
extern bool state_joined_new;

/* state_bucket: the state bucket's name, cbase-state-<account>. */
char *state_bucket(void);

/* state_key: the state file's key in the bucket, <kind>/<group>.txt. */
char *state_key(const char *kind, const char *group);

/* state_read: the group's members and its ETag, read from S3 (the ETag is "*" when there is no file,
 * and then there are no members). Stops the tool on a corrupt file or a failed read. */
void state_read(const char *kind, const char *group);

/* state_origin: whether the group's VM has an origin, from its state file or from the joining app's
 * origin (ipv6). Stops the tool when the app names another origin than the one the VM is reached at. */
bool state_origin(const char *group);

/* state_at: the index of APP among the members as state_read left them, or -1. */
long state_at(const char *app);

/* state_port_of: the port a join of the member at AT (-1: a new member) holds: a grandfathered member's
 * own (LEGACY, when set), the one it holds, or the lowest free one in 8700-8799. Stops the tool when every
 * one is held. */
const char *state_port_of(const char *group, const char *legacy, long at);

/* state_vm_line: a vm member's line, "app=<a> port=<p> service=cbase-<a>.service mem=<m>", with
 * " data=<dir> legacy=1" after it when LEGACY_DATA is set. Returns a new string. */
char *state_vm_line(const char *app, const char *port, const char *mem, const char *legacy_data);

/* state_fit: whether the members, with APP's line made LINE, fit in state_fit_limit MiB. Fills
 * state_fit_report (kept in state.c) for the message. */
bool state_fit(const char *app, const char *line);

/* state_list: the group's members as a table, on stdout. Returns 3 when the group has no state file yet
 * (nothing printed for it), 0 otherwise. Stops the tool on a corrupt file or a failed read. */
int state_list(const char *kind, const char *group);

/* state_change: one join or leave (OP "join" or "leave") of APP with MEM as its declared memory, read,
 * changed and conditionally written, retried on a lost race. Prints what it did. Stops the tool on a
 * refusal, having changed nothing. */
void state_change(const char *kind, const char *group, const char *app, const char *mem, const char *op);

#endif
