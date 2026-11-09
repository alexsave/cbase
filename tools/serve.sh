#!/bin/sh
# serve.sh - `make serve` (cbase.mk): run a product's `cbase serve` binary the way every product
# runs it on a person's own machine. cbase.mk builds the binary first and then hands over here.
#
#   sh tools/serve.sh <run dir> <binary> <env file> <listen>
#
#   <run dir>   where serve runs (cbase.mk's APP_SERVE_DIR), so the paths the app reads from its
#               working directory (page files, a relative CBASE_DB) are the product's
#   <binary>    the serve binary (build/$(APP_SERVE)), relative to the directory make runs in
#   <env file>  the settings file (APP_ENV_FILE, relative to the run directory unless absolute),
#               named to serve as CBASE_ENV_FILE when it exists and CBASE_ENV_FILE is not already
#               set; empty for none. It is not read here: serve reads it strictly, so a bad line
#               refuses the boot with serve's own sentence
#   <listen>    CBASE_LISTEN unless the environment already sets it (APP_SERVE_LISTEN)
#
# The binary is exec'd, so make's child is the server itself and a signal sent to it (SIGTERM,
# Ctrl-C's SIGINT) reaches serve, which drains and exits.
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: sh tools/serve.sh <run dir> <binary> <env file> <listen>" >&2
  exit 2
fi
dir="$1"; bin="$2"; env_file="$3"; listen="$4"

# The binary's path is from where make runs, so it is made absolute before the cd.
case "$bin" in
  /*) ;;
  *) bin="$(pwd)/$bin" ;;
esac
if [ ! -x "$bin" ]; then
  echo "serve.sh: no serve binary at $bin" >&2
  exit 2
fi
cd "$dir" || { echo "serve.sh: cannot enter the run directory $dir" >&2; exit 2; }

if [ -z "${CBASE_ENV_FILE:-}" ] && [ -n "$env_file" ]; then
  case "$env_file" in
    /*) ;;
    *) env_file="$(pwd)/$env_file" ;;
  esac
  if [ -f "$env_file" ]; then
    CBASE_ENV_FILE="$env_file"
    export CBASE_ENV_FILE
  fi
fi
CBASE_LISTEN="${CBASE_LISTEN:-$listen}"
export CBASE_LISTEN

exec "$bin" serve
