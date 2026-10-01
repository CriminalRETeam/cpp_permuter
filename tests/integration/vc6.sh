#!/bin/sh
# Integration tests with the real compiler: MSVC 6 under wine, on gta2_re.
#
# Every function used here is a MATCH_FUNC, so VC6's code for the unmodified
# gta2_re source is the original game's code. Each case compiles that as the
# target, breaks the function (or an inline helper it calls) the way a
# decompiler would, and checks that the permuter finds the way back to exactly
# the original source. A control run without the needed pass must not match.
#
# The work happens in a copy of Source/ made of symlinks, so the gta2_re
# checkout is never modified.
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

ORIG=$GTA2_RE/Source
COMPILE="$ROOT/examples/gta2/compile.sh {src} {obj}"
WORK=$(mktemp -d)
SRC=$WORK/Source
trap 'rm -rf "$WORK"' EXIT
FAILED=0
N=0

# target <file.cpp>: the original TU's object, compiled once
target() {
    obj=$WORK/$1.target.obj
    if [ ! -f "$obj" ]; then
        "$ROOT/examples/gta2/compile.sh" "$ORIG/$1" "$obj" > "$WORK/target.log" 2>&1 ||
            { echo "FAIL: $1 doesn't compile"; cat "$WORK/target.log"; exit 1; }
    fi
    echo "$obj"
}

