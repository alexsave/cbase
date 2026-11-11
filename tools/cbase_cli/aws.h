/* aws.h - every AWS call the deploy tool makes (aws.sh; README.md "./cbase deploy tool"). Each is one
 * AWS CLI call read with --query and --output text: the CLI keeps the credentials, and the tool never
 * reads or prints one. The last call's stderr is kept in memory and classified by the error code the
 * CLI prints; what is shown of it is masked (aws_mask). Shell function names are kept where C allows.
 */
#ifndef CBC_AWS_H
#define CBC_AWS_H

#include "cli.h"

#include <stdarg.h>
#include <stdbool.h>

/* aws_account: the twelve-digit account the credentials are in, set by aws_init. */
extern char *aws_account;

/* aws_mask_add: a value this run holds that a line may name (vm.sh adds the origin secret, of letters,
 * digits, _ and -). A line holding it is masked by aws_mask. */
void aws_mask_add(const char *value);

/* aws_init: the CLI found, and aws_account asked of STS. Stops the tool on failure. */
void aws_init(void);

/* aws_call: runs `aws --no-cli-pager` with the NULL-terminated ARGS, its stdout appended to OUT (may be
 * NULL to discard it). A throttled call is tried again, up to five times in all. Returns the status of
 * the last attempt; its stderr is held, read by aws_code and aws_fail. */
int aws_call(cbc_buf *out, ...);

/* aws_exec: the one core every call goes through: `aws --no-cli-pager`, then the words of HEAD, then the
 * words of TAIL (each NULL-terminated, either may be NULL), run once per attempt with the retry on
 * throttling. The child's argv holds pointers to the callers' strings, which must outlive the call:
 * nothing is copied. aws_call and aws_callv are the varargs forms (aws_callv with a HEAD, for the
 * `cloudformation --region R` prefix). */
int aws_exec(cbc_buf *out, const char *const *head, const char *const *tail);
int aws_callv(cbc_buf *out, const char *const *head, va_list ap);

/* aws_err_contains: whether the last failed call's stderr holds TEXT (a plain substring). */
bool aws_err_contains(const char *text);

/* aws_code: the error code of the last failed call ("NoSuchKey"), or "". Valid until the next call. */
char *aws_code(void);

/* aws_fail: stops the tool (exit 1) on a failed call the caller does not handle, after printing what the
 * person can do about it (credentials, an old CLI, AccessDenied, or the masked error text). WHAT names
 * the call for the message. Never returns. */
void aws_fail(const char *what) __attribute__((noreturn));

/* aws_s3_delete_keys: deletes the N KEYS of BUCKET (REGION) for a prune, and prints one "pruned s3://BUCKET/KEY"
 * line per key after the call that deleted it succeeded. A key made only of letters, digits and "/ . _ - ~" goes in
 * one `s3api delete-objects` call with up to 1000 keys (S3's limit), in the CLI's shorthand syntax (no JSON); any
 * other key (a space, comma, bracket, quote, equals, non-ASCII: shorthand cannot carry it unmangled) goes alone
 * through `s3api delete-object`. An error entry in a batch's answer, or a failed call, stops the tool (exit 1). The
 * caller has already held every key to its folder: nothing is checked here. */
void aws_s3_delete_keys(const char *bucket, const char *region, char *const *keys, size_t n);

/* aws_mask: LINE as the person may see it: a line naming an origin's headers or a parameter's value, or
 * holding one of aws_mask_values, is replaced by a sentence saying so. Returns a static buffer or LINE. */
const char *aws_mask(const char *line);

/* aws_poll: THE BOUNDED WAIT. Calls FN up to TRIES times, POLL_SECONDS apart (CBASE_POLL_SECONDS, 5 by
 * default), until FN returns true. Returns true when FN did, false when it never did. FN may stop the
 * tool itself. */
bool aws_poll(int tries, bool (*fn)(void));

/* aws_region: the region the CLI calls, or "the configured region" when none is set (for messages). */
char *aws_region(void);

/* aws_region_required: the region the CLI calls; stops the tool when none is set. */
char *aws_region_required(void);

/* aws_group_find: a group's resource, found by its tag: the instance id of a vm group, the bucket name
 * of a static one, printed through ID on success (returns 0). On a failure it prints the message and
 * returns 1 without exiting, as the shell's exit inside a command substitution would: the caller
 * decides whether to stop. */
int aws_group_find(const char *kind, const char *name, char **id);

/* aws_s3_get: the object at KEY in BUCKET into FILE. Returns 0 with its ETag in ETAG, 3 when there is no
 * such key. A missing state bucket or another failure stops the tool. */
int aws_s3_get(const char *bucket, const char *key, const char *file, char **etag);

/* aws_s3_put: writes FILE under KEY if COND holds: "*" for no object yet, otherwise the ETag the object
 * must still have. Returns 0 when written, 3 when the precondition failed (someone else wrote first).
 * Other failures stop the tool. */
int aws_s3_put(const char *bucket, const char *key, const char *file, const char *cond);

/* aws_no_state_bucket: stops the tool: the account has no state bucket BUCKET yet. */
void aws_no_state_bucket(const char *bucket) __attribute__((noreturn));


/* aws_show: the first LINES lines of the last failed call's stderr, masked and indented by two spaces, on
 * stderr (aws.sh's aws_show). */
void aws_show(int lines);

/* aws_set_err_text: replaces the held stderr of the last call with TEXT, exactly (the shell writes the
 * change set's status reason or the events there before aws_show). */
void aws_set_err_text(const char *text);

/* aws_secret_filter: the JMESPath filter that holds a detail (a change set's Target, a drift's property
 * difference) secret: its path or either value names an origin's custom headers, or, with CAUSE (a change
 * set), a parameter caused it that is not plain (aws.sh's aws_secret_filter). Returns a new string. */
char *aws_secret_filter(const char *path, const char *before, const char *after, const char *cause);

#endif
