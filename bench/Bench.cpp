// Standing benchmark against immer, the reference implementation.
//   cmake --build build --target HamtBench && ./build/bench/HamtBench
// Sweep the branching factor by configuring a second build dir with -DHAMT_BITS=6.

#include "Bench.h"
#include "Hamt.h"

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/algorithm.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#pragma clang diagnostic pop

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace bench;
using ImmerMap = immer::map<uint64_t, uint64_t>;

// Key shape matters as much as key count. immer feeds the key straight to the trie, so a dense key set
// gives it a dense trie and one with zeroed low bits wastes whole levels. Our fold leaves both alone,
// so these sweeps compare like with like.
enum class Pattern { Random,
                     Sequential,
                     Pointer16,
                     Pointer64 };
constexpr const char *PatternNames[]{"random", "sequential", "pointer x16", "pointer x64"};
// A counter, and two rates of heap address, one object per 16 or 64 bytes. Unused for random keys.
constexpr uint64_t PatternStrides[]{0, 1, 16, 64};
constexpr int PatternCount = std::size(PatternNames);

// `run` separates the present keys from the absent ones without changing the shape of either.
std::vector<uint64_t> Keys(uint64_t n, Pattern p, uint64_t run) {
    std::vector<uint64_t> keys(n);
    if (p == Pattern::Random) {
        std::mt19937_64 rng{run};
        for (auto &key : keys) key = rng();
        return keys;
    }
    const uint64_t base = p == Pattern::Sequential ? 0 : 0x600000000000ull, stride = PatternStrides[int(p)];
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

void Run(uint64_t n, Pattern pattern) {
    const auto keys = Keys(n, pattern, 0), absent = Keys(n, pattern, 1);
    const auto ours = Built(keys);
    const auto theirs = ImmerBuilt(keys);
    // The same bindings as a plain range, for the constructors that take one.
    std::vector<hamt::Entry> entries;
    std::vector<std::pair<uint64_t, uint64_t>> pairs;
    entries.reserve(n);
    pairs.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
        entries.emplace_back(keys[i], i);
        pairs.emplace_back(keys[i], i);
    }

    std::printf("\nn = %llu, %s keys\n", (unsigned long long)n, PatternNames[int(pattern)]);
    // Only our map is live here, so this is what a walk over it covers. A build grows every node up
    // through the size classes, freeing the smaller one each time, so the holes it leaves are the gap.
    if (const auto held = hamt::Held())
        std::printf("  %-14s %8.2f MB of node spread over %.2f MB of slab, %.0f%% full (%.0f MB reserved)\n", "footprint", held->LiveBytes / 1048576.0, held->SpannedBytes / 1048576.0, 100.0 * double(held->LiveBytes) / double(held->SpannedBytes), held->ReservedBytes / 1048576.0);
    std::printf("  %-14s %8s %8s %9s\n", "", "ours", "immer", "ours vs");

    Row("build copy",
        Time(n, [&] { hamt::Map m{}; for (uint64_t i = 0; i < n; ++i) m = hamt::Set(m, keys[i], i); return m.Size; }),
        Time(n, [&] { ImmerMap m; for (uint64_t i = 0; i < n; ++i) m = m.set(keys[i], i); return m.size(); }));
    Row("build move",
        Time(n, [&] { return Built(keys).Size; }),
        Time(n, [&] { return ImmerBuilt(keys).size(); }));
    // Handed every binding up front, which is a different algorithm and not just a different spelling:
    // ours partitions by hash into the nodes that will hold them, immer's range constructor inserts
    // into a transient. Both are the fastest route each library has from a range to a map.
    Row("build range",
        Time(n, [&] { return hamt::Map{entries.begin(), entries.end()}.Size; }),
        Time(n, [&] { return ImmerMap{pairs.begin(), pairs.end()}.size(); }));
    Row("lookup hit",
        Time(n, [&] { uint64_t s = 0; for (auto key : keys) s += *hamt::Get(ours, key); return s; }),
        Time(n, [&] { uint64_t s = 0; for (auto key : keys) s += *theirs.find(key); return s; }));
    Row("lookup miss",
        Time(n, [&] { uint64_t s = 0; for (auto key : absent) s += hamt::Get(ours, key) != nullptr; return s; }),
        Time(n, [&] { uint64_t s = 0; for (auto key : absent) s += theirs.find(key) != nullptr; return s; }));
    // Both sides move, which is what lets a write rewrite a path in place: ours because `Erase` and
    // `Update` take the map by value, immer through its default policy's rvalue overloads. On an lvalue
    // both copy every path -- 2.7x on erase and 2.9x on update, swamping what these rows report.
    Row("erase move",
        Time(n, [&] { auto m = ours; for (auto key : keys) m = hamt::Erase(std::move(m), key); return m.Size; }),
        Time(n, [&] { auto m = theirs; for (auto key : keys) m = std::move(m).erase(key); return m.size(); }));
    Row("update move",
        Time(n, [&] { auto m = ours; for (auto key : keys) m = hamt::Update(std::move(m), key, [](uint64_t v) { return v + 1; }); return m.Size; }),
        Time(n, [&] { auto m = theirs; for (auto key : keys) m = std::move(m).update(key, [](uint64_t v) { return v + 1; }); return m.size(); }));
    // Both libraries walk a map two ways, worth separating. An iterator has to hold its position
    // between steps where a callback keeps it in the call stack, worth up to 28% on immer at a million
    // keys -- so an iterator row alone would compare our best walk against their second-best.
    Row("iterate",
        Time(n, [&] { uint64_t s = 0; for (const auto &e : ours) s += e.Value; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto &e : theirs) s += e.second; return s; }));
    Row("for each",
        Time(n, [&] { uint64_t s = 0; hamt::ForEach(ours, [&s](const hamt::Entry &e) { s += e.Value; }); return s; }),
        Time(n, [&] { uint64_t s = 0; immer::for_each(theirs, [&s](const auto &e) { s += e.second; }); return s; }));
    // Built separately from the same keys, so equality has no shared history to short-circuit on.
    const auto ours_twin = Built(keys);
    const auto theirs_twin = ImmerBuilt(keys);
    Row("equal",
        Time(n, [&] { return uint64_t(ours == ours_twin); }),
        Time(n, [&] { return uint64_t(theirs == theirs_twin); }));

    // Diff has two regimes, orders of magnitude apart, so each gets a row. The first is what reconciliation
    // asks for: a map derived from another by a few writes, where both settle the untouched structure by
    // pointer. Timed per change reported, since the point is that the size of the map does not enter in.
    const uint64_t edits = n / 100 ? n / 100 : 1, edit_stride = n / edits;
    auto ours_edited = ours;
    auto theirs_edited = theirs;
    for (uint64_t i = 0; i < edits; ++i) {
        ours_edited = hamt::Set(std::move(ours_edited), keys[i * edit_stride], ~i);
        theirs_edited = theirs_edited.set(keys[i * edit_stride], ~i);
    }
    const auto our_diff = [](const hamt::Map &x, const hamt::Map &y) { uint64_t s = 0; hamt::Diff(x, y, [&s](const hamt::Change &c) { s += c.Key; }); return s; };
    const auto immer_diff = [](const ImmerMap &x, const ImmerMap &y) { uint64_t s = 0; const auto add = [&s](const auto &e) { s += e.first; }; immer::diff(x, y, add, add, [&s](const auto &, const auto &e) { s += e.first; }); return s; };
    Row("diff edits",
        Time(edits, [&] { return our_diff(ours, ours_edited); }),
        Time(edits, [&] { return immer_diff(theirs, theirs_edited); }));
    // The other regime: the same bindings with no history in common, which neither can shortcut, so both
    // walk the whole of both tries to report nothing. Per key, and the floor until a node carries a hash.
    Row("diff twins",
        Time(n, [&] { return our_diff(ours, ours_twin); }),
        Time(n, [&] { return immer_diff(theirs, theirs_twin); }));
}
} // namespace