# fresh: a pristine symlinked copy of Source/
fresh() {
    rm -rf "$SRC"
    mkdir -p "$SRC"
    for e in "$ORIG"/*; do ln -s "$e" "$SRC/"; done
}

# mutate <file>: replaces the text before the "====" line on stdin with the
# text after it, in a private copy of Source/<file>
mutate() {
    f=$SRC/$1
    [ -L "$f" ] && cp --remove-destination "$(readlink -f "$f")" "$f"
    python3 -c '
import sys
path = sys.argv[1]
old, new = sys.stdin.read().split("\n====\n")
s = open(path).read()
old, new = old.strip("\n"), new.strip("\n")
assert old in s, "%s: text to replace not found; gta2_re changed, update vc6.sh" % path
open(path, "w").write(s.replace(old, new, 1))
' "$f"
}

# run <label> <file.cpp> <function> <permuter args...>; sets RC and OUT
run() {
    LABEL=$1 CPP=$2 FN=$3
    shift 3
    N=$((N + 1))
    OUT=$WORK/out_$N
    "$PERMUTER" -s "$SRC/$CPP" -f "$FN" -c "$COMPILE" -t "$(target "$CPP")" -j 4 --seed 1 \
        -o "$OUT" "$@" > "$WORK/log_$N" 2>&1
    RC=$?
}

fail() {
    echo "FAIL: $LABEL: $1"
    tr '\r' '\n' < "$WORK/log_$N" | grep -v '^iterations' | tail -30
    FAILED=$((FAILED + 1))
}

# A match whose sources are the original ones: the .cpp, and each header named.
expect_original() {
    [ "$RC" -eq 0 ] || { fail "expected a match, exit $RC"; return; }
    b=$(tr '\r' '\n' < "$WORK/log_$N" | sed -n 's/^base score: //p')
    [ -n "$b" ] && [ "$b" -gt 0 ] || { fail "base already matched: this change doesn't alter VC6's code"; return; }
    m=$(ls -d "$OUT"/output-0-* 2>/dev/null | head -1)
    [ -n "$m" ] || { fail "no output-0-* written"; return; }
    if ! diff -w "$m/source.cpp" "$ORIG/$CPP" > "$WORK/diff_$N"; then
        fail "matched, but not the original $CPP:"
        cat "$WORK/diff_$N"
        return
    fi
    for h in "$@"; do
        if ! diff -w "$m/$h" "$ORIG/$h" > "$WORK/diff_$N" 2>&1; then
            fail "matched, but not the original $h:"
            cat "$WORK/diff_$N"
            return
        fi
    done
    echo "ok:   $LABEL: base $b -> 0, recovered the original source"
}

expect_no_match() {
    [ "$RC" -eq 3 ] && { echo "ok:   $LABEL: no match, as expected"; return; }
    fail "expected 'no match' (exit 3), got exit $RC"
}

# Every pass except the ones named.
all_but() {
    "$PERMUTER" --list-passes | sed -n 's/^  \([a-z_]*\)$/\1/p' | grep -v -x -F "$(printf '%s\n' "$@")" |
        paste -sd, -
}

SAVES='    Weapon_30* leaderWeapon = field_2C_ped_leader->field_170_selected_weapon;
    Weapon_30* memberWeapon = field_4_ped_list[idx]->field_170_selected_weapon;
    Weapon_30* leaderWeapon2 = field_2C_ped_leader->field_174_pWeapon;
    Weapon_30* memberWeapon2 = field_4_ped_list[idx]->field_174_pWeapon;'
# reordered <order>: the four saves in that order, e.g. 3021
reordered() { printf '%s\n' "$SAVES" | python3 -c "import sys; l=sys.stdin.read().split('\n'); print('\n'.join(l[int(c)] for c in '$1'))"; }

# --- reorder_saves: PedGroup::PromoteMemberToLeader_4C9680 ------------------
PROMOTE=PedGroup::PromoteMemberToLeader_4C9680
for order in 3021 3210 1032; do
    fresh
    printf '%s\n====\n%s\n' "$SAVES" "$(reordered $order)" | mutate PedGroup.cpp
    run "reorder_saves: saves in order $order" PedGroup.cpp $PROMOTE -m exhaustive -p reorder_saves
    expect_original
done
run "reorder_saves: control" PedGroup.cpp $PROMOTE -m exhaustive -p "$(all_but reorder_saves move_stmt)"
expect_no_match

# --- combo: the saves out of order and an inverted if ------------------------
fresh
printf '%s\n====\n%s\n' "$SAVES" "$(reordered 2301)" | mutate PedGroup.cpp
mutate PedGroup.cpp <<'EOF'
    if (pTmp->field_238_ped_type == 5)
    {
        pTmp->PoolAllocate();
        pTmp->field_21C |= 0x400;
    }
    else
    {
        pTmp->PoolAllocate();
    }
====
    if (pTmp->field_238_ped_type != 5)
    {
        pTmp->PoolAllocate();
    }
    else
    {
        pTmp->PoolAllocate();
        pTmp->field_21C |= 0x400;
    }
EOF
run "combo: single passes" PedGroup.cpp $PROMOTE -m exhaustive -p reorder_saves,invert_if
expect_no_match
run "combo: reorder_saves+invert_if" PedGroup.cpp $PROMOTE -m exhaustive -p reorder_saves+invert_if
expect_original
run "combo: random" PedGroup.cpp $PROMOTE -m random -p reorder_saves+invert_if -n 300
expect_original

# --- ternary_arg: PedGroup::RemovePed_4C9970 (matching_quirks.md) ------------
fresh
mutate PedGroup.cpp <<'EOF'
                if (field_2C_ped_leader->field_25C_internal_objective == 0)
                {
                    field_2C_ped_leader->sub_4633E0(1);
                }
                else
                {
                    field_2C_ped_leader->sub_4633E0(0);
                }
====
                field_2C_ped_leader->sub_4633E0(field_2C_ped_leader->field_25C_internal_objective == 0 ? 1 : 0);
EOF
run "ternary_arg: RemovePed" PedGroup.cpp PedGroup::RemovePed_4C9970 -m exhaustive -p ternary_arg
expect_original
run "ternary_arg: control" PedGroup.cpp PedGroup::RemovePed_4C9970 -m exhaustive -p "$(all_but ternary_arg ternary)"
expect_no_match

# --- bool_return: sound_obj::IsTrainOrBoxcar_57F120 (matching_quirks.md) -----
fresh
mutate sound_obj.cpp <<'EOF'
    if (pCar->field_84_car_info_idx == car_model_enum::boxcar ||
        (pCar->field_84_car_info_idx > car_model_enum::TOWTRUCK && pCar->field_84_car_info_idx <= car_model_enum::TRAINFB))
    {
        return true;
    }
    return false;
====
    return pCar->field_84_car_info_idx == car_model_enum::boxcar ||
        (pCar->field_84_car_info_idx > car_model_enum::TOWTRUCK && pCar->field_84_car_info_idx <= car_model_enum::TRAINFB);
EOF
run "bool_return: IsTrainOrBoxcar" sound_obj.cpp sound_obj::IsTrainOrBoxcar_57F120 -m exhaustive -p bool_return -j 1
expect_original
run "bool_return: control" sound_obj.cpp sound_obj::IsTrainOrBoxcar_57F120 -m exhaustive -p "$(all_but bool_return)"
expect_no_match

# --- switch_if: Particle_4C::UpdateShortAnim_state_37_53B580 -----------------
fresh
mutate Particle_4C.cpp <<'EOF'
    switch (field_46_sub_state)
    {
        case 1:
            this->field_30_pNext->set_id_lazy_4206C0(field_30_pNext->field_22_sprite_id);
            break;

        default:
            this->field_30_pNext->set_id_4206E0(field_30_pNext->field_22_sprite_id + 4);
            break;
    }
====
    if (field_46_sub_state == 1)
    {
        this->field_30_pNext->set_id_lazy_4206C0(field_30_pNext->field_22_sprite_id);
    }
    else
    {
        this->field_30_pNext->set_id_4206E0(field_30_pNext->field_22_sprite_id + 4);
    }
EOF
ANIM=Particle_4C::UpdateShortAnim_state_37_53B580
run "switch_if: UpdateShortAnim" Particle_4C.cpp $ANIM -m exhaustive -p switch_if
# the case body's blank line isn't recreated, so compare the code only
[ "$RC" -eq 0 ] && grep -q "case 1:" "$OUT"/output-0-*/function.cpp && echo "ok:   switch_if: UpdateShortAnim: matched with a switch" ||
    fail "expected a match with a switch, exit $RC"
run "switch_if: control" Particle_4C.cpp $ANIM -m exhaustive -p "$(all_but switch_if)"
expect_no_match

# --- inline callees: helpers in fix16.hpp that VC6 inlines into PedGroup ----
# Fix16::Max with its comparison mirrored; only flip_compare on the helper
# (not on the calling function) gets it back.
fresh
mutate fix16.hpp <<'EOF'
        return (diff_x > diff_y) ? diff_x : diff_y;
====
        return (diff_y < diff_x) ? diff_x : diff_y;
EOF
TIGHT=PedGroup::UpdateMemberTightFollowState_4CA820
run "inline callee: Fix16::Max via --inline-callees" PedGroup.cpp $TIGHT -m exhaustive -p flip_compare --inline-callees
expect_original fix16.hpp
run "inline callee: Fix16::Max via --also" PedGroup.cpp $TIGHT -m exhaustive -p flip_compare --also fix16.hpp:Max
expect_original fix16.hpp
run "inline callee: Fix16::Max, control (calling function only)" PedGroup.cpp $TIGHT -m exhaustive
expect_no_match

# Fix16::Abs written as a ternary; the original has if/else.
fresh
mutate fix16.hpp <<'EOF'
        if (input.mValue > 0)
        {
            return input;
        }
        else
        {
            return -input;
        }
====
        return input.mValue > 0 ? input : -input;
EOF
FAR=PedGroup::IsMemberTooFarFromLeader_4CAC20
run "inline callee: Fix16::Abs via --inline-callees" PedGroup.cpp $FAR -m exhaustive -p ternary --inline-callees
expect_original fix16.hpp
run "inline callee: Fix16::Abs, random" PedGroup.cpp $FAR -m random -p ternary,flip_compare,invert_if --inline-callees -n 60
expect_original fix16.hpp
run "inline callee: Fix16::Abs, control (calling function only)" PedGroup.cpp $FAR -m exhaustive
expect_no_match

# --- candidates VC6 accepts: random mutations from every pass ---------------
# The parse check (gta2_parse.sh) only shows candidates are well formed; this
# shows VC6 compiles nearly all of them. RemovePed has nested ifs, loops,
# locals and calls, so most passes have something to do. It starts from the
# ternary version above so the run doesn't stop at "already matching".
fresh
mutate PedGroup.cpp <<'EOF'
                if (field_2C_ped_leader->field_25C_internal_objective == 0)
                {
                    field_2C_ped_leader->sub_4633E0(1);
                }
                else
                {
                    field_2C_ped_leader->sub_4633E0(0);
                }
====
                field_2C_ped_leader->sub_4633E0(field_2C_ped_leader->field_25C_internal_objective == 0 ? 1 : 0);
EOF
LABEL="compile rate: RemovePed, all passes, 60 random candidates"
run "$LABEL" PedGroup.cpp PedGroup::RemovePed_4C9970 -m random -n 60 --max-mutations 2 --keep-going
set -- $(tr '\r' '\n' < "$WORK/log_$N" | sed -n 's/.*after \([0-9]*\) compiles (\([0-9]*\) failed).*/\1 \2/p')
if [ $# -ne 2 ] || [ "$1" -lt 50 ]; then
    fail "didn't run (exit $RC)"
elif [ $(($2 * 10)) -gt "$1" ]; then
    fail "$2 of $1 candidates didn't compile (more than 10%)"
else
    echo "ok:   $LABEL: $2 of $1 failed to compile"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "$FAILED of $N VC6 integration runs failed"
    exit 1
fi
echo "all $N VC6 integration runs passed"
