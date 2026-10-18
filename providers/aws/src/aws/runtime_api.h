// runtime_api.h - the Lambda Runtime API client: how `bootstrap` gets work.
//
// Lambda's custom-runtime contract (provided.al2023): plain HTTP to
// $AWS_LAMBDA_RUNTIME_API (host:port, loopback, no TLS):
//   GET  /2018-06-01/runtime/invocation/next             blocks until an event;
//        headers Lambda-Runtime-Aws-Request-Id, Lambda-Runtime-Deadline-Ms,
//        Lambda-Runtime-Invoked-Function-Arn, Lambda-Runtime-Trace-Id
//   POST /2018-06-01/runtime/invocation/<id>/response    the result
//   POST /2018-06-01/runtime/invocation/<id>/error       {errorMessage, errorType}
//   POST /2018-06-01/runtime/init/error                  a failed start
// Implemented here (not a stub): it is a dozen lines of HTTP and the loop in
// main_lambda.c, and the fake-runtime smoke test exercises it.
#ifndef CB_RUNTIME_API_H
#define CB_RUNTIME_API_H

#include "util/str.h"

#include <stdint.h>

typedef struct {
    char    request_id[128];
    int64_t deadline_ms;            // epoch ms
    char    function_arn[256];
    char    trace_id[256];
    cb_buf  event;                  // the event JSON
} cb_invocation;

// Reads AWS_LAMBDA_RUNTIME_API. CB_OK, or CB_E_INVALID when unset.
int  cb_runtime_init(char *host, size_t cap, int *port);
// Blocks for the next event. CB_OK, or CB_E_IO (the runtime is gone: exit).
int  cb_runtime_next(const char *host, int port, cb_invocation *out);
void cb_invocation_free(cb_invocation *inv);
int  cb_runtime_respond(const char *host, int port, const char *request_id, const void *body, size_t len);
int  cb_runtime_error(const char *host, int port, const char *request_id, const char *type, const char *message);
int  cb_runtime_init_error(const char *host, int port, const char *type, const char *message);

#endif
