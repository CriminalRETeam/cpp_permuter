// Everything the permuter edits: the target function plus, optionally, other
// functions (typically inline helpers in headers) whose code VC6 inlines into
// it. Candidates are compiled in a per-worker mirror of the source tree so
// edited headers are seen by every #include.
#pragma once

#include "parser.hpp"

#include <set>
#include <string>
#include <vector>

namespace perm {

// A function being permuted.
struct Region {
    std::string file;  // absolute path of the file holding it
    size_t start = 0;  // byte range of the definition in the original file
    size_t end = 0;
    std::string name;  // as given or found, e.g. "Ped::GetX"
    // A global's declaration rather than a function: the other form of the line
    // (the extern declaration of a definition, or the definition of an extern).
    std::string alt;
    // A calling convention site (--calling-conventions): the text between a declaration's return
    // type and its name. alts are the four choices (none, __cdecl, __stdcall, __fastcall); regions
    // with the same link (the declarations of one function) always hold the same choice.
    std::vector<std::string> alts;
    int link = -1;
    bool isToggle() const { return !alt.empty() || !alts.empty(); }
};

// Names of the functions called in f's body (the last component: "GetX" for
// "p->GetX()" or "Ped::GetX()").
std::set<std::string> calledNames(const Func& f);

// Files reachable from file through #include "..." lines, resolved against
// the including file's directory and then dirs. Missing files are skipped.
std::vector<std::string> includedFiles(const std::string& file, const std::vector<std::string>& dirs);

// Definitions of the functions main calls that VC6 could inline into it:
// those in headers reachable from srcPath, and those in srcPath itself that are
// declared inline. At most maxRegions, in the order the calls appear.
std::vector<Region> inlineCallees(const std::string& srcPath, size_t mainStart, size_t mainEnd,
                                  const Func& main, const std::vector<std::string>& includeDirs,
                                  size_t maxRegions);

// Replaces each region's range in original with its text. Ranges must not overlap.
// The definitions and extern declarations, in srcText, of the globals named in
// `names`, as regions whose alt is the other form, for --extern-globals. Lines
// look like gta2_re's Function.hpp macros: "<define>[_INIT|_ARRAY...](type, name,
// ...)" and "<extern>[_ARRAY](type, name[, size])". An extern's definition is
// looked for in the other .cpp files of srcPath's directory.
std::vector<Region> globalRegions(const std::string& srcPath, const std::string& srcText,
                                  const std::set<std::string>& names, const std::string& definePrefix,
                                  const std::string& externPrefix);

// The calling convention sites of the functions named in `names` (declarations and definitions
// in `files`), for --calling-conventions. A site is "<return type> [convention] [Class::]name(" at
// the start of a line; its region is the text between the return type and the name. Sites of one
// name share a link number, starting at firstLink.
std::vector<Region> conventionRegions(const std::vector<std::string>& files,
                                      const std::set<std::string>& names, int firstLink);

std::string spliceRegions(const std::string& original,
                          std::vector<std::pair<const Region*, const std::string*>> parts);

// A directory that mirrors root through symlinks, with real copies of the files
// a candidate changes.
class Mirror {
public:
    bool create(const std::string& root, const std::string& dir,
                const std::vector<std::string>& realFiles, std::string& err);
    std::string map(const std::string& original) const; // path inside the mirror
    const std::string& dir() const { return dir_; }

private:
    std::string root_, dir_;
};

} // namespace perm