int main(int argc, char **argv) {
    bench::Begin();
    // An optional leading key pattern, then sizes.
    //   HamtBench                     the three default sizes on random keys
    //   HamtBench sequential 1000000  one size, one pattern
    //   HamtBench all 100000          every pattern at one size
    //   BENCH_ROUNDS=9 HamtBench all         three times the work, for reading one cell closely
    int arg = 1;
    std::vector<Pattern> patterns{Pattern::Random};
    if (argc > 1 && !std::isdigit(static_cast<unsigned char>(argv[1][0]))) {
        ++arg;
        // "all" takes every pattern and "pointer" both strides, so a name can select more than one.
        const std::string_view name{argv[1]};
        patterns.clear();
        for (int i = 0; i < PatternCount; ++i)
            if (name == "all" || name == PatternNames[i] || (name == "pointer" && i >= int(Pattern::Pointer16))) patterns.push_back(Pattern(i));
        if (patterns.empty()) {
            std::printf("unknown key pattern '%s'\n", argv[1]);
            return 1;
        }
    }
    for (const auto pattern : patterns) {
        if (arg < argc) {
            for (int i = arg; i < argc; ++i) Run(std::strtoull(argv[i], nullptr, 10), pattern);
        } else {
            for (const uint64_t n : {uint64_t{1'000}, uint64_t{100'000}, uint64_t{1'000'000}}) Run(n, pattern);
        }
    }
    bench::End();
}
