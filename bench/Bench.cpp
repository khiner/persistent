// Standing benchmark against immer, the reference implementation.
//   cmake --build build --target HamtBench && ./build/bench/HamtBench
// Sweep the branching factor by configuring a second build dir with -DHAMT_BITS=5.

#include "Hamt.h"

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/algorithm.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#pragma clang diagnostic pop

#include <pthread.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using ImmerMap = immer::map<uint64_t, uint64_t>;

// Nanoseconds per operation, best of `Rounds`. Timing noise only ever adds, so the minimum is the
// closest we get to the real cost. `Sink` is there to keep the optimizer from deleting the work.
//
// A round repeats the work until it has run for `MinRoundNanos`, then divides by the number of
// repeats. A thousand lookups take a couple of microseconds, close enough to the clock's own cost that
// the reading is mostly noise, so the small sizes are only comparable once a round runs for
// milliseconds.
// `HAMT_BENCH_ROUNDS` buys wall clock with steadiness, and the whole sweep costs in proportion. Nine
// reads a single cell more steadily and is worth setting to read one. It does not make the summary
// below any steadier, which is what a change is judged on, so three is the default.
const uint32_t Rounds = [] {
    const auto *rounds = std::getenv("HAMT_BENCH_ROUNDS");
    const auto n = rounds ? std::strtoul(rounds, nullptr, 10) : 0;
    return n ? uint32_t(n) : 3;
}();
constexpr double MinRoundNanos = 2e6;
uint64_t Sink = 0;

template<typename F> double Time(uint64_t ops, F &&f) {
    double best = 1e300;
    for (uint32_t i = 0; i < Rounds; ++i) {
        uint64_t reps = 0;
        double ns;
        const auto start = Clock::now();
        do {
            Sink += f();
            ++reps;
            ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        } while (ns < MinRoundNanos);
        const auto per_op = ns / double(ops * reps);
        best = per_op < best ? per_op : best;
    }
    return best;
}

// Key shape matters as much as key count. immer feeds the key straight to the trie, so a dense key set
// gives it a dense trie and one with zeroed low bits wastes whole levels. Our fold leaves both shapes
// alone, so these sweeps compare like with like. A hash that scrambled would trade the wasted levels
// for a sparse trie on every shape.
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

// The sweep's verdict, accumulated across every row it prints. One cell's margin moves by a tenth on
// nothing but where the linker put the code, which rerunning the same binary does not resample, so a
// single cell is not evidence on its own. The mean over all of them holds to a few tenths of a point,
// because a layout that costs one cell pays another back, and that is what a change is judged on.
double LogRatios = 0;
uint32_t Cells = 0, Behind = 0;

void Row(const char *name, double ours, double immer) {
    std::printf("  %-14s %8.2f %8.2f   %+6.1f%%\n", name, ours, immer, 100 * (immer - ours) / immer);
    LogRatios += std::log(immer / ours);
    Behind += ours > immer;
    ++Cells;
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
    // through the size classes and frees the smaller one each time, so the holes it leaves are the
    // difference between the two numbers.
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
    // Both sides move, which is what lets a write rewrite a path in place instead of copying it: ours
    // because `Erase` and `Update` take the map by value, immer through the rvalue overloads its
    // default memory policy turns on. Called on an lvalue instead it copies every path, which is 2.7x
    // on erase and 2.9x on update at a million keys -- a difference far larger than anything the rows
    // are meant to be reporting, so the call has to be written this way to measure what it says.
    Row("erase move",
        Time(n, [&] { auto m = ours; for (auto key : keys) m = hamt::Erase(std::move(m), key); return m.Size; }),
        Time(n, [&] { auto m = theirs; for (auto key : keys) m = std::move(m).erase(key); return m.size(); }));
    Row("update move",
        Time(n, [&] { auto m = ours; for (auto key : keys) m = hamt::Update(std::move(m), key, [](uint64_t v) { return v + 1; }); return m.Size; }),
        Time(n, [&] { auto m = theirs; for (auto key : keys) m = std::move(m).update(key, [](uint64_t v) { return v + 1; }); return m.size(); }));
    // Both libraries walk a map two ways, and the two are worth separating. An iterator has to hold
    // its position between steps, where a callback keeps it in the call stack, and on immer that is
    // worth up to 28% at a million keys -- so an iterator row alone would compare our best walk
    // against their second-best, which is how the erase row above used to read.
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

    // Diff has two regimes and they are orders of magnitude apart, so each gets a row. The first is
    // what reconciliation actually asks for: a map derived from another by a few writes, where both
    // implementations settle the untouched structure by pointer and pay for the paths down to what
    // moved and nothing else. Timed per change reported rather than per key, since the point of the
    // row is that the size of the map does not enter into it.
    const uint64_t edits = n / 100 ? n / 100 : 1, edit_stride = n / edits;
    auto ours_edited = ours;
    auto theirs_edited = theirs;
    for (uint64_t i = 0; i < edits; ++i) {
        ours_edited = hamt::Set(std::move(ours_edited), keys[i * edit_stride], ~i);
        theirs_edited = theirs_edited.set(keys[i * edit_stride], ~i);
    }
    Row("diff edits",
        Time(edits, [&] { uint64_t s = 0; hamt::Diff(ours, ours_edited, [&s](const hamt::Change &c) { s += c.Key; }); return s; }),
        Time(edits, [&] { uint64_t s = 0; immer::diff(theirs, theirs_edited, [&s](const auto &e) { s += e.first; }, [&s](const auto &e) { s += e.first; }, [&s](const auto &, const auto &e) { s += e.first; }); return s; }));
    // The other regime: the same bindings with no history in common, which neither implementation can
    // shortcut, so both walk the whole of both tries to report nothing. Per key, and the floor on
    // what a diff between unrelated maps costs until a node carries a hash of its own contents.
    Row("diff twins",
        Time(n, [&] { uint64_t s = 0; hamt::Diff(ours, ours_twin, [&s](const hamt::Change &c) { s += c.Key; }); return s; }),
        Time(n, [&] { uint64_t s = 0; immer::diff(theirs, theirs_twin, [&s](const auto &e) { s += e.first; }, [&s](const auto &e) { s += e.first; }, [&s](const auto &, const auto &e) { s += e.first; }); return s; }));
}
} // namespace

int main(int argc, char **argv) {
    // Apple silicon runs a plain process on whichever core is free, and an efficiency core is about
    // half as fast here. Best-of-`Rounds` only hides that when some round lands on a performance core,
    // so without this the readings are bimodal.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::printf("ns/op, lower is better. Last column is our margin over immer.\n");
    // An optional leading key pattern, then sizes.
    //   HamtBench                     the three default sizes on random keys
    //   HamtBench sequential 1000000  one size, one pattern
    //   HamtBench all 100000          every pattern at one size
    //   HAMT_BENCH_ROUNDS=9 HamtBench all   three times the work, for reading one cell closely
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
    std::printf("\n%u cells, %u behind immer, geometric mean %+.1f%%  (checksum %llu)\n", Cells, Behind, 100 * (1 - std::exp(-LogRatios / Cells)), (unsigned long long)Sink);
}
