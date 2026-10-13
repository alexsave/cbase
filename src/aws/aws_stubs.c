// The src/aws stubs: sigv4, https, apigw_mgmt, lambda_invoke and event. The
// aws builder replaces each with its own file (sigv4.c, https.c, ...) and
// deletes it from here. runtime_api.c is real already.
#include "apigw_mgmt.h"
#include "event.h"
#include "https.h"
#include "lambda_invoke.h"
#include "sigv4.h"

#include "util/err.h"

#include <string.h>

int cb_apigw_post(const char *endpoint, const char *connection_id, const void *data, size_t len) { return CB_E_NOTIMPL; }
int cb_apigw_delete(const char *endpoint, const char *connection_id) { return CB_E_NOTIMPL; }

int cb_lambda_invoke_bot_drive(const char *game_id) { return CB_E_NOTIMPL; }
