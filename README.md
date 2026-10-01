# cpp_permuter

Brute-forces source permutations of a single C++ function until its compiled code matches a
target object, or gets closer to it. It is built for matching decompilation projects such as
[gta2_re](https://github.com/CriminalRETeam/gta2_re) (MSVC 6).

cpp_permuter is heavily inspired by Simon Lindholm's
[decomp-permuter](https://github.com/simonlindholm/decomp-permuter), which does the same for
C. It borrows decomp-permuter's design wholesale: random and exhaustive search,
weighted randomization passes, `PERM_*` macros, an objdump-based scorer, and outputs with a
score and a diff. It is a separate implementation, written in C++, for C++ code compiled by
MSVC. Like decomp-permuter, it is released under the [MIT license](LICENSE).

How it works:

1. Find the function in the `.cpp` and parse its body into statements, declarations and
   expressions.
2. Make candidates by applying mutation passes (reorder local saves, inline a local, move
   declarations in and out of blocks, swap operands, invert an `if`, ...). Optionally do the
   same to the inline helpers the function calls.
3. Splice each candidate back into the file, compile it with your compile command, and
   disassemble the function from the object with `llvm-objdump`.
4. Score it against the target: 0 means identical, lower is closer. Improvements are written
   to the output directory.

## Building

Needs CMake 3.16+ and a C++17 compiler. Scoring also needs `llvm-objdump` on `PATH`.

```sh
cmake -S . -B build -G Ninja      # or leave out -G for make
cmake --build build
ctest --test-dir build            # unit and integration tests (see Tests below)
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
| `-c/--compile` | compile command. `{src}` is the candidate source, `{obj}` the object it must write, `{dir}` the candidate's directory, `{root}` the mirror's root (see below) |
| `-t/--target-obj` | object file holding the target code |
| `--symbol`, `--target-symbol` | symbol names, if the guess from `--function` is wrong (MSVC `?Name@Class@@...`, Itanium, or C) |
| `--score-cmd` | score with your own command instead (`{obj}`, `{src}`). It must print the score (0 = match) as the last number in its output |
| `--ignore-reloc-names` | compare relocated operands without their symbol names, and treat absolute addresses as symbols. Use this when the target came from raw exe asm |
| `-m exhaustive` | try every candidate the selected passes and combos produce. `--depth N` chains N of them, `--max-candidates` caps the count |
| `-m random` | (default) apply 1..`--max-mutations` random passes or combos, starting from the base or from the best so far. Runs until a match, `-n` compiles, or Ctrl-C |
| `-p a,b` | passes to use (default: all). `-p` can be repeated. `a+b` combines passes, see below |
| `-j N` | parallel compiles |
| `--dry-run` | print the candidates as diffs without compiling. A quick check of what a pass would do |
| `--show-ast` | print how the function was parsed |
| `--show-base-diff` | print the target/base asm alignment before starting |
| `--config FILE` | `key = value` lines, same keys as the long options. Handy for one config per function |
| `--keep-going` | keep searching after an exact match |
| `--inline-callees`, `--also [FILE:]NAME` | also permute inline helpers the function calls, see below |
| `--check-parse FILE...` | parse every function in the files and run every pass on each; see Tests |

Each improvement goes to `permuter_out/output-<score>-<n>/`:

- `function.cpp`: the permuted function (and any helper that changed).
- `source.cpp`: the whole file.
- `<header>.hpp`: each header that changed, when helpers were permuted.
- `diff.txt`: what changed.
- `asm_diff.txt`: target and candidate asm aligned side by side.

The exit status is 0 if a match was found and 3 if not.

Candidates are compiled in a mirror of the source's directory under
`permuter_out/.work/w<N>/`, one per worker. The mirror is built from symlinks to every entry,
plus real copies of the files the permuter edits. Your files are never written to. File names
stay the same, and every `#include "..."` resolves inside the mirror, so an edited header is
the one that every file includes. `--mirror-root DIR` mirrors a larger tree when the edited
files aren't all under the source's directory.

## Inline helpers

VC6 inlines small helpers (getters, `Fix16::Abs`, `Max`, ...) into their callers, so a
function can be off only because a helper is written differently. The permuter can work on
those too:

```sh
--inline-callees            # the inline functions this function calls
--also fix16.hpp:Max        # a particular function, by file (relative to . or the source)
--also Fix16::Max           # ... or looked up in the source and the headers it includes
--list-regions              # print what would be permuted, then exit
```

`--inline-callees` looks for definitions of the called names in these places:

- the headers the source reaches through `#include "..."` (searched in the including file's
  directory, then in `-I` dirs);
- the source itself, for functions marked `inline`.

Overloads whose parameter count can't fit any call are skipped. The first definition of
every name is taken before any second overload, up to `--max-callees` (default 8).

Every pass then also works on those helpers. Half of the random mutations go to the target
function, and exhaustive mode tries every pass on every function. Only the target function
is scored. When a candidate matches, the output has the changed headers next to
`source.cpp`.

## Combining passes

A function is often off in more than one way: the saves are in the wrong order *and* an `if`
is the wrong way round. A single pass can't fix both, so passes can be combined:

```sh
-p reorder_saves,invert_if         # each pass on its own: 23 + 4 candidates
-p reorder_saves+invert_if         # a combo: 23 + 4 + 23*4 = 119 candidates
-p reorder_saves -p invert_if+swap_operands   # -p can be repeated
```

In exhaustive mode, a combo `a+b+c` tries every variant of every pass in it, and every
in-order chain of them: a, b, c, a then b, a then c, b then c, and a then b then c. In random
mode a combo applies one mutation from each of its passes. `--depth N` chains whole passes or
combos too: `-p reorder_saves -p invert_if --depth 2` reaches the same candidates as the combo
above, plus each pass applied twice.

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
| `bool_return` | `return a <= b;` and `if (a <= b) return true; return false;` (also with `else`, and with `1`/`0`), both ways |
| `ternary_arg` | `f(c ? a : b);` and `if (c) f(a); else f(b);`, both ways. Going back, two branches that differ in one sub-expression become one statement with a ternary |
| `switch_if` | `if (x == K) S else D` and `switch (x) { case K: S break; default: D }`, both ways (the one-case switch quirk) |
| `early_return` | `if (c) { ...; return; } else { B }` and `if (c) { ...; return; } B`, both ways (also `break`, `continue`, `goto`) |
| `branch_dup` | Move a statement that both branches start or end with out of the `if`, or copy the statement before or after an `if`/`else` into both branches |
| `cond_split` | `if (a && b) S` and `if (a) { if (b) S }`, both ways |
| `explicit_compare` | `if (x)` and `if (x != 0)`, `if (!x)` and `if (x == 0)`, in conditions |
| `negate_const` | `x - 4` and `x + -4`, `x -= 4` and `x += -4` (the `sub` vs `add -N` quirk) |
| `loop_form` | `for (i; c; s) B` and `i; while (c) { B s; }`, `while (c)` and `for (; c;)` |
| `reassociate` | `a + b + c` and `a + (b + c)`, for `+ * & \| ^` |

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

On gta2_re (`--check-parse Source/*.cpp Source/*.hpp`):

- all 4,235 functions in 231 files parse;
- 2 of 53,552 expressions are not understood (a `decltype` cast and a function-pointer cast);
- 4 statements stay barriers (a `typedef`, two `using namespace`, a local `struct`);
- every candidate that every pass produces parses back.

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

## Tests

`ctest --test-dir build` runs four suites.

`unit` (`tests/tests.cpp`) covers:

- the lexer and the parser, including listing every definition in a file;
- the effects analysis;
- every pass, in both directions where it has two;
- pass combos;
- the scorer;
- finding inline callees;
- the mirror, including a source tree made of symlinks.

`integration` (`tests/integration/run.sh`) needs clang and llvm-objdump. It uses clang's
i686 MSVC target at `-O0`, where statement order and expression shape show up directly in the
code. Each case has a `base/` directory that is wrong in some way, and a `target/` directory
that the target object is built from: `main.cpp`, plus any headers. The permuter must match
the target, and where it should, give back exactly the target sources. Negative controls run
without the pass under test (usually: with every other pass) and must *not* match, which
shows the match comes from that pass. The cases are:

- `var_reorder`: local saves out of order.
- `combo`: saves out of order plus an inverted `if`. Single passes fail and the combo matches.
- `inline_local`: a local the original didn't have.
- `ops`: swapped operands.
- One case for each of `bool_return`, `ternary_arg`, `switch_if`, `branch_dup`, `cond_split`,
  `negate_const` and `reassociate`.
- `inline_callee`: the function is right, but the `__forceinline` helper in its header isn't.
  It is found by `--inline-callees`, `--also NAME` and `--also FILE:NAME`. Without them there
  is no match.

`early_return`, `explicit_compare` and `loop_form` compile identically at clang `-O0`, so only
the unit tests cover them.

`gta2_parse` (`tests/integration/gta2_parse.sh`) runs `--check-parse` over every gta2_re
source and header. It fails if any of these happen:

- a function doesn't parse;
- a candidate doesn't parse back, or is a no-op;
- more than 0.1% of expressions, or more than 10 statements, aren't understood.

`vc6` (`tests/integration/vc6.sh`) uses real MSVC 6 under wine, on gta2_re functions that
already match, so VC6's code for them is the original game's code. Each case breaks a
function the way a decompiler might, and checks that the permuter recovers the exact original
source. A control without the needed pass must not match. It works in a symlinked copy of
`Source/` and never modifies the checkout. The cases are:

- `PedGroup::PromoteMemberToLeader_4C9680`: the four weapon saves in three different orders;
  and the saves out of order plus an inverted `if`, which only the combo fixes.
- `PedGroup::RemovePed_4C9970`: `ternary_arg`, the `if`/`else` around a call (from
  `matching_quirks.md`).
- `sound_obj::IsTrainOrBoxcar_57F120`: `bool_return` (from `matching_quirks.md`).
- `Particle_4C::UpdateShortAnim_state_37_53B580`: `switch_if`.
- `Fix16::Max` with its comparison mirrored, and `Fix16::Abs` written as a ternary, in
  `fix16.hpp`, as inlined into `PedGroup` functions. They are recovered through
  `--inline-callees` and `--also`; permuting only the calling function doesn't match.
- 60 random candidates from every pass on `RemovePed`: at most 10% may fail to compile.
  None did.

`gta2_parse` and `vc6` need `GTA2_RE` set to a gta2_re checkout, and `vc6` also needs wine.
Without them they are reported as skipped.

## gta2_re

`examples/gta2/` has a VC6-under-wine compile script and a walkthrough. See
[examples/gta2/README.md](examples/gta2/README.md).
