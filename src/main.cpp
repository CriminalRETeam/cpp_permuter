// cpp_permuter: brute-forces source permutations of one C++ function until
// its compiled code matches (or gets closer to) a target object.

#include "analysis.hpp"
#include "parser.hpp"
#include "passes.hpp"
#include "runner.hpp"
#include "scorer.hpp"
#include "workspace.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace perm;

namespace {

const char* kUsage = R"(usage: cpp_permuter [options]

Finds a source permutation of one function that compiles to the target code.

  cpp_permuter -s FILE -f NAME -c CMD -t OBJ [options]
  cpp_permuter --check-parse FILE... [--check-limit N]

Required:
  -s, --source FILE         .cpp file holding the function
  -f, --function NAME       function to permute, e.g. PedGroup::PromoteMemberToLeader_4C9680
  -c, --compile CMD         compile command. {src} is the candidate source, {obj} the
                            object to write, {dir} the candidate's directory and {root}
                            the mirror of --mirror-root
  -t, --target-obj FILE     object file with the target code (not needed with --score-cmd)

Scoring:
      --symbol SYM          symbol of the function in the compiled object
                            (default: guessed from --function)
      --target-symbol SYM   symbol in the target object (default: same as --symbol)
      --score-cmd CMD       score with this command instead ({obj}, {src}); it must
                            print the score (lower is better, 0 = match) as its last number
      --objdump PATH        llvm-objdump to use (default: llvm-objdump)
      --ignore-reloc-names  compare relocated operands without their symbol names
                            (for targets built from raw executable asm)

Search:
  -m, --mode MODE           random (default) or exhaustive
  -p, --passes LIST         passes to use (default: all), see --list-passes. Commas
                            separate passes, '+' combines them: "a+b" tries a, b and
                            a then b. Can be given more than once
      --depth N             exhaustive: chain up to N passes/combos (default 1)
      --max-candidates N    exhaustive: stop after N candidates (default 200000)
  -n, --iterations N        random: stop after N compiles (default: run until match/Ctrl-C)
      --max-mutations N     random: up to N passes/combos per candidate (default 3)
      --seed N              random seed
      --keep-going          don't stop at the first exact match
  -j, --jobs N              parallel compiles (default 1)
      --timeout SEC         compile timeout (default 120)

Other functions (for helpers VC6 inlines into the target):
      --also [FILE:]NAME    also permute this function. FILE is relative to the current
                            directory or the source's; without it NAME is looked up in
                            the source and the headers it includes. Repeatable
      --inline-callees      also permute the inline functions the target calls: their
                            definitions in the source (if marked inline) and in the
                            headers it includes
      --max-callees N       at most N of those (default 8)
  -I, --include-dir DIR     where to look for #include "..." headers besides the
                            including file's directory (default: the source's)
      --mirror-root DIR     candidates are compiled in a mirror of this directory
                            (symlinks plus the changed files), so changed headers are
                            the ones every #include sees (default: the source's dir)
      --list-regions        print the functions that would be permuted and exit

Output:
  -o, --output-dir DIR      where improvements go (default: permuter_out)
      --config FILE         read "key = value" lines as if they were --key value
      --list-passes         list the passes and exit
      --show-ast            print the parsed statement tree and exit
      --dry-run             print the candidates' diffs instead of compiling
      --show-base-diff      print the asm diff of the unmodified function first
  -v, --verbose             print compile errors

Checking the parser:
      --check-parse FILE... parse every function in the files and run every pass on
                            it; report functions that don't parse and candidates
                            that don't parse back. Exit 1 on any
      --check-limit N       candidates checked per pass and function (default 25)
)";

struct Options {
    std::string source, function, compile, targetObj, symbol, targetSymbol, scoreCmd;
    std::string objdump = "llvm-objdump";
    bool ignoreRelocNames = false;
    std::string mode = "random";
    std::vector<std::string> passes;
    int depth = 1;
    long maxCandidates = 200000;
    long iterations = 0;
    int maxMutations = 3;
    unsigned long long seed = 0;
    bool seedSet = false;
    bool keepGoing = false;
    int jobs = 1;
    int timeout = 120;
    std::string outputDir = "permuter_out";
    std::vector<std::string> also, includeDirs, checkParse;
    bool inlineCallees = false, listRegions = false, checkMode = false;
    int maxCallees = 8;
    long checkLimit = 25;
    std::string mirrorRoot;
    bool listPasses = false, showAst = false, dryRun = false, showBaseDiff = false, verbose = false;
};

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    return (bool)out;
}

