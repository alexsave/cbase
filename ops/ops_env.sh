#!/bin/bash
# ops_env.sh - where the ops scripts' settings and secrets live, and the one
# reader of them. Sourced by deploy.sh and web_deploy.sh; never run.
#
# THE FILE: $CBASE_OPS_ENV, else ~/.config/cbase/ops.env, else the old
# c_base/ops/.env (gitignored). It lives outside the repo on purpose: a
# worktree removed with its checkout once took the only copy with it.
# Mode 0600. Only CBASE_* NAME=value lines are read, so the file can never run
# anything, and a value already in the environment wins over the file.
#
# THE RULE THAT MATTERS: a secret is generated only for a stack that does not
# exist yet (cbase_ops_secret). Against a live stack a fresh CBASE_JWT_SECRET
# would sign every user out and a fresh admin token would lock the console, so
# a missing secret there is an error that says where to recover it from.

# The settings file this run reads (and, for a new stack, writes).
cbase_ops_env_file() {
  local here="$1"
  if [ -n "${CBASE_OPS_ENV:-}" ]; then
    printf '%s\n' "$CBASE_OPS_ENV"
  elif [ -f "$HOME/.config/cbase/ops.env" ] || [ ! -f "$here/ops/.env" ]; then
    printf '%s\n' "$HOME/.config/cbase/ops.env"
  else
    printf '%s\n' "$here/ops/.env"
  fi
}

# Reads CBASE_* NAME=value lines of $1 into the environment (the environment wins).
cbase_ops_read() {
  local file="$1" line name value
  [ -f "$file" ] || return 0
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|'#'*) continue ;; esac
    name="${line%%=*}"
    value="${line#*=}"
    case "$name" in CBASE_[A-Z0-9_]*) ;; *) continue ;; esac
    if [ -z "${!name:-}" ]; then
      printf -v "$name" '%s' "$value"
      export "${name?}"
    fi
  done < "$file"
}

# cbase_ops_secret NAME FILE STACK_EXISTS: NAME is set (from the environment or
# the file), or, for a stack that does not exist yet, generated into FILE.
# Against an existing stack a missing secret stops the run.
cbase_ops_secret() {
  local name="$1" file="$2" exists="$3"
  [ -n "${!name:-}" ] && return 0
  if [ "$exists" = yes ]; then
    echo "ops_env: $name is not in $file, and the stack already exists." >&2
    echo "ops_env: generating a new one would sign every user out (or lock the console)." >&2
    echo "ops_env: recover it from the live function (c_base/RUNBOOK.md, \"The ops settings file\") and run again." >&2
    return 1
  fi
  local value
  value="$(openssl rand -hex 32 2>/dev/null)"
  [ "${#value}" -eq 64 ] || { echo "ops_env: openssl could not make $name" >&2; return 1; }
  mkdir -p "$(dirname "$file")"
  chmod 700 "$(dirname "$file")" 2>/dev/null || true
  [ -f "$file" ] || (umask 077; : > "$file")
  chmod 600 "$file"
  printf -v "$name" '%s' "$value"
  export "${name?}"
  printf '%s=%s\n' "$name" "${!name}" >> "$file"
  echo "generated $name into $file (a new stack)"
}
