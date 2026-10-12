// Source mutation passes. Each pass looks at a parsed function and offers
// a set of rewrites ("mutations"); a mutation produces the new function text.
#pragma once

#include "parser.hpp"

#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace perm {

using Rng = std::mt19937_64;
using Mutation = std::function<std::string()>;
using EmitFn = std::function<bool(Mutation)>; // return false to stop enumerating

struct Pass {
    std::string name;
    std::string description;
    int weight = 10; // relative chance of being picked in random mode
    std::function<void(const Func&, const EmitFn&)> enumerate;
    // Optional: a cheaper random pick than enumerating everything.
    std::function<bool(const Func&, Rng&, std::string&)> random;
};

const std::vector<Pass>& allPasses();
const Pass* findPass(const std::string& name);

// One random mutation of f by pass p. Returns false if p has nothing to offer.
bool randomMutation(const Pass& p, const Func& f, Rng& rng, std::string& out);

// An inline getter found in a header: "get_cam_x" returning "field_1AC_cam.x"
// (the member path with whitespace removed). use_getter swaps between the two.
struct Getter {
    std::string name, expr;
};
std::vector<Getter> findGetters(const std::string& text);
void setGetters(std::vector<Getter> g); // before the passes run
const std::vector<Getter>& getters();

// An operator that also exists as a named (out-of-line) method: "*" -> "Multiply_408680",
// "neg" (unary minus) -> "Negate_4086A0". named_op swaps between the two, which decides
// whether VC6 inlines it. Set with --op-alias OP=NAME.
struct OpAlias {
    std::string op, method;
};
void setOpAliases(std::vector<OpAlias> a); // before the passes run
const std::vector<OpAlias>& opAliases();

// Declared types of the members (and other one-per-line declarations) in the
// source and the headers it includes, by name: "field_7C_pObj" -> "Object_2C*".
// Names declared with different types are left out. cache_member uses these.
std::map<std::string, std::string> findFieldTypes(const std::string& text);
void setFieldTypes(std::map<std::string, std::string> t); // before the passes run
const std::map<std::string, std::string>& fieldTypes();

// A pass, or a combination of passes written "a+b+c" on the command line.
struct PassGroup {
    std::vector<const Pass*> passes;
    int weightOverride = -1; // --weight
    std::string name() const;
    int weight() const;
};

// Parses pass specs such as {"reorder_saves,invert_if", "swap_operands+flip_compare"}:
// commas separate groups, '+' combines passes into one group, "all" adds every
// pass as its own group, "none" adds nothing (no passes). No specs means "all".
bool parsePassSpecs(const std::vector<std::string>& specs, std::vector<PassGroup>& out,
                    std::string& err);

// Every distinct text made by applying one mutation of each pass in some
// non-empty, in-order subset of g's passes (for "a+b": a, b, and a then b).
// emit returns false to stop.
void enumerateGroup(const PassGroup& g, const std::string& text,
                    const std::function<bool(const std::string&)>& emit);

// One random mutation from each of g's passes, in order. Passes with nothing to
// offer are skipped. Returns false if nothing changed.
bool randomGroupMutation(const PassGroup& g, const std::string& text, Rng& rng, std::string& out);

// Token-range rewriting of a function.
class Rewriter {
public:
    explicit Rewriter(const Func& f);
    void replace(int b, int e, const std::string& text);
    void remove(int b, int e); // also drops the line break before the range
    void insertBefore(int tok, const std::string& stmtText); // on its own line
    std::string apply() const; // empty if edits overlapped

private:
    const Func& f_;
    std::vector<std::string> lead_, text_, prefix_;
    std::vector<bool> touched_;
    bool overlap_ = false;
};

} // namespace perm