bool applyOption(Options& o, const std::string& key, const std::string& val, bool hasVal,
                 std::string& err) {
    auto need = [&]() {
        if (!hasVal) err = "--" + key + " needs a value";
        return hasVal;
    };
    auto num = [&](auto& dst) {
        if (!need()) return false;
        try {
            dst = (std::remove_reference_t<decltype(dst)>)std::stoll(val);
        } catch (...) {
            err = "--" + key + ": not a number: " + val;
            return false;
        }
        return true;
    };
    if (key == "source" || key == "s") return need() && (o.source = val, true);
    if (key == "function" || key == "f") return need() && (o.function = val, true);
    if (key == "compile" || key == "c") return need() && (o.compile = val, true);
    if (key == "target-obj" || key == "t") return need() && (o.targetObj = val, true);
    if (key == "symbol") return need() && (o.symbol = val, true);
    if (key == "target-symbol") return need() && (o.targetSymbol = val, true);
    if (key == "score-cmd") return need() && (o.scoreCmd = val, true);
    if (key == "objdump") return need() && (o.objdump = val, true);
    if (key == "ignore-reloc-names") return o.ignoreRelocNames = true;
    if (key == "mode" || key == "m") return need() && (o.mode = val, true);
    if (key == "passes" || key == "p") return need() && (o.passes.push_back(val), true);
    if (key == "depth") return num(o.depth);
    if (key == "max-candidates") return num(o.maxCandidates);
    if (key == "iterations" || key == "n") return num(o.iterations);
    if (key == "max-mutations") return num(o.maxMutations);
    if (key == "seed") return num(o.seed) && (o.seedSet = true);
    if (key == "keep-going") return o.keepGoing = true;
    if (key == "jobs" || key == "j") return num(o.jobs);
    if (key == "timeout") return num(o.timeout);
    if (key == "output-dir" || key == "o") return need() && (o.outputDir = val, true);
    if (key == "also") return need() && (o.also.push_back(val), true);
    if (key == "inline-callees") return o.inlineCallees = true;
    if (key == "max-callees") return num(o.maxCallees);
    if (key == "include-dir" || key == "I") return need() && (o.includeDirs.push_back(val), true);
    if (key == "mirror-root") return need() && (o.mirrorRoot = val, true);
    if (key == "list-regions") return o.listRegions = true;
    if (key == "check-parse") return o.checkMode = true;
    if (key == "check-limit") return num(o.checkLimit);
    if (key == "list-passes") return o.listPasses = true;
    if (key == "show-ast") return o.showAst = true;
    if (key == "dry-run") return o.dryRun = true;
    if (key == "show-base-diff") return o.showBaseDiff = true;
    if (key == "verbose" || key == "v") return o.verbose = true;
    err = "unknown option --" + key;
    return false;
}

bool isFlag(const std::string& k) {
    static const std::set<std::string> flags = {
        "ignore-reloc-names", "keep-going", "list-passes", "show-ast", "dry-run",
        "show-base-diff", "verbose", "v", "inline-callees", "list-regions", "check-parse",
    };
    return flags.count(k) > 0;
}

bool loadConfig(Options& o, const std::string& path, std::string& err) {
    std::string text;
    if (!readFile(path, text)) {
        err = "can't read config " + path;
        return false;
    }
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        line = line.substr(b);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        std::string key = line.substr(0, eq), val;
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (eq != std::string::npos) {
            val = line.substr(eq + 1);
            size_t vb = val.find_first_not_of(" \t");
            val = vb == std::string::npos ? "" : val.substr(vb);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        }
        if (!applyOption(o, key, val, eq != std::string::npos, err)) return false;
    }
    return true;
}

bool parseArgs(int argc, char** argv, Options& o, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout << kUsage;
            std::exit(0);
        }
        std::string key, val;
        bool hasVal = false;
        if (a.rfind("--", 0) == 0) {
            key = a.substr(2);
            size_t eq = key.find('=');
            if (eq != std::string::npos) {
                val = key.substr(eq + 1);
                key = key.substr(0, eq);
                hasVal = true;
            }
        } else if (a.size() == 2 && a[0] == '-') {
            key = a.substr(1);
        } else {
            o.checkParse.push_back(a); // files for --check-parse
            continue;
        }
        if (!hasVal && !isFlag(key) && i + 1 < argc) {
            val = argv[++i];
            hasVal = true;
        }
        if (key == "config") {
            if (!hasVal || !loadConfig(o, val, err)) {
                if (err.empty()) err = "--config needs a value";
                return false;
            }
            continue;
        }
        if (!applyOption(o, key, val, hasVal, err)) return false;
    }
    return true;
}

