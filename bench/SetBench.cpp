// Standing benchmark against immer, the reference implementation.
//   cmake --build build --target SetBench && ./build/bench/SetBench
// Sweep the branching factor by configuring a second build dir with -DHAMT_BITS=6.
//
// The map's sweep is next door in Bench.cpp, and the rows here are its rows over bare keys. The one it
// does not have is `insert held`: a key already in the set is not rebound, so the write copies nothing.

#include "Bench.h"
#include "Hamt.h"

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/algorithm.hpp>
#include <immer/set.hpp>
#include <immer/set_transient.hpp>
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
using ImmerSet = immer::set<uint64_t>;

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

hamt::Set Built(const std::vector<uint64_t> &keys) {
    hamt::Set s{};
    for (const auto key : keys) s = hamt::Insert(std::move(s), key);
    return s;
}

ImmerSet ImmerBuilt(const std::vector<uint64_t> &keys) {
    auto t = ImmerSet{}.transient();
    for (const auto key : keys) t.insert(key);
    return t.persistent();
}

void Run(uint64_t n, Pattern pattern) {
    const auto keys = Keys(n, pattern, 0), absent = Keys(n, pattern, 1);
    const auto ours = Built(keys);
    const auto theirs = ImmerBuilt(keys);

    std::printf("\nn = %llu, %s keys\n", (unsigned long long)n, PatternNames[int(pattern)]);
    // Only our set is live here, so a walk over it covers exactly this. A build grows every node up
    // through the size classes, freeing the smaller one each time, so the holes it leaves are the gap.
    if (const auto held = hamt::Held())
        std::printf("  %-14s %8.2f MB of node spread over %.2f MB of slab, %.0f%% full (%.0f MB reserved)\n", "footprint", held->LiveBytes / 1048576.0, held->SpannedBytes / 1048576.0, 100.0 * double(held->LiveBytes) / double(held->SpannedBytes), held->ReservedBytes / 1048576.0);
    std::printf("  %-14s %8s %8s %9s\n", "", "ours", "immer", "ours vs");

    Row("build copy",
        Time(n, [&] { hamt::Set s{}; for (const auto key : keys) s = hamt::Insert(s, key); return s.Size; }),
        Time(n, [&] { ImmerSet s; for (const auto key : keys) s = s.insert(key); return s.size(); }));
    Row("build move",
        Time(n, [&] { return Built(keys).Size; }),
        Time(n, [&] { return ImmerBuilt(keys).size(); }));
    // Handed every key up front, which is a different algorithm and not just a different spelling: ours
    // partitions by hash into the nodes that will hold them, immer's range constructor inserts into a
    // transient. Both are the fastest route each library has from a range to a set.
    Row("build range",
        Time(n, [&] { return hamt::Set{keys.begin(), keys.end()}.Size; }),
        Time(n, [&] { return ImmerSet{keys.begin(), keys.end()}.size(); }));
    Row("lookup hit",
        Time(n, [&] { uint64_t s = 0; for (const auto key : keys) s += hamt::Contains(ours, key); return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto key : keys) s += theirs.find(key) != nullptr; return s; }));
    Row("lookup miss",
        Time(n, [&] { uint64_t s = 0; for (const auto key : absent) s += hamt::Contains(ours, key); return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto key : absent) s += theirs.find(key) != nullptr; return s; }));
    // A write that finds the key already there. Ours hands the set back untouched, so this is a lookup and
    // nothing more, and the set is copied rather than moved to prove it. The widest margin in either sweep
    // and the least of a design difference: immer's `do_add` serves map and table too, and those have to
    // replace on an equal key, so its set copies a path it has no reason to.
    Row("insert held",
        Time(n, [&] { uint64_t s = 0; for (const auto key : keys) s += hamt::Insert(ours, key).Size; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto key : keys) s += theirs.insert(key).size(); return s; }));
    // Both sides move, so a write can rewrite a path in place: ours because `Erase` takes the set by
    // value, immer through its default policy's rvalue overloads.
    Row("erase move",
        Time(n, [&] { auto s = ours; for (const auto key : keys) s = hamt::Erase(std::move(s), key); return s.Size; }),
        Time(n, [&] { auto s = theirs; for (const auto key : keys) s = std::move(s).erase(key); return s.size(); }));
    // Both libraries walk a set two ways, worth separating. An iterator has to hold its position between
    // steps where a callback keeps it in the call stack.
    Row("iterate",
        Time(n, [&] { uint64_t s = 0; for (const auto key : ours) s += key; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto &key : theirs) s += key; return s; }));
    Row("for each",
        Time(n, [&] { uint64_t s = 0; hamt::ForEach(ours, [&s](uint64_t key) { s += key; }); return s; }),
        Time(n, [&] { uint64_t s = 0; immer::for_each(theirs, [&s](const auto &key) { s += key; }); return s; }));
    // Built separately from the same keys, so equality has no shared history to short-circuit on.
    const auto ours_twin = Built(keys);
    const auto theirs_twin = ImmerBuilt(keys);
    Row("equal",
        Time(n, [&] { return uint64_t(ours == ours_twin); }),
        Time(n, [&] { return uint64_t(theirs == theirs_twin); }));

    // Diff has two regimes, orders of magnitude apart, so each gets a row. The first measures what
    // reconciliation asks for: a set derived from another by a few writes, where both settle the
    // untouched structure by pointer. Timed per change reported, since the size of the set does not
    // enter in. A key is erased and not rebound, there being nothing in a set to rebind.
    const uint64_t edits = n / 100 ? n / 100 : 1, edit_stride = n / edits;
    auto ours_edited = ours;
    auto theirs_edited = theirs;
    for (uint64_t i = 0; i < edits; ++i) {
        ours_edited = hamt::Erase(std::move(ours_edited), keys[i * edit_stride]);
        theirs_edited = theirs_edited.erase(keys[i * edit_stride]);
    }
    const auto our_diff = [](const hamt::Set &x, const hamt::Set &y) { uint64_t s = 0; hamt::Diff(x, y, [&s](uint64_t key, bool) { s += key; }); return s; };
    const auto immer_diff = [](const ImmerSet &x, const ImmerSet &y) { uint64_t s = 0; const auto add = [&s](const auto &key) { s += key; }; immer::diff(x, y, add, add, [&s](const auto &, const auto &key) { s += key; }); return s; };
    Row("diff edits",
        Time(edits, [&] { return our_diff(ours, ours_edited); }),
        Time(edits, [&] { return immer_diff(theirs, theirs_edited); }));
    // The other regime: the same keys with no history in common, which neither can shortcut, so both walk
    // the whole of both tries to report nothing. Per key, and the floor until a node carries a hash.
    Row("diff twins",
        Time(n, [&] { return our_diff(ours, ours_twin); }),
        Time(n, [&] { return immer_diff(theirs, theirs_twin); }));
}
} // namespace

int main(int argc, char **argv) {
    bench::Begin();
    // An optional leading key pattern, then sizes.
    //   SetBench                     the three default sizes on random keys
    //   SetBench sequential 1000000  one size, one pattern
    //   SetBench all 100000          every pattern at one size
    //   BENCH_ROUNDS=9 SetBench all         three times the work, for reading one cell closely
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
