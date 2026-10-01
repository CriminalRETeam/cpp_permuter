// Source mutation passes. Each pass looks at a parsed function and offers
// a set of rewrites ("mutations"); a mutation produces the new function text.
#pragma once

#include "parser.hpp"

#include <functional>
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