// Line diff (LCS) with a little context, for the output files.
std::string lineDiff(const std::string& a, const std::string& b) {
    auto lines = [](const std::string& s) {
        std::vector<std::string> r;
        std::istringstream in(s);
        std::string l;
        while (std::getline(in, l)) r.push_back(l);
        return r;
    };
    auto A = lines(a), B = lines(b);
    size_t n = A.size(), m = B.size();
    std::vector<std::vector<int>> L(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            L[i][j] = A[i] == B[j] ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
    std::vector<std::pair<char, std::string>> ops;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && A[i] == B[j]) ops.push_back({' ', A[i++]}), j++;
        else if (j < m && (i == n || L[i][j + 1] >= L[i + 1][j])) ops.push_back({'+', B[j++]});
        else ops.push_back({'-', A[i++]});
    }
    std::string out;
    const int ctx = 2;
    std::vector<bool> show(ops.size(), false);
    for (size_t k = 0; k < ops.size(); ++k)
        if (ops[k].first != ' ')
            for (long d = -ctx; d <= ctx; ++d)
                if ((long)k + d >= 0 && (long)k + d < (long)ops.size()) show[k + d] = true;
    bool gap = false;
    for (size_t k = 0; k < ops.size(); ++k) {
        if (!show[k]) {
            if (!gap && !out.empty()) out += "...\n";
            gap = true;
            continue;
        }
        gap = false;
        out += ops[k].first;
        out += ops[k].second + "\n";
    }
    return out;
}

double lastNumber(const std::string& s, bool& ok) {
    ok = false;
    double v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::isdigit((unsigned char)s[i]) || (s[i] == '-' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t used = 0;
            try {
                v = std::stod(s.substr(i), &used);
                ok = true;
                i += used - 1;
            } catch (...) {
            }
        }
    }
    return v;
}

// One text per region; [0] is the target function.
using Cand = std::vector<std::string>;

size_t hashCand(const Cand& c) {
    std::string all;
    for (auto& t : c) all += t + '\x01';
    return std::hash<std::string>()(all);
}

struct Workspace {
    std::vector<Region> regions;
    Cand base;
    std::map<std::string, std::string> files; // original contents of the files with regions
    std::string root;

    // New contents of every file that holds a region.
    std::map<std::string, std::string> render(const Cand& c) const {
        std::map<std::string, std::string> out;
        for (auto& [file, text] : files) {
            std::vector<std::pair<const Region*, const std::string*>> parts;
            for (size_t i = 0; i < regions.size(); ++i)
                if (regions[i].file == file) parts.push_back({&regions[i], &c[i]});
            out[file] = spliceRegions(text, parts);
        }
        return out;
    }
};

struct Evaluator {
    const Options& o;
    const Workspace& ws;
    ScoreConfig sc;
    std::string symbol, targetSymbol;
    std::vector<Insn> target;
    std::vector<Mirror> mirrors; // one per worker

    std::string srcPath(int w) const { return mirrors[w].map(ws.regions[0].file); }
    std::string objPath(int w) const {
        return (fs::path(o.outputDir) / ".work" / ("w" + std::to_string(w) + ".obj")).string();
    }

    bool compile(int w, const Cand& c, std::string& log) const {
        for (auto& [file, text] : ws.render(c)) {
            std::string path = mirrors[w].map(file);
            if (!writeFile(path, text)) {
                log = "can't write " + path;
                return false;
            }
        }
        std::string src = srcPath(w), obj = objPath(w);
        std::error_code ec;
        fs::remove(obj, ec);
        std::string cmd = expandTemplate(o.compile, {{"src", src},
                                                     {"obj", obj},
                                                     {"dir", fs::path(src).parent_path().string()},
                                                     {"root", mirrors[w].dir()}});
        CmdResult r = runCommand(cmd, o.timeout);
        log = r.output;
        if (r.timedOut) log += "\n(timed out)";
        return r.status == 0 && fs::exists(obj);
    }

