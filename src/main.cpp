// cpp_permuter: brute-forces source permutations of one C++ function until
// its compiled code matches (or gets closer to) a target object.

#include "analysis.hpp"
#include "parser.hpp"
#include "passes.hpp"
#include "runner.hpp"
#include "scorer.hpp"

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

Required:
  -s, --source FILE         .cpp file holding the function
  -f, --function NAME       function to permute, e.g. PedGroup::PromoteMemberToLeader_4C9680
  -c, --compile CMD         compile command. {src} is the candidate source, {obj} the
                            object to write, {dir} the original source's directory
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
  -p, --passes LIST         comma separated passes (default: all). See --list-passes
      --depth N             exhaustive: apply up to N mutations in a row (default 1)
      --max-candidates N    exhaustive: stop after N candidates (default 200000)
  -n, --iterations N        random: stop after N compiles (default: run until match/Ctrl-C)
      --max-mutations N     random: up to N mutations per candidate (default 3)
      --seed N              random seed
      --keep-going          don't stop at the first exact match
  -j, --jobs N              parallel compiles (default 1)
      --timeout SEC         compile timeout (default 120)

Output:
  -o, --output-dir DIR      where improvements go (default: permuter_out)
      --candidate-dir DIR   where candidate sources are written (default: next to
                            the source, so relative #includes keep working)
      --config FILE         read "key = value" lines as if they were --key value
      --list-passes         list the passes and exit
      --show-ast            print the parsed statement tree and exit
      --dry-run             print the candidates' diffs instead of compiling
      --show-base-diff      print the asm diff of the unmodified function first
  -v, --verbose             print compile errors
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
    std::string candidateDir;
    bool listPasses = false, showAst = false, dryRun = false, showBaseDiff = false, verbose = false;
};

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) r.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur += c;
        }
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

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
    if (key == "passes" || key == "p") return need() && (o.passes = split(val, ','), true);
    if (key == "depth") return num(o.depth);
    if (key == "max-candidates") return num(o.maxCandidates);
    if (key == "iterations" || key == "n") return num(o.iterations);
    if (key == "max-mutations") return num(o.maxMutations);
    if (key == "seed") return num(o.seed) && (o.seedSet = true);
    if (key == "keep-going") return o.keepGoing = true;
    if (key == "jobs" || key == "j") return num(o.jobs);
    if (key == "timeout") return num(o.timeout);
    if (key == "output-dir" || key == "o") return need() && (o.outputDir = val, true);
    if (key == "candidate-dir") return need() && (o.candidateDir = val, true);
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
        "show-base-diff", "verbose", "v",
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
            err = "unexpected argument: " + a;
            return false;
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

struct Evaluator {
    const Options& o;
    ScoreConfig sc;
    std::string prefix, suffix, srcDir, stem, ext, symbol, targetSymbol;
    std::vector<Insn> target;
    std::vector<std::string> candPaths;

    std::string srcPath(int w) const {
        std::string dir = o.candidateDir.empty() ? srcDir : o.candidateDir;
        return (fs::path(dir) / (stem + ".permuter" + std::to_string(w) + ext)).string();
    }
    std::string objPath(int w) const {
        return (fs::path(o.outputDir) / ".work" / ("w" + std::to_string(w) + ".obj")).string();
    }

