/* deploy.h - the deploy and undeploy commands (the deploy and undeploy cases of tools/cbase): the order
 * of the steps over the stack, VM, static and certificate modules, and the dry run's problems.
 */
#ifndef CBC_DEPLOY_H
#define CBC_DEPLOY_H

#include "cli.h"

#include <stdbool.h>

/* The dry run's state: the flag (DRY_RUN=1), and the problems a dry run lists at its end (tools/cbase's
 * DRY_PROBLEMS). */
extern bool cbc_dry_run;
extern cbc_strs cbc_dry_problems;

/* dry_stop: a problem a deploy stops on, said where it is found. A dry run keeps WHAT and goes on; a deploy
 * prints LINE (if any, to stderr) and exits 1 there. */
void dry_stop(const char *what, const char *line);

/* cbc_deploy_scan: static_scan DIR, listed in a file of the run's (stk_files) as the router's page list
 * and the prune read it. FILES gets the sorted names. */
void cbc_deploy_scan(const char *dir, cbc_strs *files);

/* cbc_deploy: CONF_FILE is the -f file; ARGS (ARGC of them, after "deploy"), already checked by the caller to be deploy's flags. */
void cbc_deploy(const char *conf_file, int argc, char **argv);

/* cbc_undeploy: ARGS after "undeploy", already checked. */
void cbc_undeploy(const char *conf_file, int argc, char **argv);

#endif