    // Score of a compiled candidate; -1 if it couldn't be scored.
    long score(int w, std::vector<Insn>* insns, std::string& log) const {
        std::string obj = objPath(w);
        if (!o.scoreCmd.empty()) {
            CmdResult r = runCommand(expandTemplate(o.scoreCmd, {{"obj", obj}, {"src", srcPath(w)}}), o.timeout);
            bool ok;
            double v = lastNumber(r.output, ok);
            if (r.status != 0 || !ok) {
                log = r.output;
                return -1;
            }
            return (long)(v + 0.5);
        }
        std::vector<Insn> mine;
        if (!disassemble(sc, obj, symbol, mine, log)) return -1;
        if (insns) *insns = mine;
        return scoreInsns(sc, target, mine);
    }

    long eval(int w, const Cand& c, std::string& log, std::vector<Insn>* insns = nullptr) const {
        if (!compile(w, c, log)) return -1;
        return score(w, insns, log);
    }
};

struct State {
    std::mutex mu;
    Cand base, best;
    long baseScore = 0, bestScore = 0;
    long compiles = 0, failures = 0, improvements = 0;
    std::unordered_set<size_t> seen;
    int outputs = 0;
};

// Diff of every region that changed, with a header per region when there are several.
std::string candDiff(const Workspace& ws, const Cand& c) {
    std::string out;
    for (size_t i = 0; i < c.size(); ++i) {
        if (c[i] == ws.base[i]) continue;
        if (ws.regions.size() > 1) out += "@@ " + ws.regions[i].name + " (" + fs::path(ws.regions[i].file).filename().string() + ")\n";
        out += lineDiff(ws.base[i], c[i]);
    }
    return out;
}

void writeOutput(const Options& o, const Evaluator& ev, State& st, const Cand& c, long score,
                 const std::vector<Insn>& insns) {
    const Workspace& ws = ev.ws;
    std::string dir = (fs::path(o.outputDir) / ("output-" + std::to_string(score) + "-" +
                                                std::to_string(++st.outputs)))
                          .string();
    fs::create_directories(dir);
    std::string funcs;
    for (size_t i = 0; i < c.size(); ++i) {
        if (i > 0 && c[i] == ws.base[i]) continue;
        if (ws.regions.size() > 1) funcs += "// " + ws.regions[i].name + " in " + ws.regions[i].file + "\n";
        funcs += c[i] + "\n\n";
    }
    writeFile(dir + "/function.cpp", funcs);
    auto rendered = ws.render(c);
    writeFile(dir + "/source.cpp", rendered[ws.regions[0].file]);
    for (auto& [file, text] : rendered) // changed headers, under their own names
        if (file != ws.regions[0].file && text != ws.files.at(file))
            writeFile(dir + "/" + fs::path(file).filename().string(), text);
    std::string diff = candDiff(ws, c);
    writeFile(dir + "/diff.txt", diff);
    if (!insns.empty()) writeFile(dir + "/asm_diff.txt", diffInsns(ev.sc, ev.target, insns));
    writeFile(dir + "/score.txt", std::to_string(score) + "\n");
    std::cout << "\n[" << score << "] new best (was " << st.bestScore << "), written to " << dir
              << "\n"
              << diff << std::flush;
}

// Records a scored candidate. Returns true if it was a match and we should stop.
bool report(const Options& o, const Evaluator& ev, State& st, const Cand& c, long score,
            const std::vector<Insn>& insns, const std::string& log) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.compiles++;
    if (score < 0) {
        st.failures++;
        if (o.verbose) std::cout << "\ncandidate failed:\n" << log << "\n";
        return false;
    }
    if (score < st.bestScore) {
        writeOutput(o, ev, st, c, score, insns);
        st.bestScore = score;
        st.best = c;
        st.improvements++;
    }
    return score == 0 && !o.keepGoing;
}

void printStatus(State& st) {
    std::lock_guard<std::mutex> lk(st.mu);
    std::cout << "\riterations: " << st.compiles << ", failed: " << st.failures
              << ", base: " << st.baseScore << ", best: " << st.bestScore << "      "
              << std::flush;
}

