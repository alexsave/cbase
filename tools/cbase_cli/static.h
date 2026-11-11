/* static.h - an app's static files into its folder of the static group's bucket, s3://<bucket>/<app>/
 * (static.sh; README.md "./cbase deploy tool"). The scan, the per-file headers from the rules table,
 * the upload order, and the prune that is held to the app's folder.
 */
#ifndef CBC_STATIC_H
#define CBC_STATIC_H

#include <stdbool.h>
#include <stddef.h>

#include "cli.h"
#include "conf.h"

/* The static site's upload plan: one group per pair of headers (and phase), each a scratch tree of
 * links to its files, so one `aws s3 cp --recursive` carries exactly its pair (static.sh static_plan). */
typedef struct {
  int phase;        /* 1 content-hashed, 2 other than html, 3 html: the upload order */
  char *type;       /* Content-Type */
  char *cache;      /* Cache-Control */
  size_t count;     /* files in the group */
  char *tree;       /* the scratch tree of the group's links */
} cbc_static_group;

typedef struct {
  cbc_static_group *v;
  size_t n;
  size_t cap;
  char *root;       /* the scratch tree's directory: TMP/static-tree */
} cbc_static_plan;

/* cbc_static_scan: DIR's files to upload, relative paths, sorted by bytes into FILES. Stops the tool on
 * a directory that is not a build (no index.html), a file name with a control character, or a listing
 * that fails; nothing is changed by a scan. */
void cbc_static_scan(const char *dir, cbc_strs *files);

/* cbc_static_plan_make: the upload groups for FILES under DIR, their links made in TMP. Sets P. */
void cbc_static_plan_make(cbc_static_plan *p, const char *dir, const cbc_strs *files, const char *tmp);

/* cbc_static_print_plan: the groups in upload order, one line each. */
void cbc_static_print_plan(const cbc_static_plan *p);

/* cbc_static_upload: each group, phase by phase, into s3://BUCKET/APP/ (html last). */
void cbc_static_upload(const cbc_static_plan *p, const char *bucket, const char *region, const char *app);

/* cbc_static_prune: deletes the keys under APP/ that FILES does not name. A key outside APP/ is never
 * deleted, whatever the listing says. */
void cbc_static_prune(const char *bucket, const char *region, const char *app, const cbc_strs *files);

/* cbc_static_bucket: the static group's bucket for CONF_STATIC (or CBASE_STATIC_BUCKET, when set), and
 * its region; the caller takes the app's folder from CBASE_STATIC_PREFIX or the app's name. Stops the
 * tool when a call fails. */
void cbc_static_bucket(const char *static_bucket, const char *group, char **bucket, char **region);

/* cbc_static_policy_ok: true when BUCKET's policy lets ACCOUNT's distributions read it. Otherwise it
 * prints the statement to add (stderr) and returns false; the caller decides whether that stops the
 * deploy (a dry run goes on). Stops the tool on a failed read. */
bool cbc_static_policy_ok(const char *bucket, const char *account);

#endif
