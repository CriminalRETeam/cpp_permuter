#!/bin/sh
# Integration tests. Each case directory holds base/ (the "decompiled"
# source, wrong somewhere) and target/ (the source the target object is built
# from); main.cpp holds the function, other files are headers it includes. The
# permuter has to turn base/ into code that compiles to exactly the target, and
# where the case says so, into exactly the target source.
#
# The compiler is clang's i686 MSVC target at -O0, standing in for VC6: at -O0
# statement order and expression shape show up directly in the code. See
# vc6.sh for the same kind of checks with the real VC6 on gta2_re.
#
# Negative controls run without the pass under test (usually: every other
# pass) and must NOT match, which shows the match comes from that pass.
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

# Every pass except the ones named.
all_but() {
    "$PERMUTER" --list-passes | sed -n 's/^  \([a-z_]*\)$/\1/p' | grep -v -x -F "$(printf '%s\n' "$@")" |
        paste -sd, -
}

# run <case> <function> <name> <permuter args...>; sets RC and OUT
run() {
    case_=$1 fn=$2 name=$3
    shift 3
    N=$((N + 1))
    OUT="$WORK/out_$N"
    mkdir -p "$WORK/src_$N"
    cp "$HERE/$case_"/base/* "$WORK/src_$N/"
    $CXX --target=i686-pc-windows-msvc -O0 -c "$HERE/$case_/target/main.cpp" -o "$WORK/target_$N.obj" || exit 1
    "$PERMUTER" -s "$WORK/src_$N/main.cpp" -f "$fn" -c "$COMPILE" -t "$WORK/target_$N.obj" \
        -j 2 --seed 1 -o "$OUT" "$@" > "$WORK/log_$N" 2>&1
    RC=$?
    LABEL="$case_: $name"
    CASE=$case_
}

fail() {
    echo "FAIL: $LABEL: $1"
    tr '\r' '\n' < "$WORK/log_$N" | grep -v '^iterations' | tail -30
    FAILED=$((FAILED + 1))
}

base_score() { tr '\r' '\n' < "$WORK/log_$N" | sed -n 's/^base score: //p'; }

# An exact match (exit 0, score 0) from a base that didn't match.
expect_match() {
    if [ "$RC" -ne 0 ]; then fail "expected a match, exit $RC"; return 1; fi
    b=$(base_score)
    if [ -z "$b" ] || [ "$b" -eq 0 ]; then fail "base already matched (score '$b')"; return 1; fi
    if ! ls -d "$OUT"/output-0-* >/dev/null 2>&1; then fail "no output-0-* written"; return 1; fi
    echo "ok:   $LABEL: base $b -> 0"
}

# Like expect_match, and the matched sources are the target sources (ignoring
# whitespace): main.cpp, and every header that differs between base and target.
expect_exact_source() {
    expect_match || return
    m=$(ls -d "$OUT"/output-0-* | head -1)
    if ! diff -w -B "$m/source.cpp" "$HERE/$CASE/target/main.cpp" > "$WORK/srcdiff_$N"; then
        fail "matched, but main.cpp differs from the target:"
        cat "$WORK/srcdiff_$N"
        return
    fi
    for t in "$HERE/$CASE"/target/*; do
        h=$(basename "$t")
        [ "$h" = main.cpp ] && continue
        cmp -s "$t" "$HERE/$CASE/base/$h" && continue
        if [ ! -f "$m/$h" ] || ! diff -w "$m/$h" "$t" > "$WORK/srcdiff_$N"; then
            fail "matched, but $h isn't the target's:"
            cat "$WORK/srcdiff_$N" 2>/dev/null
            return
        fi
    done
    echo "ok:   $LABEL: matched source == target"
}

expect_no_match() {
    if [ "$RC" -eq 0 ]; then fail "matched without the needed pass"; return; fi
    if [ "$RC" -ne 3 ]; then fail "expected 'no match' (exit 3), got exit $RC"; return; fi
    echo "ok:   $LABEL: no match, as expected"
}

# --- var_reorder: four local saves in the wrong order -----------------------
run var_reorder Group::Promote exhaustive -m exhaustive -p reorder_saves
expect_exact_source
run var_reorder Group::Promote random -m random -p reorder_saves -n 200
expect_exact_source
run var_reorder Group::Promote control -m exhaustive -p "$(all_but reorder_saves move_stmt)"
expect_no_match

# --- combo: saves in the wrong order AND an inverted if ---------------------
# Neither pass alone gets there; the combination does.
run combo Group::Swap single-passes -m exhaustive -p reorder_saves,invert_if
expect_no_match
run combo Group::Swap combo -m exhaustive -p reorder_saves+invert_if
expect_exact_source
run combo Group::Swap repeated-p-depth2 -m exhaustive -p reorder_saves -p invert_if --depth 2
expect_exact_source
run combo Group::Swap random-combo -m random -p reorder_saves+invert_if -n 300
expect_exact_source
run combo Group::Swap combo-plus-noise -m exhaustive -p swap_operands+reorder_saves+flip_compare+invert_if
expect_exact_source

# --- inline_local: a local the original didn't have -------------------------
# -j 1: the cast variant compiles to the same code; keep the plain one (tried first)
run inline_local Boost inline -m exhaustive -p inline_local -j 1
expect_exact_source
run inline_local Boost control -m exhaustive -p "$(all_but inline_local)"
expect_no_match

# --- ops: swapped operands and an inverted if --------------------------------
run ops Sum combo -m exhaustive -p swap_operands+invert_if
expect_match
run ops Sum random -m random -p swap_operands,invert_if,flip_compare -n 400
expect_match

# --- one case per pass ----------------------------------------------------------
# chain_assign: at -O0 "a = b = 0;" is "b = 0; a = 0;", which move_stmt reaches too
# bool_return: at clang -O0 the else/1-0 forms compile the same as the target's,
# so any of them is a real match (vc6.sh checks the exact form with VC6).
run bool_return IsReady exhaustive -m exhaustive -p bool_return
expect_match
run bool_return IsReady control -m exhaustive -p "$(all_but bool_return)"
expect_no_match
for c in ternary_arg:Notify switch_if:OnCommand branch_dup:Update \
         cond_split:Check negate_const:Offset reassociate:Sum inequalities:Check \
         chain_assign:Clear:move_stmt local_type:Count locals_to_array:Pair reorder_cases:Pick \
         split_case_labels:Pick use_getter:Next temp_for_expr:Send remove_stmt:Finish; do
    # pass:function[:other pass that reaches the same code, left out of the control]
    pass=${c%%:*} rest=${c#*:} fn=${rest%%:*} also=""
    [ "$rest" != "$fn" ] && also=${rest#*:}
    run "$pass" "$fn" exhaustive -m exhaustive -p "$pass"
    expect_exact_source
    run "$pass" "$fn" random -m random -p "$pass" -n 50
    # where an equivalent form exists, random mode may land on it instead
    if [ -n "$also" ]; then expect_match; else expect_exact_source; fi
    run "$pass" "$fn" control -m exhaustive -p "$(all_but "$pass" $also)"
    expect_no_match
done

# --- PERM macros: the alternatives written into the function ---------------
# 3! line orders x 3 values; without PERM_RANDOMIZE only the macros are tried
run perm_macros Run exhaustive -m exhaustive
expect_exact_source
grep -q "^17 candidates generated" "$WORK/log_$N" || fail "expected 17 candidates (18 expansions less the base)"
run perm_macros Run random -m random -n 60
expect_exact_source
run perm_macros Run control -m exhaustive -p none --max-candidates 5
expect_no_match

# --- inline_callee: the function is fine, the inline helper it calls isn't --
run inline_callee Score inline-callees -m exhaustive -p swap_operands --inline-callees
expect_exact_source
run inline_callee Score also -m exhaustive -p swap_operands --also Weight
expect_exact_source
run inline_callee Score also-file -m exhaustive -p swap_operands --also helper.hpp:Weight
expect_exact_source
# random runs can reach an equivalent helper ("4 * x + y" compiles the same at
# -O0), so this one only needs a match
run inline_callee Score random -m random -p swap_operands,flip_compare --inline-callees -n 100
expect_match
run inline_callee Score control -m exhaustive
expect_no_match

echo
if [ "$FAILED" -ne 0 ]; then
    echo "$FAILED of $N integration runs failed"
    exit 1
fi
echo "all $N integration runs passed"