// One random candidate derived from start; empty if nothing applied. With
// several regions, half the mutations go to the target function.
Cand randomCandidate(const Options& o, const std::vector<PassGroup>& groups, const Cand& start,
                     Rng& rng) {
    int total = 0;
    for (auto& g : groups) total += g.weight();
    int k = std::uniform_int_distribution<int>(1, std::max(1, o.maxMutations))(rng);
    Cand c = start;
    for (int m = 0; m < k; ++m) {
        size_t r = 0;
        if (c.size() > 1 && std::uniform_int_distribution<int>(0, 1)(rng) == 1)
            r = std::uniform_int_distribution<size_t>(1, c.size() - 1)(rng);
        for (int attempt = 0; attempt < 10; ++attempt) {
            int x = std::uniform_int_distribution<int>(0, total - 1)(rng);
            const PassGroup* g = &groups.back();
            for (auto& q : groups) {
                if (x < q.weight()) {
                    g = &q;
                    break;
                }
                x -= q.weight();
            }
            std::string out;
            if (randomGroupMutation(*g, c[r], rng, out)) {
                c[r] = out;
                break;
            }
        }
    }
    return c == start ? Cand{} : c;
}

int runRandom(const Options& o, const Evaluator& ev, State& st, const std::vector<PassGroup>& groups) {
    unsigned long long seed = o.seedSet ? o.seed : (unsigned long long)std::random_device{}();
    std::vector<std::thread> workers;
    std::atomic<int> idle{0};
    for (int w = 0; w < o.jobs; ++w) {
        workers.emplace_back([&, w]() {
            Rng rng(seed + (unsigned long long)w * 7919);
            int misses = 0;
            while (!gStop) {
                Cand start;
                {
                    std::lock_guard<std::mutex> lk(st.mu);
                    if (o.iterations && st.compiles >= o.iterations) break;
                    bool fromBest = st.best != st.base && std::uniform_int_distribution<int>(0, 1)(rng) == 0;
                    start = fromBest ? st.best : st.base;
                }
                Cand cand = randomCandidate(o, groups, start, rng);
                bool fresh = false;
                if (!cand.empty()) {
                    std::lock_guard<std::mutex> lk(st.mu);
                    fresh = st.seen.insert(hashCand(cand)).second;
                }
                if (!fresh) {
                    if (++misses > 2000) break; // nothing new left to try
                    continue;
                }
                misses = 0;
                std::string log;
                std::vector<Insn> insns;
                long s = ev.eval(w, cand, log, &insns);
                if (report(o, ev, st, cand, s, insns, log)) gStop = true;
            }
            idle++;
        });
    }
    while (idle < o.jobs) {
        for (int i = 0; i < 20 && idle < o.jobs; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        printStatus(st);
    }
    for (auto& t : workers) t.join();
    return 0;
}

// Every candidate one pass or combo away from c, in any region.
void enumerateCand(const std::vector<PassGroup>& groups, const Cand& c,
                   const std::function<bool(const Cand&, const std::string&)>& emit) {
    bool stop = false;
    for (size_t r = 0; r < c.size() && !stop; ++r)
        for (auto& g : groups) {
            if (stop) break;
            enumerateGroup(g, c[r], [&](const std::string& s) {
                Cand n = c;
                n[r] = s;
                if (!emit(n, g.name())) stop = true;
                return !stop;
            });
        }
}

int runExhaustive(const Options& o, const Evaluator& ev, State& st, const std::vector<PassGroup>& groups) {
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Cand> queue;
    bool done = false;
    long produced = 0;

    auto worker = [&](int w) {
        while (true) {
            Cand cand;
            {
                std::unique_lock<std::mutex> lk(qmu);
                qcv.wait(lk, [&] { return !queue.empty() || done || gStop; });
                if (gStop || (queue.empty() && done)) return;
                cand = std::move(queue.front());
                queue.pop_front();
            }
            qcv.notify_all();
            std::string log;
            std::vector<Insn> insns;
            long s = ev.eval(w, cand, log, &insns);
            if (report(o, ev, st, cand, s, insns, log)) {
                gStop = true;
                qcv.notify_all();
            }
        }
    };
    std::vector<std::thread> workers;
    for (int w = 0; w < o.jobs; ++w) workers.emplace_back(worker, w);

    std::thread producer([&]() {
        std::vector<Cand> level = {st.base};
        for (int d = 1; d <= o.depth && !gStop; ++d) {
            std::vector<Cand> next;
            for (auto& c : level) {
                if (gStop || produced >= o.maxCandidates) break;
                enumerateCand(groups, c, [&](const Cand& n, const std::string&) {
                    if (gStop || produced >= o.maxCandidates) return false;
                    {
                        std::lock_guard<std::mutex> lk(st.mu);
                        if (!st.seen.insert(hashCand(n)).second) return true;
                    }
                    produced++;
                    if (d < o.depth) next.push_back(n);
                    std::unique_lock<std::mutex> lk(qmu);
                    qcv.wait(lk, [&] { return queue.size() < 256 || gStop; });
                    queue.push_back(n);
                    qcv.notify_all();
                    return true;
                });
            }
            level = std::move(next);
        }
        std::lock_guard<std::mutex> lk(qmu);
        done = true;
        qcv.notify_all();
    });

    while (true) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            if ((done && queue.empty()) || gStop) break;
        }
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lk(qmu);
            if ((done && queue.empty()) || gStop) break;
        }
        printStatus(st);
    }
    producer.join();
    {
        std::lock_guard<std::mutex> lk(qmu);
        done = true;
    }
    qcv.notify_all();
    for (auto& t : workers) t.join();
    printStatus(st);
    std::cout << "\n" << produced << " candidates generated\n";
    return 0;
}

