/* stack.c - the stack functions (stack.h). Stubs until milestone 4 lands their bodies: each one says it
 * is not implemented yet and stops the tool, so nothing runs a half-port by accident.
 */
#include "stack.h"

#include "cli.h"

#include <stdbool.h>

#define STACK_STUB(name) cbc_fail(CBC_EXIT_FAIL, "%s is not implemented in the C tool yet", name)

char *stk_name = "";
char *stk_region = "";
char *stk_tag = "";
char *stk_caps = "";
bool stk_app = false;
char *stk_template = "";
char *stk_status = "";
char *stk_type = "";
char *stk_cs = "cbase-deploy";
char *stk_changes = "";
char *stk_cert = "";
char *stk_api_cert = "";
char *stk_bucket = "";
char *stk_bucket_region = "";
char *stk_prefix = "";
bool stk_no_rollback = false;
char *stk_files = "";

void stack_use(const char *name, const char *region, const char *tag) {
  (void)name; (void)region; (void)tag;
  STACK_STUB("stack_use");
}
void stack_app_use(void) { STACK_STUB("stack_app_use"); }
void stack_params_clear(void) { STACK_STUB("stack_params_clear"); }
void stack_param_add(const char *key, const char *value) {
  (void)key; (void)value;
  STACK_STUB("stack_param_add");
}
void stack_status(void) { STACK_STUB("stack_status"); }
char *stack_output(const char *key) {
  (void)key;
  STACK_STUB("stack_output");
}
void stack_changeset(void) { STACK_STUB("stack_changeset"); }
void stack_execute(void) { STACK_STUB("stack_execute"); }
void stack_discard(void) { STACK_STUB("stack_discard"); }
void stack_delete(void) { STACK_STUB("stack_delete"); }
void template_render(const char *file, const char *flags, FILE *out) {
  (void)file; (void)flags; (void)out;
  STACK_STUB("template_render");
}
void stack_render(const char *file) {
  (void)file;
  STACK_STUB("stack_render");
}