    bool compile(int w, const std::string& funcText, std::string& log) const {
        std::string src = srcPath(w), obj = objPath(w);
        if (!writeFile(src, prefix + funcText + suffix)) {
            log = "can't write " + src;
            return false;
        }
        std::error_code ec;
        fs::remove(obj, ec);
        std::string cmd = expandTemplate(o.compile, {{"src", src}, {"obj", obj}, {"dir", srcDir}});
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

    long eval(int w, const std::string& funcText, std::string& log, std::vector<Insn>* insns = nullptr) const {
        if (!compile(w, funcText, log)) return -1;
        return score(w, insns, log);
    }
};

struct State {
    std::mutex mu;
    std::string baseText, bestText;
    long baseScore = 0, bestScore = 0;
    long compiles = 0, failures = 0, improvements = 0;
    std::unordered_set<size_t> seen;
    int outputs = 0;
};

void writeOutput(const Options& o, const Evaluator& ev, State& st, const std::string& text,
                 long score, const std::vector<Insn>& insns) {
    std::string dir = (fs::path(o.outputDir) / ("output-" + std::to_string(score) + "-" +
                                                std::to_string(++st.outputs)))
                          .string();
    fs::create_directories(dir);
    writeFile(dir + "/function.cpp", text + "\n");
    writeFile(dir + "/source.cpp", ev.prefix + text + ev.suffix);
    writeFile(dir + "/diff.txt", lineDiff(st.baseText, text));
    if (!insns.empty()) writeFile(dir + "/asm_diff.txt", diffInsns(ev.sc, ev.target, insns));
    writeFile(dir + "/score.txt", std::to_string(score) + "\n");
    std::cout << "\n[" << score << "] new best (was " << st.bestScore << "), written to " << dir
              << "\n"
              << lineDiff(st.baseText, text) << std::flush;
}

// Records a scored candidate. Returns true if it was a match and we should stop.
bool report(const Options& o, const Evaluator& ev, State& st, const std::string& text,
            long score, const std::vector<Insn>& insns, const std::string& log) {
    std::lock_guard<std::mutex> lk(st.mu);
    st.compiles++;
    if (score < 0) {
        st.failures++;
        if (o.verbose) std::cout << "\ncandidate failed:\n" << log << "\n";
        return false;
    }
    if (score < st.bestScore) {
        writeOutput(o, ev, st, text, score, insns);
        st.bestScore = score;
        st.bestText = text;
        st.improvements++;
    }
    return score == 0 && !o.keepGoing;
}

size_t hashText(const std::string& s) { return std::hash<std::string>()(s); }

void printStatus(State& st) {
    std::lock_guard<std::mutex> lk(st.mu);
    std::cout << "\riterations: " << st.compiles << ", failed: " << st.failures
              << ", base: " << st.baseScore << ", best: " << st.bestScore << "      "
              << std::flush;
}

std::vector<const Pass*> pickPasses(const Options& o, std::string& err) {
    std::vector<const Pass*> r;
    if (o.passes.empty()) {
        for (auto& p : allPasses()) r.push_back(&p);
        return r;
    }
    for (auto& n : o.passes) {
        const Pass* p = findPass(n);
        if (!p) {
            err = "unknown pass '" + n + "' (see --list-passes)";
            return {};
        }
        r.push_back(p);
    }
    return r;
}

// One random candidate derived from start; empty if nothing applied.
std::string randomCandidate(const Options& o, const std::vector<const Pass*>& passes,
                            const std::string& start, Rng& rng) {
    int total = 0;
    for (auto* p : passes) total += p->weight;
    int k = std::uniform_int_distribution<int>(1, std::max(1, o.maxMutations))(rng);
    std::string text = start;
    for (int m = 0; m < k; ++m) {
        std::string err;
        auto f = parseFunc(text, err);
        if (!f) break;
        for (int attempt = 0; attempt < 10; ++attempt) {
            int r = std::uniform_int_distribution<int>(0, total - 1)(rng);
            const Pass* p = passes.back();
            for (auto* q : passes) {
                if (r < q->weight) {
                    p = q;
                    break;
                }
                r -= q->weight;
            }
            std::string out;
            if (randomMutation(*p, *f, rng, out) && out != text) {
                text = out;
                break;
            }
        }
    }
    return text == start ? "" : text;
}

int runRandom(const Options& o, const Evaluator& ev, State& st,
              const std::vector<const Pass*>& passes) {
    unsigned long long seed = o.seedSet ? o.seed : (unsigned long long)std::random_device{}();
    std::vector<std::thread> workers;
    std::atomic<int> idle{0};
    for (int w = 0; w < o.jobs; ++w) {
        workers.emplace_back([&, w]() {
            Rng rng(seed + (unsigned long long)w * 7919);
            int misses = 0;
            while (!gStop) {
                std::string start;
                {
                    std::lock_guard<std::mutex> lk(st.mu);
                    if (o.iterations && st.compiles >= o.iterations) break;
                    bool fromBest = st.bestText != st.baseText &&
                                    std::uniform_int_distribution<int>(0, 1)(rng) == 0;
                    start = fromBest ? st.bestText : st.baseText;
                }
                std::string cand = randomCandidate(o, passes, start, rng);
                bool fresh = false;
                if (!cand.empty()) {
                    std::lock_guard<std::mutex> lk(st.mu);
                    fresh = st.seen.insert(hashText(cand)).second;
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

int runExhaustive(const Options& o, const Evaluator& ev, State& st,
                  const std::vector<const Pass*>& passes) {
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::string> queue;
    bool done = false;
    long produced = 0;

    auto worker = [&](int w) {
        while (true) {
            std::string cand;
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
        std::vector<std::string> level = {st.baseText};
        for (int d = 1; d <= o.depth && !gStop; ++d) {
            std::vector<std::string> next;
            for (auto& text : level) {
                std::string err;
                auto f = parseFunc(text, err);
                if (!f) continue;
                for (auto* p : passes) {
                    p->enumerate(*f, [&](Mutation m) {
                        if (gStop || produced >= o.maxCandidates) return false;
                        std::string s = m();
                        if (s.empty()) return true;
                        {
                            std::lock_guard<std::mutex> lk(st.mu);
                            if (!st.seen.insert(hashText(s)).second) return true;
                        }
                        produced++;
                        if (d < o.depth) next.push_back(s);
                        std::unique_lock<std::mutex> lk(qmu);
                        qcv.wait(lk, [&] { return queue.size() < 256 || gStop; });
                        queue.push_back(std::move(s));
                        qcv.notify_all();
                        return true;
                    });
                }
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

int dryRun(const Options& o, const std::string& base, const std::vector<const Pass*>& passes) {
    std::string err;
    auto f = parseFunc(base, err);
    if (o.mode == "exhaustive") {
        long count = 0;
        std::unordered_set<size_t> seen;
        for (auto* p : passes) {
            long mine = 0;
            p->enumerate(*f, [&](Mutation m) {
                std::string s = m();
                if (s.empty() || !seen.insert(hashText(s)).second) return true;
                mine++;
                if (++count <= 50) std::cout << "=== " << p->name << " #" << mine << "\n" << lineDiff(base, s);
                return count < o.maxCandidates;
            });
            std::cerr << p->name << ": " << mine << " candidates\n";
        }
        std::cerr << count << " candidates in total\n";
        return 0;
    }
    Rng rng(o.seedSet ? o.seed : 1);
    long n = o.iterations ? o.iterations : 10;
    for (long i = 0; i < n; ++i) {
        std::string s = randomCandidate(o, passes, base, rng);
        std::cout << "=== random #" << i + 1 << "\n" << (s.empty() ? "(no change)\n" : lineDiff(base, s));
    }
    return 0;
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
    if (o.source.empty() || o.function.empty()) {
        std::cerr << "error: --source and --function are required\n\n" << kUsage;
        return 2;
    }
    std::string src;
    if (!readFile(o.source, src)) {
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
    auto passes = pickPasses(o, err);
    if (passes.empty()) {
        std::cerr << "error: " << err << "\n";
        return 2;
    }
    if (o.mode != "random" && o.mode != "exhaustive") {
        std::cerr << "error: --mode must be random or exhaustive\n";
        return 2;
    }
    if (o.dryRun) return dryRun(o, funcText, passes);

    if (o.compile.empty() || (o.targetObj.empty() && o.scoreCmd.empty())) {
        std::cerr << "error: --compile and --target-obj (or --score-cmd) are required\n";
        return 2;
    }
    if (o.jobs < 1) o.jobs = 1;

    Evaluator ev{o};
    ev.sc.objdump = o.objdump;
    ev.sc.ignoreRelocNames = o.ignoreRelocNames;
    ev.prefix = src.substr(0, start);
    ev.suffix = src.substr(end);
    fs::path sp = fs::absolute(o.source);
    ev.srcDir = sp.parent_path().string();
    ev.stem = sp.stem().string();
    ev.ext = sp.extension().string();
    fs::create_directories(fs::path(o.outputDir) / ".work");

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    auto cleanup = [&]() {
        std::error_code ec;
        for (int w = 0; w < o.jobs; ++w) fs::remove(ev.srcPath(w), ec);
    };

    // the unmodified function
    std::string log;
    if (!ev.compile(0, funcText, log)) {
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
    st.baseText = st.bestText = funcText;
    st.baseScore = st.bestScore = base;
    st.seen.insert(hashText(funcText));
    if (base == 0) {
        std::cout << "already matching\n";
        cleanup();
        return 0;
    }
    if (o.mode == "random") runRandom(o, ev, st, passes);
    else runExhaustive(o, ev, st, passes);
    cleanup();

    std::cout << "\n";
    if (st.bestScore == 0) std::cout << "MATCH found";
    else if (st.bestScore < base) std::cout << "best score " << st.bestScore << " (base " << base << ")";
    else std::cout << "no improvement over base score " << base;
    std::cout << " after " << st.compiles << " compiles (" << st.failures << " failed)\n";
    return st.bestScore == 0 ? 0 : 3;
}
