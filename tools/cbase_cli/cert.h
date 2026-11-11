/* cert.h - the certificate and the DNS records for an app's hostnames, CBASE_DOMAIN's and
 * CBASE_API_HOST (cert.sh; README.md "./cbase deploy tool"). The certificate is found, reused or
 * requested and validated in us-east-1; each hostname's zone is found; the validation records are written
 * where there is a zone and printed where there is not (status 3 then); each hostname is moved onto this
 * app's distribution (a staged attach) or pointed at it.
 *
 * Deploy's dry run is passed in as DRY: a dry run requests nothing and writes nothing, and says what it
 * would do. The functions that the shell stopped with dry_stop return true for the problem, and the
 * caller applies dry_stop (a deploy stops; a dry run goes on and lists it at its end).
 */
#ifndef CBC_CERT_H
#define CBC_CERT_H

#include <stdbool.h>

#include "cli.h"
#include "conf.h"

/* The certificate state of one deploy, as cert.sh's CERT_* variables. */
typedef struct {
  char *arn;        /* CERT_ARN: the certificate the distribution serves the hostnames with, or "" */
  char *api_arn;    /* CERT_API_ARN: the one CBASE_API_HOST's distribution serves, or "" */
  cbc_strs need;    /* CERT_NEED: the hostnames the certificate must cover */
  cbc_strs hosts;   /* CERT_HOSTS: the hostnames, CBASE_DOMAIN's then CBASE_API_HOST */
  cbc_strs zones;   /* CERT_ZONES: each host's Route 53 zone id, or "" for none */
  cbc_strs notes;   /* CERT_NOTES: notes for DNS served elsewhere, printed at the end */
  cbc_strs moves;   /* CERT_MOVES: "<host> <from-distribution> <to-distribution>" a line each */
} cbc_cert_t;

/* cbc_cert_covers: one of the NAMES covers HOST: the same name, or "*." and the host's parent (a wildcard
 * covers one label). */
bool cbc_cert_covers(const char *host, const cbc_strs *names);

/* cbc_cert_resolve: sets C's arn and api_arn for the hostnames (CBASE_CERT_ARN and CBASE_API_CERT_ARN
 * used as they are, else found, reused or requested). Stops the tool (status 3) when the validation
 * records must be added by hand. With DRY, requests nothing. */
void cbc_cert_resolve(cbc_cert_t *c, bool dry);

/* cbc_cert_alias_check: every hostname is free for this app's distributions: TO_MAIN and TO_API (their
 * ids, "" before the first deploy) are the ones it may move onto (moves planned in C->moves), and OWN is
 * the list of the app's own distribution ids. Returns true when a hostname another distribution holds
 * cannot be moved: the caller applies dry_stop (its message is printed here). */
bool cbc_cert_alias_check(cbc_cert_t *c, const char *to_main, const char *to_api, const cbc_strs *own, bool dry);

/* cbc_cert_attach: each planned move, made (associate-alias). Stops the tool on a failed move. */
void cbc_cert_attach(const cbc_cert_t *c);

/* cbc_cert_point: HOST at TARGET (its distribution's name): an alias in its zone, or a note (C->notes)
 * for DNS served elsewhere, or with aliases off the records to set are printed. CF_ZONE_ID is the hosted
 * zone every CloudFront distribution has, for the alias. */
void cbc_cert_point(cbc_cert_t *c, const char *host, const char *target, const char *cf_zone_id);

#endif
