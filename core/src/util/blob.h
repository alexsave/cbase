// blob.h - a place whole files are kept: one object per key, replaced as a whole.
//
// What a backup needs of its storage and no more: put a file under a key, get the file under a key,
// and delete one it no longer names.
// A provider that has such a place implements it (cb_blob_open_s3, in the cloud provider's own
// files); a test passes a fake. Both calls block, so they run on a job thread, never on the loop.
#ifndef CB_BLOB_H
#define CB_BLOB_H

typedef struct {
    // CB_OK, or a CB_E_* (the object is not stored: nothing changed under `key`).
    int (*put)(void *ud, const char *key, const char *path);
    // CB_OK with the object written to `path`, CB_E_NOT_FOUND when there is none, else a CB_E_*.
    int (*get)(void *ud, const char *key, const char *path);
    void *ud;
    // CB_OK when no object is under `key` any more (also when there was none), else a CB_E_*.
    // NULL: the place cannot delete (what is left over stays).
    int (*del)(void *ud, const char *key);
} cb_blob;

// An object bucket in `region`, reached with the credentials this process has (the environment's, else the
// instance role's). CB_OK and *out, or a CB_E_*.
int cb_blob_open_s3(const char *bucket, const char *region, cb_blob *out);

#endif
