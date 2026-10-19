// backup.h - the database file kept in a cb_blob: restored at start if it is missing, uploaded when it changed.
//
// RPO is the interval (60 s by default). One object, replaced each time; the bucket's versioning keeps the history.
#ifndef CB_BACKUP_H
#define CB_BACKUP_H

#include "ctx.h"
#include "config.h"
#include "util/blob.h"

// If `db_path` does not exist, fetches `key` into place and checks it opens and passes PRAGMA integrity_check.
// CB_OK when the file is in place, or there was nothing stored (an empty start, logged). A failed download
// or a file that fails its check is an error and leaves no file at `db_path`: an empty database would be
// uploaded over the last good backup.
int cb_backup_restore(const cb_blob *blob, const char *key, const char *db_path);

// A consistent copy of the database at `db_path` in a new file `dest` (VACUUM INTO, on its own connection,
// so the server's writers are not held).
int cb_backup_copy(const char *db_path, const char *dest);

// Every `interval_ms` on ctx's loop: when the store changed since the last upload, a job copies and
// uploads (never on the loop thread). A running backup makes the tick skip; a failure is logged and the
// next tick tries again. Returns the state, freed with cb_backup_stop.
typedef struct cb_backup cb_backup;
cb_backup *cb_backup_start(cb_ctx *ctx, const cb_blob *blob, const char *key, int64_t interval_ms);
void cb_backup_stop(cb_backup *b);
// One tick: skipped when a backup is running, else the job is queued on the job pool.
void cb_backup_tick(cb_backup *b);
// The job's body, on the calling thread (the tests call it): 0 when the store has not changed since the last
// upload (nothing is sent), 1 when a copy was uploaded, else a negative CB_E_* (the next call retries).
int cb_backup_once(cb_backup *b);
// Backups that finished, and that failed; for the tests and the log.
void cb_backup_counts(const cb_backup *b, long *ok, long *failed);

// The serve hooks of the local provider: restore before boot, the timer after listen, from the environment
// (CBASE_BACKUP_BUCKET, CBASE_BACKUP_REGION, CBASE_BACKUP_KEY, CBASE_BACKUP_INTERVAL_S). No bucket: nothing.
int cb_backup_serve_prepare(const cb_config *cfg);
void cb_backup_serve_start(cb_ctx *ctx);

#endif
