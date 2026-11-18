# tz_zones.awk - build/gen/tz_zones.c: an app's time zones, from lib/tz/tzdb.tsv (cbase.mk).
#
#   awk -v zones='America/New_York Europe/Oslo' -f tools/tz_zones.awk lib/tz/tzdb.tsv > tz_zones.c
#
# `zones` is the app's APP_TZ_ZONES. The file holds exactly those zones, in that order, so a zone's index
# (tz_find's answer) is its position in the list, and the app links only its own rules. A name that is
# not in the tsv, or is listed twice, stops the build (exit 1, the reason on stderr).
BEGIN { FS = "\t" }
/^# tzdata / { version = $0; sub(/^# tzdata /, "", version); sub(/:.*/, "", version); next }
/^#/ || NF == 0 { next }
{ rule[$1] = $2 }
END {
    n = split(zones, want, /[ \t\n]+/)
    out = ""
    count = 0
    for (i = 1; i <= n; i++) {
        z = want[i]
        if (z == "") continue
        if (!(z in rule)) { printf "tz_zones: %s is not a zone of lib/tz/tzdb.tsv (tzdata %s)\n", z, version > "/dev/stderr"; bad = 1; continue }
        if (z in seen) { printf "tz_zones: %s is listed twice in APP_TZ_ZONES\n", z > "/dev/stderr"; bad = 1; continue }
        if (z ~ /["\\]/ || rule[z] ~ /["\\]/) { printf "tz_zones: %s: a quote or backslash cannot go in a C string\n", z > "/dev/stderr"; bad = 1; continue }
        seen[z] = 1
        out = out sprintf("    { \"%s\", \"%s\" },\n", z, rule[z])
        count++
    }
    if (!bad && count == 0) { print "tz_zones: no zone given (APP_TZ_ZONES is empty)" > "/dev/stderr"; bad = 1 }
    if (bad) exit 1
    printf "// tz_zones.c - written by cbase.mk from lib/tz/tzdb.tsv (tzdata %s): this app's zones (APP_TZ_ZONES), in that order.\n", version
    printf "#include \"tz/tz.h\"\n"
    printf "const tz_zone tz_zones[] = {\n%s};\n", out
    printf "const int tz_zone_count = %d;\n", count
}
