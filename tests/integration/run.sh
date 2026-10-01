#!/bin/sh
# Integration tests: each case directory holds base.cpp (the "decompiled"
# source, wrong somewhere) and target.cpp (the source the target object was
# built from). The two differ only inside the function. The permuter has to
# turn base.cpp into code that compiles to exactly the target.
#
# The compiler is clang's i686 MSVC target at -O0, standing in for VC6: at -O0
# statement and declaration order show up directly in the code. See vc6.sh for
# the same check with the real VC6 on gta2_re.
#
# Each case also has negative controls: the wrong passes must NOT match, which
# shows the match comes from the pass under test, not from something else.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PERMUTER=${PERMUTER:-$HERE/../../build/cpp_permuter}
CXX=${CXX_INTEGRATION:-clang++}
command -v "$CXX" >/dev/null || { echo "skip: $CXX not found"; exit 77; }
command -v llvm-objdump >/dev/null || { echo "skip: llvm-objdump not found"; exit 77; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
COMPILE="$CXX --target=i686-pc-windows-msvc -O0 -c {src} -o {obj}"
FAILED=0
N=0

# run <case> <function> <out-name> <permuter args...>; sets RC and OUT
run() {
    case_=$1 fn=$2 name=$3
    shift 3
    N=$((N + 1))
    OUT="$WORK/out_$N"
    mkdir -p "$WORK/src_$N"
    cp "$HERE/$case_/base.cpp" "$WORK/src_$N/$case_.cpp"
    $CXX --target=i686-pc-windows-msvc -O0 -c "$HERE/$case_/target.cpp" -o "$WORK/target_$N.obj" || exit 1
    "$PERMUTER" -s "$WORK/src_$N/$case_.cpp" -f "$fn" -c "$COMPILE" -t "$WORK/target_$N.obj" \
        -j 2 --seed 1 -o "$OUT" "$@" > "$WORK/log_$N" 2>&1
    RC=$?
    LABEL="$case_: $name ($*)"
}

fail() {
    echo "FAIL: $LABEL: $1"
    tr '\r' '\n' < "$WORK/log_$N" | grep -v '^iterations' | tail -30
    FAILED=$((FAILED + 1))
}

base_score() { tr '\r' '\n' < "$WORK/log_$N" | sed -n 's/^base score: //p'; }

# The permuter found an exact match (exit 0, score 0, base was not a match).
expect_match() {
    if [ "$RC" -ne 0 ]; then fail "expected a match, exit $RC"; return 1; fi
    b=$(base_score)
    if [ -z "$b" ] || [ "$b" -eq 0 ]; then fail "base already matched (score '$b')"; return 1; fi
    if ! ls -d "$OUT"/output-0-* >/dev/null 2>&1; then fail "no output-0-* written"; return 1; fi
    echo "ok:   $LABEL: base $b -> 0"
}

# Like expect_match, and the matched source is the target source (ignoring
# whitespace): the search recovered exactly what the target was built from.
expect_exact_source() {
    expect_match || return
    m=$(ls -d "$OUT"/output-0-* | head -1)
    if ! diff -w "$m/source.cpp" "$HERE/$1/target.cpp" > "$WORK/srcdiff_$N"; then
        fail "matched, but the source differs from target.cpp:"
        cat "$WORK/srcdiff_$N"
        return
    fi
    echo "ok:   $LABEL: matched source == target.cpp"
}

expect_no_match() {
    if [ "$RC" -eq 0 ]; then fail "matched without the needed pass"; return; fi
    if [ "$RC" -ne 3 ]; then fail "expected 'no match' (exit 3), got exit $RC"; return; fi
    echo "ok:   $LABEL: no match, as expected"
}

# --- var_reorder: four local saves in the wrong order -----------------------
run var_reorder Group::Promote exhaustive -m exhaustive -p reorder_saves
expect_exact_source var_reorder
run var_reorder Group::Promote random -m random -p reorder_saves -n 200
expect_exact_source var_reorder
run var_reorder Group::Promote control -m exhaustive -p swap_operands,invert_if,flip_compare
expect_no_match

# --- combo: saves in the wrong order AND an inverted if ---------------------
# Neither pass alone gets there; the combination does.
run combo Group::Swap single-passes -m exhaustive -p reorder_saves,invert_if
expect_no_match
run combo Group::Swap combo -m exhaustive -p reorder_saves+invert_if
expect_exact_source combo
run combo Group::Swap repeated-p-depth2 -m exhaustive -p reorder_saves -p invert_if --depth 2
expect_exact_source combo
run combo Group::Swap random-combo -m random -p reorder_saves+invert_if -n 300
expect_exact_source combo
run combo Group::Swap combo-plus-noise -m exhaustive -p swap_operands+reorder_saves+flip_compare+invert_if
expect_exact_source combo

# --- inline_local: a local the original didn't have -------------------------
# -j 1: the cast variant compiles to the same code; keep the plain one (tried first)
run inline_local Boost inline -m exhaustive -p inline_local -j 1
expect_exact_source inline_local
run inline_local Boost control -m exhaustive -p reorder_saves,move_stmt
expect_no_match

# --- ops: swapped operands and an inverted if --------------------------------
run ops Sum combo -m exhaustive -p swap_operands+invert_if
expect_match
run ops Sum random -m random -p swap_operands,invert_if,flip_compare -n 400
expect_match

echo
if [ "$FAILED" -ne 0 ]; then
    echo "$FAILED of $N integration checks failed"
    exit 1
fi
echo "all $N integration runs passed"
