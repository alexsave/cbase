// lib_fake_runtime.h - Lambda's Runtime API, faked, for driving the real
// `bootstrap` with no AWS (the fake Runtime API harness, docs/PLATFORM.md 6.1).
//
// A cb_server on 127.0.0.1 serves what bootstrap asks of the Runtime API:
//   GET  /2018-06-01/runtime/invocation/next   the next queued event, as
//        req-<n> (n from 1), with a far deadline and a function ARN; with
//        none queued it waits (up to 20 s) unless fr_done was called, after
//        which it answers 500, which ends bootstrap's loop
//   POST /2018-06-01/runtime/invocation/req-<n>/{response,error}
//        recorded (path and body), and noted as an arrival
//   POST /2018-06-01/runtime/init/error
//        counted, and its body echoed to stderr
// Point bootstrap at it with AWS_LAMBDA_RUNTIME_API=127.0.0.1:<port>.
#ifndef CB_TEST_FAKE_RUNTIME_H
#define CB_TEST_FAKE_RUNTIME_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

#define FR_MAX 32   // events one run may queue, and arrivals it keeps

typedef struct fr_runtime fr_runtime;

// Starts it, listening on an OS-chosen port (*port). NULL on failure.
fr_runtime *fr_start(const char *function_arn, int *port);
// Queues one event (copied), handed out in queue order.
void fr_queue(fr_runtime *fr, const char *event);
// From now on /next answers 500 once the queue is empty.
void fr_done(fr_runtime *fr);
// Waits until n invocations have been posted (response or error): true, or
// false on timeout.
bool fr_wait_posted(fr_runtime *fr, int n, int timeout_ms);
// Records an arrival from elsewhere (an IoT stand-in's publish), in order with
// the posts ("response req-2", "error req-3").
void fr_note(fr_runtime *fr, const char *what);

// What was posted for req-<i+1>: its path ("" none) and body. The pointers
// stay valid until fr_stop.
const char   *fr_posted_path(fr_runtime *fr, int i);
cb_buf *fr_posted_body(fr_runtime *fr, int i);
// The posts in arrival order: how many, and the i-th one's path's req index.
int  fr_n_posted(fr_runtime *fr);
int  fr_posted_order(fr_runtime *fr, int k);
// Every arrival so far, in order (posts and notes): how many, and each.
int         fr_n_arrivals(fr_runtime *fr);
const char *fr_arrival(fr_runtime *fr, int k);
// How many times /next was asked, and /init/error posted.
int fr_nexts(fr_runtime *fr);
int fr_init_errors(fr_runtime *fr);

void fr_stop(fr_runtime *fr);

#endif
