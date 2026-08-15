// Standing benchmark against immer, the reference implementation.
//   cmake --build build --target HamtBench && ./build/bench/HamtBench
// Sweep the branching factor by configuring a second build dir with -DHAMT_BITS=5.

#include "Hamt.h"

// Included with plain -I rather than -isystem (see bench/CMakeLists.txt), so silence immer's warnings here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#pragma clang diagnostic pop

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using ImmerMap = immer::map<uint64_t, uint64_t>;

// Nanoseconds per operation, best of `Rounds`. Timing noise only ever adds, so the minimum is the
// closest we get to the real cost. `Sink` is there to keep the optimizer from deleting the work.
constexpr uint32_t Rounds = 5;
uint64_t Sink = 0;

template<typename F> double Time(uint64_t ops, F &&f) {
    double best = 1e300;
    for (uint32_t i = 0; i < Rounds; ++i) {
        const auto start = Clock::now();
        Sink += f();
        const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        best = ns < best ? ns : best;
    }
    return best / double(ops);
}

// How the keys are shaped matters as much as how many there are. immer feeds the key straight to the
// trie, so a dense key set gives it a dense trie and every key the same depth, while a key set with
// zeroed low bits wastes whole levels. We mix, so all four of these look the same to us.
enum class Pattern { Random,
                     Sequential,
                     Pointer16,
                     Pointer64 };
constexpr const char *PatternNames[]{"random", "sequential", "pointer x16", "pointer x64"};

// `run` separates the present keys from the absent ones without changing the shape of either.
std::vector<uint64_t> Keys(uint64_t n, Pattern p, uint64_t run) {
    std::vector<uint64_t> keys(n);
    if (p == Pattern::Random) {
        std::mt19937_64 rng{run};
        for (auto &key : keys) key = rng();
        return keys;
    }
    // A counter, and two rates of heap address, one object per 16 or 64 bytes.
    const uint64_t base = p == Pattern::Sequential ? 0 : 0x600000000000ull, stride = p == Pattern::Pointer16 ? 16 : p == Pattern::Pointer64 ? 64 :
                                                                                                                                              1;
    for (uint64_t i = 0; i < n; ++i) keys[i] = base + (run * n + i) * stride;
    return keys;
}

hamt::Map Built(const std::vector<uint64_t> &keys) {
    hamt::Map m{};
    for (uint64_t i = 0; i < keys.size(); ++i) m = hamt::Set(std::move(m), keys[i], i);
    return m;
}

ImmerMap ImmerBuilt(const std::vector<uint64_t> &keys) {
    auto t = ImmerMap{}.transient();
    for (uint64_t i = 0; i < keys.size(); ++i) t.set(keys[i], i);
    return t.persistent();
}

void Row(const char *name, double ours, double immer) {
    std::printf("  %-14s %8.1f %8.1f   %+6.1f%%\n", name, ours, immer, 100 * (immer - ours) / immer);
}

void Run(uint64_t n, Pattern pattern) {
    const auto keys = Keys(n, pattern, 0), absent = Keys(n, pattern, 1);
    const auto ours = Built(keys);
    const auto theirs = ImmerBuilt(keys);

    std::printf("\nn = %llu, %s keys\n  %-14s %8s %8s %9s\n", (unsigned long long)n, PatternNames[int(pattern)], "", "ours", "immer", "ours vs");

    Row("build copy",
        Time(n, [&] { hamt::Map m{}; for (uint64_t i = 0; i < n; ++i) m = hamt::Set(m, keys[i], i); return m.Size; }),
        Time(n, [&] { ImmerMap m; for (uint64_t i = 0; i < n; ++i) m = m.set(keys[i], i); return m.size(); }));
    Row("build move",
        Time(n, [&] { return Built(keys).Size; }),
        Time(n, [&] { return ImmerBuilt(keys).size(); }));
    Row("lookup hit",
        Time(n, [&] { uint64_t s = 0; for (auto key : keys) s += *hamt::Get(ours, key); return s; }),
        Time(n, [&] { uint64_t s = 0; for (auto key : keys) s += *theirs.find(key); return s; }));
    Row("lookup miss",
        Time(n, [&] { uint64_t s = 0; for (auto key : absent) s += hamt::Get(ours, key).has_value(); return s; }),
        Time(n, [&] { uint64_t s = 0; for (auto key : absent) s += theirs.find(key) != nullptr; return s; }));
    Row("erase move",
        Time(n, [&] { auto m = ours; for (auto key : keys) m = hamt::Erase(std::move(m), key); return m.Size; }),
        Time(n, [&] { auto m = theirs; for (auto key : keys) m = m.erase(key); return m.size(); }));
    Row("iterate",
        Time(n, [&] { uint64_t s = 0; for (const auto &e : ours) s += e.Value; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto &e : theirs) s += e.second; return s; }));
    // Built separately from the same keys, so the two maps share nothing and equality has to walk them.
    const auto ours_twin = Built(keys);
    const auto theirs_twin = ImmerBuilt(keys);
    Row("equal",
        Time(n, [&] { return uint64_t(ours == ours_twin); }),
        Time(n, [&] { return uint64_t(theirs == theirs_twin); }));
}
} // namespace

int main(int argc, char **argv) {
    std::printf("ns/op, lower is better. Last column is our margin over immer.\n");
    // An optional leading key pattern, then sizes, for sweeping a curve finer than the defaults
    // resolve or for driving the shapes a counter and a heap allocator actually produce.
    //   HamtBench                     the three default sizes on random keys
    //   HamtBench sequential 1000000  one size, one pattern
    //   HamtBench all 100000          every pattern at one size
    int arg = 1;
    std::vector<Pattern> patterns{Pattern::Random};
    if (argc > 1 && !std::isdigit(static_cast<unsigned char>(argv[1][0]))) {
        ++arg;
        const std::string_view name{argv[1]};
        if (name == "all") patterns = {Pattern::Random, Pattern::Sequential, Pattern::Pointer16, Pattern::Pointer64};
        else {
            patterns.clear();
            for (int i = 0; i < 4; ++i)
                if (name == PatternNames[i] || (name == "pointer" && i >= 2)) patterns.push_back(Pattern(i));
            if (patterns.empty()) {
                std::printf("unknown key pattern '%s'\n", argv[1]);
                return 1;
            }
        }
    }
    for (const auto pattern : patterns) {
        if (arg < argc) {
            for (int i = arg; i < argc; ++i) Run(std::strtoull(argv[i], nullptr, 10), pattern);
        } else {
            for (const uint64_t n : {uint64_t{1'000}, uint64_t{100'000}, uint64_t{1'000'000}}) Run(n, pattern);
        }
    }
    std::printf("\n(checksum %llu)\n", (unsigned long long)Sink);
}
