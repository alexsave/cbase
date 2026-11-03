# shellcheck shell=bash
# Sourced by vm_deploy.sh and vm_release.sh (no AWS call in here, so
# role_covers_release.sh has nothing to read): how the instance's boot is built.
#
#   boot_assemble BOOT UNIT OUT   vm_boot.sh with cbase.service spliced in at
#                                 @CBASE_SERVICE@, comments and all. This is the
#                                 object uploaded as vm/cbase-vm-boot-<sha12>.
#   stub_build STUB OUT [LIMIT]   vm_boot_stub.sh without its comments and blank
#                                 lines: the user-data. Fails (return 1, message
#                                 on stderr) over LIMIT bytes, 1024 by default.
#   boot_key SUM                  the object key of a boot script with this sha256.
#   sha256 FILE                   the file's sha256 (sha256sum, or shasum on a Mac).

STUB_LIMIT=1024

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

boot_key() { echo "vm/cbase-vm-boot-${1:0:12}"; }

boot_assemble() {
  awk -v unit="$2" '
    $0 == "@CBASE_SERVICE@" { while ((getline l < unit) > 0) print l; next } { print }
  ' "$1" > "$3"
  if grep -q '^@CBASE_SERVICE@$' "$3" || ! grep -q '^ExecStart=/usr/local/bin/cbase serve$' "$3"; then
    echo "boot_assemble: cbase.service was not spliced into the boot script" >&2
    return 1
  fi
}

stub_build() {
  local limit="${3:-$STUB_LIMIT}" n
  # The shebang stays; every other comment line and blank line goes.
  awk 'NR == 1 || (!/^#/ && !/^$/)' "$1" > "$2"
  n="$(wc -c < "$2" | tr -d ' ')"
  if [ "$n" -gt "$limit" ]; then
    echo "stub_build: the user-data stub is $n bytes; the limit is $limit. It only fetches, checks and runs the pinned boot script: put new logic in vm_boot.sh" >&2
    return 1
  fi
}
