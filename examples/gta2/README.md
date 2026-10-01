# Using cpp_permuter with gta2_re

## Setup (Linux)

Set up wine and the gta2_re VC6 toolchain as gta2_re's `CLAUDE.md` describes, so the
`3rdParty/gta2_re_compile_tools` submodule is checked out. Then:

```sh
export GTA2_RE=/path/to/gta2_re
export WINEDEBUG=-all
wineserver -p   # keep wineserver running: compiles drop from seconds to well under one
```

Check that a single TU builds:

```sh
examples/gta2/compile.sh $GTA2_RE/Source/PedGroup.cpp /tmp/PedGroup.obj
```

`compile.sh` uses the `gta2_lib` flags (`/O2 /GX /ML ...`). Files that `cmake/vc6.cmake` gives
extra flags need `EXTRA_CFLAGS`, for example `EXTRA_CFLAGS=/Gz` for `Network_20324.cpp`.

## A target object

The permuter compares against an object file that holds the original function. It needs the
original `10.5.exe` in `Scripts/bin_comp/`, as for objdiff:

```sh
cd $GTA2_RE
python3 Scripts/generate_target_asm_for_objs.py PedGroup.cpp   # writes Scripts/asm/PedGroup.cpp.asm + make_objs.sh
(cd Scripts/asm && ./make_objs.sh)                              # -> Scripts/asm/PedGroup.cpp.obj
```

Calls in that asm are resolved to mangled names, so they compare with the symbols in our
object. If some operands are still raw addresses from the exe, add `--ignore-reloc-names`.

## Running

Remove `WIP_IMPLEMENTED` / `NOT_IMPLEMENTED` from the function first, since they add code.

```sh
cpp_permuter -s $GTA2_RE/Source/PedGroup.cpp -f PedGroup::SomeFunction_4C9xxx \
    -c "$PWD/examples/gta2/compile.sh {src} {obj}" \
    -t $GTA2_RE/Scripts/asm/PedGroup.cpp.obj \
    --show-base-diff -j 4
```

Or put the options in a file (see `PromoteMemberToLeader.conf`) and run
`cpp_permuter --config PromoteMemberToLeader.conf`.

Suggestions:

- When you already suspect the kind of fix, use exhaustive mode with just that pass:
  `-m exhaustive -p reorder_saves` tries every order of every run of local saves.
- When it's off in two ways at once, combine the passes: `-p reorder_saves+invert_if` tries
  each pass alone and every pair of their variants. `-p` can be repeated to run several
  combos in one go.
- `--dry-run` shows what a pass would produce without compiling anything.
- Random mode (the default) mixes all passes and is good for leaving running. Add
  `-p reorder_saves,move_stmt` and similar to focus it.
- Results go to `permuter_out/output-<score>-<n>/`. Copy the function back from
  `function.cpp`, then run `build.py` and `compare_builds.py` as usual.

## Checking the setup without the original exe

A `MATCH_FUNC` is its own target. Compile the unmodified TU, scramble the function, and the
permuter should find the way back:

```sh
examples/gta2/compile.sh $GTA2_RE/Source/PedGroup.cpp /tmp/target.obj
# swap a few lines of PedGroup::PromoteMemberToLeader_4C9680 in a copy of the file, then
cpp_permuter -s $GTA2_RE/Source/PedGroup_scrambled.cpp -f PedGroup::PromoteMemberToLeader_4C9680 \
    -c "$PWD/examples/gta2/compile.sh {src} {obj}" -t /tmp/target.obj \
    -m exhaustive -p reorder_saves -j 4
```

With the four weapon saves scrambled (base score 520), this tried the other 23 orders and found
the matching one in about 4 seconds.
