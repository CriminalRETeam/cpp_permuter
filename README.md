# cpp_permuter

Brute-forces source permutations of a single C++ function until its compiled code matches a
target object, or gets closer to it. It is built for matching decompilation projects such as
[gta2_re](https://github.com/CriminalRETeam/gta2_re) (MSVC 6), and works like
[decomp-permuter](https://github.com/simonlindholm/decomp-permuter), but for C++ and written
in C++.

How it works:

1. Find the function in the `.cpp` and parse its body into statements, declarations and
   expressions.
2. Make candidates by applying mutation passes (reorder local saves, inline a local, move
   declarations in and out of blocks, swap operands, invert an `if`, ...).
3. Splice each candidate back into the file, compile it with your compile command, and
   disassemble the function from the object with `llvm-objdump`.
4. Score it against the target: 0 means identical, lower is closer. Improvements are written
   to the output directory.

## Building

Needs CMake 3.16+ and a C++17 compiler. Scoring also needs `llvm-objdump` on `PATH`.

```sh
cmake -S . -B build -G Ninja      # or leave out -G for make
cmake --build build
ctest --test-dir build            # unit tests, plus end-to-end tests with clang
```

## Usage

```sh
cpp_permuter -s Source/PedGroup.cpp -f PedGroup::PromoteMemberToLeader_4C9680 \
    -c 'examples/gta2/compile.sh {src} {obj}' \
    -t Scripts/asm/PedGroup.cpp.obj \
    -m exhaustive -p reorder_saves -j 4
```

| option | meaning |
|---|---|
| `-s/--source` | the `.cpp` holding the function |
| `-f/--function` | `Class::Method`, `Method`, `Class::Class`, `Class::~Class` |
| `-c/--compile` | compile command. `{src}` is the candidate source, `{obj}` the object it must write, `{dir}` the original source's directory |
| `-t/--target-obj` | object file holding the target code |
| `--symbol`, `--target-symbol` | symbol names, if the guess from `--function` is wrong (MSVC `?Name@Class@@...`, Itanium, or C) |
| `--score-cmd` | score with your own command instead (`{obj}`, `{src}`). It must print the score (0 = match) as the last number in its output |
| `--ignore-reloc-names` | compare relocated operands without their symbol names, and treat absolute addresses as symbols. Use this when the target came from raw exe asm |
| `-m exhaustive` | try every candidate the selected passes produce. `--depth N` chains N mutations, `--max-candidates` caps the count |
| `-m random` | (default) apply 1..`--max-mutations` random mutations, starting from the base or from the best so far. Runs until a match, `-n` compiles, or Ctrl-C |
| `-p a,b` | passes to use (default: all) |
| `-j N` | parallel compiles |
| `--dry-run` | print the candidates as diffs without compiling. A quick check of what a pass would do |
| `--show-ast` | print how the function was parsed |
| `--show-base-diff` | print the target/base asm alignment before starting |
| `--config FILE` | `key = value` lines, same keys as the long options. Handy for one config per function |
| `--keep-going` | keep searching after an exact match |

Each improvement goes to `permuter_out/output-<score>-<n>/`:

- `function.cpp`: the permuted function.
- `source.cpp`: the whole file.
- `diff.txt`: what changed.
- `asm_diff.txt`: target and candidate asm aligned side by side.

The exit status is 0 if a match was found and 3 if not.

Candidate sources are written next to the original (`PedGroup.permuter0.cpp`, ...) so relative
`#include`s still resolve. They are deleted on exit. `--candidate-dir` puts them somewhere
else.

## Passes

`cpp_permuter --list-passes` prints these:

| pass | what it does |
|---|---|
| `reorder_saves` | Runs of consecutive "local saves" (declarations, or assignments to locals, that don't call or store) in every order that keeps dependencies. Four independent saves give 23 new orders (the `PedGroup::PromoteMemberToLeader_4C9680` quirk) |
| `move_stmt` | Move one statement up or down past neighbours it doesn't depend on. Field loads and stores are assumed not to alias; locals and calls are respected |
| `inline_local` | Remove a local by putting its initializer into its uses, plainly and with a cast to the local's type. A local that is never used is dropped |
| `sink_decl` | Move a declaration into the innermost block that holds all its uses, or to just before its first use |
| `hoist_decl` | Move a declaration to the top of its block or of the function, splitting `T x = e;` into `T x;` plus `x = e;`. Also offers one candidate that hoists every declaration at once (C89 style) |
| `merge_decl` | `T x; ... x = e;` becomes `T x = e;` at the assignment |
| `swap_operands` | `a + b` becomes `b + a`, for `+ * & \| ^ == !=` |
| `flip_compare` | `a < b` becomes `b > a` |
| `invert_if` | `if (c) A else B` becomes `if (!c) B else A`, negating comparisons directly |
| `ternary` | `x = c ? a : b` and `return c ? a : b` become `if`/`else` (and an early-return form), and back |
| `compound_assign` | `x = x op y` and `x op= y`, both ways |

The passes aim to keep behaviour the same, but they don't prove it. For example they assume
that two different fields don't alias. That is fine here, because a candidate that matches the
target byte for byte is correct by construction. A rewrite that changes behaviour just won't
match.

## The parser

This is not a full C++ front end, and that is deliberate. It works on the unpreprocessed
source and only rewrites the body of the target function. Everything else in the file stays
byte-identical, so candidates go through cl.exe with the original includes and macros. Inside
the body it recognises statements, local declarations and expressions. Anything it can't
classify (`__asm`, unknown macros, local types) becomes a fixed barrier that is never moved
across. If it misreads something, that candidate fails to compile and is skipped.

Running clang on the whole file instead would mean the code has to compile under clang. MSVC 6
code with its own headers, `__asm` and naked marker functions doesn't, and clang's error
recovery drops the very declarations we want to move.

## Scoring

The function is disassembled with `llvm-objdump -d -r --disassemble-symbols=<sym>`. Before
comparing:

- relocated operands are replaced with their symbol;
- branch targets become instruction indices, so a size change upstream doesn't change every
  jump;
- `<...>` annotations are stripped.

Target and candidate are then aligned with an edit distance. Penalties: 10 for a register
difference, 50 for other operand differences, 60 for a moved instruction, 100 for an insertion
or a deletion. The weights are the same idea as decomp-permuter's.

## gta2_re

`examples/gta2/` has a VC6-under-wine compile script and a walkthrough. See
[examples/gta2/README.md](examples/gta2/README.md).