int dryRun(const Options& o, const Workspace& ws, const std::vector<PassGroup>& groups) {
    if (o.mode == "exhaustive") {
        long count = 0;
        std::unordered_set<size_t> seen = {hashCand(ws.base)};
        std::map<std::string, long> perGroup;
        enumerateCand(groups, ws.base, [&](const Cand& c, const std::string& g) {
            if (!seen.insert(hashCand(c)).second) return true;
            perGroup[g]++;
            if (++count <= 50) std::cout << "=== " << g << " #" << perGroup[g] << "\n" << candDiff(ws, c);
            return count < o.maxCandidates;
        });
        for (auto& g : groups) std::cerr << g.name() << ": " << perGroup[g.name()] << " candidates\n";
        std::cerr << count << " candidates in total\n";
        return 0;
    }
    Rng rng(o.seedSet ? o.seed : 1);
    long n = o.iterations ? o.iterations : 10;
    for (long i = 0; i < n; ++i) {
        Cand c = randomCandidate(o, groups, ws.base, rng);
        std::cout << "=== random #" << i + 1 << "\n" << (c.empty() ? "(no change)\n" : candDiff(ws, c));
    }
    return 0;
}

// --check-parse: every function in the files must parse, and every pass's
// candidates must parse back.
int checkParse(const Options& o) {
    long funcs = 0, parseFails = 0, others = 0, sites = 0, siteFails = 0, cands = 0, badCands = 0;
    std::map<std::string, long> perPass;
    for (auto& path : o.checkParse) {
        std::string src;
        if (!readFile(path, src)) {
            std::cerr << "can't read " << path << "\n";
            parseFails++;
            continue;
        }
        for (auto& d : listDefinitions(src)) {
            funcs++;
            std::string text = src.substr(d.start, d.end - d.start);
            std::string err;
            auto f = parseFunc(text, err);
            if (!f) {
                parseFails++;
                std::cout << "PARSE FAIL " << path << ": " << d.name << ": " << err << "\n";
                continue;
            }
            forEachStmt(*f->body, [&](const Stmt& s) {
                if (s.kind == SK::Other) {
                    others++;
                    if (o.verbose) std::cout << "other " << path << ": " << d.name << ": " << f->textOf(s).substr(0, 60) << "\n";
                }
            });
            for (auto& site : exprSites(*f)) {
                sites++;
                if (!parseExpr(*f, site.b, site.e)) {
                    siteFails++;
                    if (o.verbose) std::cout << "expr " << path << ": " << d.name << ": " << f->tokText(site.b, site.e).substr(0, 60) << "\n";
                }
            }
            for (auto& p : allPasses()) {
                long n = 0;
                p.enumerate(*f, [&](Mutation m) {
                    std::string out = m();
                    cands++;
                    perPass[p.name]++;
                    std::string e2;
                    if (out.empty() || out == text || !parseFunc(out, e2)) {
                        badCands++;
                        std::cout << "BAD CANDIDATE " << path << ": " << d.name << " (" << p.name << ")"
                                  << (out.empty() ? ": empty" : out == text ? ": unchanged" : ": " + e2) << "\n";
                    }
                    return ++n < o.checkLimit;
                });
            }
        }
    }
    std::cout << o.checkParse.size() << " files, " << funcs << " functions, " << parseFails
              << " failed to parse\n"
              << others << " statements left unclassified (barriers), " << siteFails << " of "
              << sites << " expressions not understood\n"
              << cands << " candidates checked, " << badCands << " bad\n";
    for (auto& [name, n] : perPass) std::cout << "  " << name << ": " << n << "\n";
    return parseFails == 0 && badCands == 0 ? 0 : 1;
}

