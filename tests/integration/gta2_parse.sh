#!/bin/sh
# Parses every function in gta2_re (Source/*.cpp and *.hpp) and runs every
# pass on each, checking that:
#   - every function parses,
#   - every candidate a pass produces parses back (and isn't a no-op),
#   - almost every expression and statement is understood: anything the
#     parser can't read is a barrier the passes never touch, so a rise here
#     means less of gta2_re gets permuted.
#
# Needs GTA2_RE pointing at a gta2_re checkout; skips (exit 77) otherwise.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PERMUTER=${PERMUTER:-$HERE/../../build/cpp_permuter}
[ -n "${GTA2_RE:-}" ] && [ -d "$GTA2_RE/Source" ] || { echo "skip: GTA2_RE not set"; exit 77; }

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
"$PERMUTER" --check-parse "$GTA2_RE"/Source/*.cpp "$GTA2_RE"/Source/*.hpp > "$OUT" 2>&1
RC=$?
grep -v '^  ' "$OUT" | grep -v '^BAD\|^PARSE' | sed 's/^/    /'
if [ "$RC" -ne 0 ]; then
    echo "FAIL: functions that don't parse or bad candidates:"
    grep '^BAD\|^PARSE' "$OUT" | head -40
    exit 1
fi

# at most 0.1% of expressions and 10 statements not understood
set -- $(sed -n 's/^\([0-9]*\) statements left unclassified.*, \([0-9]*\) of \([0-9]*\) expressions.*/\1 \2 \3/p' "$OUT")
[ $# -eq 3 ] || { echo "FAIL: can't read the summary"; cat "$OUT"; exit 1; }
if [ "$1" -gt 10 ] || [ $(($2 * 1000)) -gt "$3" ]; then
    echo "FAIL: too much of gta2_re isn't understood ($1 statements, $2 of $3 expressions)"
    exit 1
fi
echo "ok: every gta2_re function parses, every candidate parses back"
