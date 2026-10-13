// lambda_invoke.h - the async self-invoke that starts a bot drive.
//
// POST https://lambda.<region>.amazonaws.com/2015-03-31/functions/<name>/invocations
// with X-Amz-Invocation-Type: Event and the payload {"cbase":"bot_drive",
// "game":"<id>"}, SigV4 for service "lambda". The function name is
// AWS_LAMBDA_FUNCTION_NAME (set by Lambda). 202 is success.
#ifndef CB_LAMBDA_INVOKE_H
#define CB_LAMBDA_INVOKE_H

// CB_OK when Lambda accepted the event, CB_E_IO, or CB_E_NOTIMPL.
int cb_lambda_invoke_bot_drive(const char *game_id);

#endif
