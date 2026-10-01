// decomp-permuter's PERM_* macros: hand-written alternatives inside the
// function, expanded before anything is compiled.
//
//   PERM_GENERAL(a, b, ...)   one of a, b, ...
//   PERM_LINESWAP(lines)      the non-blank lines in any order
//   PERM_INT(lo, hi)          an integer from lo to hi
//   PERM_ONCE([key,] code)    code at exactly one of the places using key
//   PERM_VAR(a, b) / (a)      set / read the meta-variable a
//   PERM_RANDOMIZE(code)      code; also lets the passes run (see below)
//   PERM_FORCE_SAMELINE(code) code joined onto one line
//   PERM_IGNORE(code)         code, unexpanded
//   PERM_PRETEND(code)        nothing (it only helps decomp-permuter's C parser)
//
// Arguments are split at top-level commas; "(,)" is a literal comma. Macros
// nest. Like decomp-permuter, a function using any PERM macro is only permuted
// through them unless it also has PERM_RANDOMIZE, which turns the passes on
// (for the whole function, not just that region).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace perm {

class MacroExpander {
public:
    explicit MacroExpander(const std::string& text);

    bool hasMacros() const { return hasMacros_; }
    bool randomize() const { return randomize_; } // PERM_RANDOMIZE used
    const std::string& error() const { return error_; }

    // One expansion; choose(n) picks one of n options at each choice point.
    std::string expand(const std::function<int(int)>& choose) const;
    // The expansion taking the first option everywhere.
    std::string first() const;
    // Every expansion (each choice combination once), until emit returns false
    // or cap expansions were produced.
    void enumerate(size_t cap, const std::function<bool(const std::string&)>& emit) const;

    struct Node;

private:
    std::shared_ptr<Node> root_;
    bool hasMacros_ = false, randomize_ = false;
    std::string error_;
};

} // namespace perm
