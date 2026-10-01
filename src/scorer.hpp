// Disassembles one function from a COFF/ELF object with llvm-objdump and
// scores it against a target (0 = identical after normalisation).
#pragma once

#include <string>
#include <vector>

namespace perm {

struct Insn {
    std::string mnem, args;
    std::string full() const { return args.empty() ? mnem : mnem + " " + args; }
};

struct ScoreConfig {
    std::string objdump = "llvm-objdump";
    bool ignoreRelocNames = false; // compare relocated operands as "<sym>"
    int penaltyRegalloc = 10;      // same instruction, other registers
    int penaltyArgs = 50;          // same mnemonic, other operands
    int penaltyReorder = 60;       // an instruction that moved
    int penaltyInsert = 100;
    int penaltyDelete = 100;
};

bool disassemble(const ScoreConfig& cfg, const std::string& obj, const std::string& symbol,
                 std::vector<Insn>& out, std::string& err);

// Parses llvm-objdump -d -r output for one function. Exposed for tests.
std::vector<Insn> parseObjdump(const std::string& text, const std::string& symbol,
                               bool ignoreRelocNames);

int scoreInsns(const ScoreConfig& cfg, const std::vector<Insn>& target,
               const std::vector<Insn>& cand);

// Side-by-side alignment of target and candidate, for humans.
std::string diffInsns(const ScoreConfig& cfg, const std::vector<Insn>& target,
                      const std::vector<Insn>& cand);

std::vector<std::string> listSymbols(const ScoreConfig& cfg, const std::string& obj,
                                     std::string& err);

// Picks the symbol for a qualified C++ name (MSVC or Itanium mangling, or C).
std::string guessSymbol(const std::vector<std::string>& syms, const std::string& qualName);

} // namespace perm
