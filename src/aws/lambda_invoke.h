// lambda_invoke.h - the async self-invoke that starts a bot drive.
//
// POST https://lambda.<region>.amazonaws.com/2015-03-31/functions/<name>/invocations
// with X-Amz-Invocation-Type: Event and the payload "bot_drive:<id>" (a JSON
// string literal; aws/event.h), SigV4 for service "lambda". The function name
// is AWS_LAMBDA_FUNCTION_NAME and the region AWS_REGION (both set by Lambda).
// AWS_ENDPOINT_URL_LAMBDA ("https://host[:port]"), the AWS SDKs' own override,
// replaces the endpoint when set. 202 is success.
#ifndef CB_LAMBDA_INVOKE_H
#define CB_LAMBDA_INVOKE_H

// CB_OK when Lambda accepted the event (202), CB_E_IO, or CB_E_INVALID for a
// game id the payload cannot carry (aws/event.h cb_event_bot_drive_payload).
int cb_lambda_invoke_bot_drive(const char *game_id);

#endif