// Resolves "--also [FILE:]NAME" to a region.
bool findAlso(const std::string& spec, const std::string& srcPath, const std::vector<std::string>& dirs,
              Region& r, std::string& err) {
    std::string file, name = spec;
    size_t colon = spec.find(':');
    while (colon != std::string::npos && colon + 1 < spec.size() && spec[colon + 1] == ':')
        colon = spec.find(':', colon + 2); // skip "::"
    if (colon != std::string::npos) {
        file = spec.substr(0, colon);
        name = spec.substr(colon + 1);
    }
    std::vector<std::string> files;
    if (!file.empty()) {
        // relative to the current directory, else to the source's
        fs::path p = fs::absolute(file);
        if (!fs::exists(p)) p = fs::path(srcPath).parent_path() / file;
        files.push_back(fs::absolute(p).lexically_normal().string());
    }
    else {
        files.push_back(srcPath);
        for (auto& h : includedFiles(srcPath, dirs)) files.push_back(h);
    }
    for (auto& path : files) {
        std::string text;
        if (!readFile(path, text)) continue;
        auto defs = findDefinitions(text, name);
        if (defs.empty()) continue;
        r = {path, defs[0].first, defs[0].second, name};
        return true;
    }
    err = "--also " + spec + ": definition not found";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    std::string err;
    if (!parseArgs(argc, argv, o, err)) {
        std::cerr << "error: " << err << "\n\n" << kUsage;
        return 2;
    }
    if (o.listPasses) {
        for (auto& p : allPasses()) std::cout << "  " << p.name << "\n      " << p.description << "\n";
        return 0;
    }
    if (o.checkMode) {
        if (o.checkParse.empty()) {
            std::cerr << "error: --check-parse needs files\n";
            return 2;
        }
        return checkParse(o);
    }
    if (!o.checkParse.empty()) {
        std::cerr << "error: unexpected argument: " << o.checkParse[0] << "\n";
        return 2;
    }
    if (o.source.empty() || o.function.empty()) {
        std::cerr << "error: --source and --function are required\n\n" << kUsage;
        return 2;
    }
    std::string srcPath = fs::absolute(o.source).lexically_normal().string();
    std::string src;
    if (!readFile(srcPath, src)) {
        std::cerr << "error: can't read " << o.source << "\n";
        return 1;
    }
    size_t start, end;
    if (!locateFunction(src, o.function, start, end, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    if (!err.empty()) std::cerr << err << "\n";
    std::string funcText = src.substr(start, end - start);
    auto func = parseFunc(funcText, err);
    if (!func) {
        std::cerr << "error: can't parse " << o.function << ": " << err << "\n";
        return 1;
    }
    if (o.showAst) {
        std::cout << dumpStmt(*func, *func->body);
        std::cout << "params:";
        for (auto& p : func->params) std::cout << " " << p;
        std::cout << "\nlocals:";
        for (auto& l : func->locals) std::cout << " " << l;
        std::cout << "\n";
        return 0;
    }
    std::vector<PassGroup> groups;
    if (!parsePassSpecs(o.passes, groups, err)) {
        std::cerr << "error: " << err << "\n";
        return 2;
    }
    if (o.mode != "random" && o.mode != "exhaustive") {
        std::cerr << "error: --mode must be random or exhaustive\n";
        return 2;
    }

    // the functions to permute
    Workspace ws;
    std::vector<std::string> dirs = o.includeDirs;
    if (dirs.empty()) dirs.push_back(fs::path(srcPath).parent_path().string());
    ws.root = o.mirrorRoot.empty() ? fs::path(srcPath).parent_path().string()
                                   : fs::absolute(o.mirrorRoot).lexically_normal().string();
    ws.regions.push_back({srcPath, start, end, o.function});
    auto addRegion = [&](const Region& r) {
        for (auto& x : ws.regions)
            if (x.file == r.file && x.start < r.end && r.start < x.end) return; // overlaps
        ws.regions.push_back(r);
    };
    for (auto& spec : o.also) {
        Region r;
        if (!findAlso(spec, srcPath, dirs, r, err)) {
            std::cerr << "error: " << err << "\n";
            return 1;
        }
        addRegion(r);
    }
    if (o.inlineCallees)
        for (auto& r : inlineCallees(srcPath, start, end, *func, dirs, (size_t)o.maxCallees)) addRegion(r);
    for (auto& r : ws.regions) {
        if (!ws.files.count(r.file) && !readFile(r.file, ws.files[r.file])) {
            std::cerr << "error: can't read " << r.file << "\n";
            return 1;
        }
        std::string text = ws.files[r.file].substr(r.start, r.end - r.start);
        if (!parseFunc(text, err)) {
            std::cerr << "error: can't parse " << r.name << ": " << err << "\n";
            return 1;
        }
        ws.base.push_back(text);
    }
    if (o.listRegions || ws.regions.size() > 1) {
        std::cout << "permuting:\n";
        for (auto& r : ws.regions) std::cout << "  " << r.name << "  (" << r.file << ")\n";
        if (o.listRegions) return 0;
    }
    if (o.dryRun) return dryRun(o, ws, groups);

    if (o.compile.empty() || (o.targetObj.empty() && o.scoreCmd.empty())) {
        std::cerr << "error: --compile and --target-obj (or --score-cmd) are required\n";
        return 2;
    }
    if (o.jobs < 1) o.jobs = 1;

    Evaluator ev{o, ws};
    ev.sc.objdump = o.objdump;
    ev.sc.ignoreRelocNames = o.ignoreRelocNames;
    std::vector<std::string> realFiles;
    for (auto& [file, text] : ws.files) realFiles.push_back(file);
    for (int w = 0; w < o.jobs; ++w) {
        Mirror m;
        std::string dir = (fs::path(o.outputDir) / ".work" / ("w" + std::to_string(w))).string();
        if (!m.create(ws.root, dir, realFiles, err)) {
            std::cerr << "error: " << err << "\n";
            return 1;
        }
        ev.mirrors.push_back(m);
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    auto cleanup = [&]() {
        std::error_code ec;
        for (auto& m : ev.mirrors) fs::remove_all(m.dir(), ec);
    };

    // the unmodified functions
    std::string log;
    if (!ev.compile(0, ws.base, log)) {
        std::cerr << "error: the unmodified source doesn't compile:\n" << log << "\n";
        cleanup();
        return 1;
    }
    if (o.scoreCmd.empty()) {
        if (o.symbol.empty()) {
            auto syms = listSymbols(ev.sc, ev.objPath(0), err);
            ev.symbol = guessSymbol(syms, o.function);
            if (ev.symbol.empty()) {
                std::cerr << "error: can't find the symbol for " << o.function
                          << " in the compiled object; pass --symbol\n";
                cleanup();
                return 1;
            }
        } else {
            ev.symbol = o.symbol;
        }
        ev.targetSymbol = o.targetSymbol.empty() ? ev.symbol : o.targetSymbol;
        if (!disassemble(ev.sc, o.targetObj, ev.targetSymbol, ev.target, err)) {
            std::cerr << "error: target: " << err << "\n";
            cleanup();
            return 1;
        }
        std::cout << "symbol: " << ev.symbol << " (" << ev.target.size() << " target instructions)\n";
    }
    std::vector<Insn> baseInsns;
    long base = ev.score(0, &baseInsns, log);
    if (base < 0) {
        std::cerr << "error: can't score the unmodified source:\n" << log << "\n";
        cleanup();
        return 1;
    }
    if (o.showBaseDiff && !baseInsns.empty()) std::cout << diffInsns(ev.sc, ev.target, baseInsns);
    std::cout << "base score: " << base << "\n";

    State st;
    st.base = st.best = ws.base;
    st.baseScore = st.bestScore = base;
    st.seen.insert(hashCand(ws.base));
    if (base == 0) {
        std::cout << "already matching\n";
        cleanup();
        return 0;
    }
    if (o.mode == "random") runRandom(o, ev, st, groups);
    else runExhaustive(o, ev, st, groups);
    cleanup();

    std::cout << "\n";
    if (st.bestScore == 0) std::cout << "MATCH found";
    else if (st.bestScore < base) std::cout << "best score " << st.bestScore << " (base " << base << ")";
    else std::cout << "no improvement over base score " << base;
    std::cout << " after " << st.compiles << " compiles (" << st.failures << " failed)\n";
    return st.bestScore == 0 ? 0 : 3;
}
