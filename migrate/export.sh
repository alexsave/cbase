#!/bin/bash
# export.sh - the hosted Supabase data, read-only, into c_base/migrate/out/ as
# CSV (one file per table), for cbase-import.
#
#   c_base/migrate/export.sh [out-dir]      default: c_base/migrate/out
#
# Every statement is a SELECT, sent to the Management API's READ-ONLY query
# endpoint (it runs as supabase_read_only_user), so this script cannot change
# the hosted database. The token is the Supabase CLI's, from the macOS keychain
# (service "Supabase CLI", account supabase); it is never printed.
#
# THE OUTPUT HOLDS PASSWORD HASHES. out/ is gitignored, created mode 0700, and
# every file 0600; delete it once the import is done (README.md).
#
# Format: RFC 4180 CSV with a header row; NULL is an empty unquoted field and
# every non-NULL value is quoted; bytea is lowercase hex (no "\x"); every
# timestamp is integer milliseconds since the Unix epoch (a TIMESTAMP without
# a zone is read as UTC, the hosted database's zone); arrays are joined by ';'.
set -euo pipefail
umask 077

PROJECT=wngpfwmwkltonwosqflx
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
chmod 700 "$OUT"

TOKEN=$(security find-generic-password -s "Supabase CLI" -a supabase -w 2>/dev/null || true)
case "$TOKEN" in go-keyring-base64:*) TOKEN=$(printf %s "${TOKEN#go-keyring-base64:}" | base64 -d) ;; esac
if [ -z "$TOKEN" ]; then
    echo "export.sh: no Supabase CLI token in the keychain (run: supabase login)" >&2
    exit 1
fi

# One CSV field: NULL stays empty, anything else is quoted with "" escaping.
f() { printf "CASE WHEN (%s) IS NULL THEN '' ELSE '\"' || replace((%s)::text, '\"', '\"\"') || '\"' END" "$1" "$1"; }
ms() { printf "(extract(epoch FROM %s) * 1000)::bigint" "$1"; }
hex() { printf "encode(%s, 'hex')" "$1"; }

# export_table <name> <table> <order by> <header> <expr>...
export_table() {
    local name=$1 table=$2 order=$3 header=$4
    shift 4
    local row="" e
    for e in "$@"; do
        row="${row:+$row || ',' || }$(f "$e")"
    done
    local sql="SELECT coalesce(string_agg($row, E'\\n' ORDER BY $order), '') AS csv, count(*) AS n FROM $table"
    local body
    body=$(python3 -I -c 'import json,sys; print(json.dumps({"query": sys.argv[1]}))' "$sql")
    local raw="$OUT/.$name.json"
    local code
    code=$(curl -sS -o "$raw" -w '%{http_code}' -X POST \
        -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
        --data "$body" "https://api.supabase.com/v1/projects/$PROJECT/database/query/read-only")
    if [ "$code" != "201" ] && [ "$code" != "200" ]; then
        echo "export.sh: $name: HTTP $code: $(head -c 400 "$raw")" >&2
        rm -f "$raw"
        exit 1
    fi
    {
        printf '%s\n' "$header"
        python3 -I -c 'import json,sys; r=json.load(open(sys.argv[1]))[0]; sys.stdout.write(r["csv"] + ("\n" if r["csv"] else ""))' "$raw"
    } > "$OUT/$name.csv"
    local n
    n=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["n"])' "$raw")
    rm -f "$raw"
    printf '%-18s %6s rows\n' "$name" "$n"
    printf '%s %s\n' "$name" "$n" >> "$OUT/counts.txt"
}

: > "$OUT/counts.txt"

export_table users \
    "auth.users" "created_at, id" \
    "id,email,encrypted_password,username,created_at,last_sign_in_at" \
    "id" "lower(email)" "encrypted_password" "raw_user_meta_data->>'username'" "$(ms created_at)" "$(ms last_sign_in_at)"

export_table bots \
    "public.bots" "created_at, nickname" \
    "id,nickname,strategy_key,elo_rating,previous_elo,games_played,created_at,updated_at" \
    "id" "nickname" "strategy_key" "elo_rating" "previous_elo" "games_played" "$(ms created_at)" "$(ms updated_at)"

export_table user_elo_ratings \
    "public.user_elo_ratings" "user_id" \
    "user_id,username,elo_rating,previous_elo,games_played,updated_at" \
    "user_id" "username" "elo_rating" "previous_elo" "games_played" "$(ms updated_at)"

export_table games \
    "public.games" "created_at, id" \
    "id,status,state,roster,needs_bots,game_seed,logs_packed,version,round_epoch,created_at,updated_at,last_commit_at" \
    "id" "status" "$(hex state)" "$(hex roster)" "needs_bots" "game_seed" "$(hex logs_packed)" "version" "round_epoch" \
    "$(ms created_at)" "$(ms updated_at)" "$(ms last_commit_at)"

export_table player_hands \
    "public.player_hands" "game_id, player_id" \
    "game_id,player_id,joined_at" \
    "game_id" "player_id" "$(ms joined_at)"

export_table bot_hands \
    "public.bot_hands" "game_id, bot_id" \
    "game_id,bot_id" \
    "game_id" "bot_id"

export_table game_snapshots \
    "public.game_snapshots" "created_at, id" \
    "id,game_id,player_ids,moves,extras,created_at" \
    "id" "game_id" "array_to_string(player_ids, ';')" "$(hex moves)" "$(hex extras)" "$(ms created_at)"

export_table chat_messages \
    "public.chat_messages" "created_at, id" \
    "id,game_id,user_id,message,is_system,created_at" \
    "id" "game_id" "user_id" "message" "is_system" "$(ms created_at)"

chmod 600 "$OUT"/*.csv "$OUT/counts.txt"
echo "export.sh: wrote $OUT (password hashes inside: delete it after the import)"
