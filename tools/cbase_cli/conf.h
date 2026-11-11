/* conf.h - cbase.conf: the deploy tool's settings file, read and checked (the shell tool's conf.sh).
 * conf_load stops the tool with the file's line number and a sentence on any error; a value that
 * looks like a secret stops it without being printed. Nothing here calls AWS.
 */
#ifndef CBC_CONF_H
#define CBC_CONF_H

#include "cli.h"

#include <stdbool.h>
#include <stddef.h>

/* One normalized route: "<pattern> <origin> <cache> <methods>", and the line it is on. */
typedef struct {
  char *pattern;
  char *origin;
  char *cache;
  char *methods;
  int line;
} cbc_route;

typedef struct {
  cbc_route *v;
  size_t n;
  size_t cap;
} cbc_routes;

/* The settings as conf_load leaves them. An unset string is "", as the shell's CONF_ variables are. */
typedef struct {
  char *file;
  char *app;
  char *vm;            /* CONF_VM: the VM group, or "none" */
  char *stat;          /* CONF_STATIC: the static group, or "none" */
  char *vm_stack;
  char *vm_origin;
  char *vm_port;       /* decimal, as printed */
  char *vm_data;
  char *vm_settings;
  char *vm_artifacts;
  char *vm_replaces;
  char *backup_key;
  char *backup_bucket;
  char *static_bucket;
  char *static_prefix;
  char *memory;        /* the declared MiB in decimal, or "" */
  bool memory_estimated;
  char *domain;        /* the first CBASE_DOMAIN hostname */
  cbc_strs domains;
  char *cert_arn;
  char *api_cert_arn;
  char *zone;
  char *aliases;       /* "on" or "off" */
  char *api_host;
  char *static_dir;    /* no trailing '/'; "" for a VM-only app */
  bool static_dir_default;
  cbc_strs hashed;
  char *shell;
  cbc_routes routes;
  cbc_routes api_routes;
} cbc_conf_t;

extern cbc_conf_t cbc_conf;

/* The memory an undeclared VM app is counted at, MiB (the shell's CONF_MEMORY_ESTIMATE). */
#define CBC_MEMORY_ESTIMATE 128

/* cbc_conf_name_ok: a group name or CBASE_APP: a lower-case letter, then lower-case letters, digits and -, at most 32 characters, not ending in -. */
bool cbc_conf_name_ok(const char *s);

/* cbc_conf_load: reads FILE, checks it, and resolves the defaults into cbc_conf. Exits on error. */
void cbc_conf_load(const char *file);

/* cbc_conf_mem: the app's mem field in a state file: the declared MiB, or "~128" when undeclared; "" for an app
 * with no VM. */
const char *cbc_conf_mem(void);

/* cbc_conf_print: the settings as read, defaults filled in, to stdout (config check, doctor). */
void cbc_conf_print(void);

#endif
