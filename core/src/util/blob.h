// blob.h - a place whole files are kept: one object per key, replaced as a whole.
//
// What a backup needs of its storage and no more: put a file under a key, get the file under a key,
// and delete one it no longer names.
// A provider that has such a place implements it (a cb_blob_backend, in that provider's own files);
// a test passes a fake. Both calls block, so they run on a job thread, never on the loop.
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

// AN OBJECT STORE BACKEND: a bucket in `region`, reached with the credentials this process has (the
// environment's, else the instance role's). open answers CB_OK and *out, or a CB_E_*.
typedef struct {
    const char *name;
    int (*open)(const char *bucket, const char *region, cb_blob *out);
} cb_blob_backend;
// The object stores this binary links, NULL-terminated, the first the one a backup uses: written by
// cbase.mk (build/gen/cbase_links.c) from the providers the build links. Empty when none is.
extern const cb_blob_backend *const cb_blob_backends[];

#endif
