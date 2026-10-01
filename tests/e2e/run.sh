#!/bin/sh
# End-to-end: permute a wrongly ordered source until it compiles to the
# target. Uses clang's i686 MSVC target at -O0 as a stand-in for VC6 (at -O0
# the statement order shows up directly in the code).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PERMUTER=${PERMUTER:-$HERE/../../build/cpp_permuter}
CXX=${CXX_E2E:-clang++}
command -v "$CXX" >/dev/null || { echo "skip: $CXX not found"; exit 0; }
command -v llvm-objdump >/dev/null || { echo "skip: llvm-objdump not found"; exit 0; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
COMPILE="$CXX --target=i686-pc-windows-msvc -O0 -c {src} -o {obj}"

check() { # name function mode passes [extra args]
    cp "$HERE/$1_base.cpp" "$WORK/$1.cpp"
    $CXX --target=i686-pc-windows-msvc -O0 -c "$HERE/$1_target.cpp" -o "$WORK/$1_target.obj"
    if "$PERMUTER" -s "$WORK/$1.cpp" -f "$2" -c "$COMPILE" -t "$WORK/$1_target.obj" \
        -m "$3" -p "$4" -j 2 -n 400 --seed 1 -o "$WORK/out_$1_$3" $5 > "$WORK/log" 2>&1; then
        echo "ok: $1 ($3)"
    else
        echo "FAIL: $1 ($3)"
        cat "$WORK/log"
        exit 1
    fi
}

check saves Group::Promote exhaustive reorder_saves
check saves Group::Promote random reorder_saves,move_stmt
check ops Sum exhaustive swap_operands,invert_if "--depth 2"
check ops Sum random swap_operands,invert_if,flip_compare
