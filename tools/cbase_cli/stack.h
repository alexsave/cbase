/* stack.h - the tool's CloudFormation stacks and the one change-set flow every one of them goes through
 * (stack.sh; README.md "./cbase deploy tool"). Templates are plain CloudFormation: template_render fills
 * a file's __NAME__ values and keeps its "#if FLAG" blocks.
 *
 * The shell's STACK_* variables are stk_* here: the shell's function names are kept, and stack_status
 * is both a function and STACK_STATUS's value, which C cannot name twice.
 */
#ifndef CBC_STACK_H
#define CBC_STACK_H

#include <stdbool.h>
#include <stdio.h>

/* The stack the next change set or delete is on: its name, region and tag ("Key=K,Value=V"), and its
 * capabilities ("" or CAPABILITY_IAM). stack_use sets them; stack_params_clear and stack_param_add
 * manage the parameters ("ParameterKey=K,ParameterValue=V" each). */
extern char *stk_name;
extern char *stk_region;
extern char *stk_tag;
extern char *stk_caps;
extern bool stk_app;           /* an app's own stack, whose redeploys the downtime guard holds */
extern char *stk_template;     /* the rendered template's file */
extern char *stk_status;       /* before this run: NONE, or the stack's status (stack_status sets it) */
extern char *stk_type;         /* CREATE or UPDATE */
extern char *stk_cs;           /* the change set's name, cbase-deploy */
extern char *stk_changes;      /* "1" when the change set holds changes */
extern char *stk_cert;         /* the distribution's certificate ARN, or "" */
extern char *stk_api_cert;     /* CBASE_API_HOST's distribution's certificate ARN, or "" */
extern char *stk_bucket;       /* the app's bucket, its region and its folder (static.sh) */
extern char *stk_bucket_region;
extern char *stk_prefix;
extern bool stk_no_rollback;   /* an attach's update is not rolled back */
extern char *stk_files;        /* ST_FILES: the static dir's file list (static_scan), "" when none */

/* stack_use: the stack the next change set or delete is on, with no parameters yet. */
void stack_use(const char *name, const char *region, const char *tag);

/* stack_app_use: the app's own stack, cbase-app-<CONF_APP> in us-east-1. */
void stack_app_use(void);

/* stack_params_clear / stack_param_add: the stack's parameters. */
void stack_params_clear(void);
void stack_param_add(const char *key, const char *value);

/* stack_param_previous: the parameter KEY keeps the value the stack holds (UsePreviousValue=true). */
void stack_param_previous(const char *key);

/* stack_plan: the app's template rendered into stk_template, then its change set (stack_changeset). */
void stack_plan(void);

/* stack_status: reads the stack's status into stk_status (NONE when there is no stack). Stops on a
 * failed read other than a missing stack. */
void stack_status(void);

/* stack_output: one of the stack's outputs, "" when none. Stops the tool on a failed read. */
char *stack_output(const char *key);

/* stack_changeset: THE CHANGE-SET FLOW for stk_name from stk_template: the drift check, the change set
 * printed, the hard stops (a bucket or an instance replaced, a downtime). Sets stk_changes; a dry run's
 * change set is deleted here. Stops the tool on a refusal, having changed nothing. */
void stack_changeset(void);

/* stack_execute: runs the change set and waits for the stack to finish. Stops the tool on failure. */
void stack_execute(void);

/* stack_discard: deletes the change set, and the empty stack a first change set made. */
void stack_discard(void);

/* stack_delete: deletes the stack and waits; nothing when there is none. */
void stack_delete(void);

/* template_clear / template_set: the values template_render replaces each __NAME__ with (the shell's
 * T_NAME variables, named without the T_). A name not set is replaced by "". */
void template_clear(void);
void template_set(const char *name, const char *value);

/* stk_addr: the VM origin's IP address type, ipv6 or dualstack, or "" (the shell's VM_ORIGIN_ADDR, set by
 * vm.sh); it adds the template's "addr" block. */
extern char *stk_addr;

/* template_render: templates/FILE with each "#if FLAG" block kept only when FLAG is one of the blank-
 * separated FLAGS (a "#if !FLAG" is the opposite) and each __NAME__ replaced by the value of that name,
 * written to OUT. The template's bytes are the shell's. */
void template_render(const char *file, const char *flags, FILE *out);

/* stack_render: the app's template into FILE, from cbase.conf and the stk_* values above. Stops the tool
 * when the router's code is over CloudFront's size limit, or the app shell is not in the build. */
void stack_render(const char *file);

#endif
