#!/bin/sh
# Integration test with the real compiler: MSVC 6 under wine, on gta2_re.
#
# PedGroup::PromoteMemberToLeader_4C9680 is a MATCH_FUNC, so VC6's code for
# the unmodified PedGroup.cpp is the original game's code. We compile that as
# the target, break the function the way a decompiler would (the four weapon
# saves in another order, an if inverted) and check that the permuter finds
# the way back to the exact original source.
#
# Needs GTA2_RE pointing at a gta2_re checkout with its compile tools
# submodule, and wine. Skips (exit 77) otherwise.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/../..
PERMUTER=${PERMUTER:-$ROOT/build/cpp_permuter}
[ -n "${GTA2_RE:-}" ] || { echo "skip: GTA2_RE not set"; exit 77; }
[ -d "$GTA2_RE/3rdParty/gta2_re_compile_tools/VC98" ] || { echo "skip: no VC6 in $GTA2_RE"; exit 77; }
command -v wine >/dev/null || { echo "skip: wine not found"; exit 77; }
command -v llvm-objdump >/dev/null || { echo "skip: llvm-objdump not found"; exit 77; }
command -v python3 >/dev/null || { echo "skip: python3 not found"; exit 77; }
export WINEDEBUG=-all GTA2_RE
wineserver -p 2>/dev/null || true

FN=PedGroup::PromoteMemberToLeader_4C9680
ORIG=$GTA2_RE/Source/PedGroup.cpp
TEST_SRC=$GTA2_RE/Source/PedGroup_permuter_test.cpp
COMPILE="$ROOT/examples/gta2/compile.sh {src} {obj}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; rm -f "$TEST_SRC"' EXIT
FAILED=0
N=0

"$ROOT/examples/gta2/compile.sh" "$ORIG" "$WORK/target.obj" > "$WORK/target.log" 2>&1 ||
    { echo "FAIL: PedGroup.cpp doesn't compile"; cat "$WORK/target.log"; exit 1; }

# mutate <save order, e.g. 3021> <invert: 0/1>: writes TEST_SRC from ORIG
mutate() {
    python3 - "$ORIG" "$TEST_SRC" "$1" "$2" <<'EOF'
import sys
src, dst, order, invert = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"
s = open(src).read()
saves = [
    "    Weapon_30* leaderWeapon = field_2C_ped_leader->field_170_selected_weapon;",
    "    Weapon_30* memberWeapon = field_4_ped_list[idx]->field_170_selected_weapon;",
    "    Weapon_30* leaderWeapon2 = field_2C_ped_leader->field_174_pWeapon;",
    "    Weapon_30* memberWeapon2 = field_4_ped_list[idx]->field_174_pWeapon;",
]
old = "\n".join(saves)
assert old in s, "the saves in PedGroup.cpp changed; update vc6.sh"
s = s.replace(old, "\n".join(saves[int(c)] for c in order))
if invert:
    a = """    if (pTmp->field_238_ped_type == 5)
    {
        pTmp->PoolAllocate();
        pTmp->field_21C |= 0x400;
    }
    else
    {
        pTmp->PoolAllocate();
    }"""
    b = """    if (pTmp->field_238_ped_type != 5)
    {
        pTmp->PoolAllocate();
    }
    else
    {
        pTmp->PoolAllocate();
        pTmp->field_21C |= 0x400;
    }"""
    assert a in s, "the if in PedGroup.cpp changed; update vc6.sh"
    s = s.replace(a, b)
open(dst, "w").write(s)
EOF
}

# run <label> <permuter args...>; sets RC and OUT
run() {
    LABEL=$1
    shift
    N=$((N + 1))
    OUT=$WORK/out_$N
    "$PERMUTER" -s "$TEST_SRC" -f "$FN" -c "$COMPILE" -t "$WORK/target.obj" -j 4 --seed 1 \
        -o "$OUT" "$@" > "$WORK/log_$N" 2>&1
    RC=$?
}

fail() {
    echo "FAIL: $LABEL: $1"
    tr '\r' '\n' < "$WORK/log_$N" | grep -v '^iterations' | tail -30
    FAILED=$((FAILED + 1))
}

expect_original() {
    [ "$RC" -eq 0 ] || { fail "expected a match, exit $RC"; return; }
    b=$(tr '\r' '\n' < "$WORK/log_$N" | sed -n 's/^base score: //p')
    [ -n "$b" ] && [ "$b" -gt 0 ] || { fail "base already matched"; return; }
    m=$(ls -d "$OUT"/output-0-* 2>/dev/null | head -1)
    [ -n "$m" ] || { fail "no output-0-* written"; return; }
    if ! diff -w "$m/source.cpp" "$ORIG" > "$WORK/diff_$N"; then
        fail "matched, but not the original source:"
        cat "$WORK/diff_$N"
        return
    fi
    echo "ok:   $LABEL: base $b -> 0, recovered the original source"
}

expect_no_match() {
    [ "$RC" -eq 3 ] && { echo "ok:   $LABEL: no match, as expected"; return; }
    fail "expected 'no match' (exit 3), got exit $RC"
}

for order in 3021 3210 1032; do
    mutate $order 0
    run "saves order $order, reorder_saves" -m exhaustive -p reorder_saves
    expect_original
done

mutate 3021 0
run "saves order 3021, without reorder_saves" -m exhaustive -p invert_if,swap_operands,flip_compare
expect_no_match

mutate 2301 1
run "saves 2301 + inverted if, single passes" -m exhaustive -p reorder_saves,invert_if
expect_no_match
run "saves 2301 + inverted if, combo" -m exhaustive -p reorder_saves+invert_if
expect_original
run "saves 2301 + inverted if, random combo" -m random -p reorder_saves+invert_if -n 300
expect_original

echo
if [ "$FAILED" -ne 0 ]; then
    echo "$FAILED of $N VC6 integration runs failed"
    exit 1
fi
echo "all $N VC6 integration runs passed"
