// Standing benchmark against immer, the reference implementation.
//   cmake --build build --target FlexVectorBench && ./build/bench/FlexVectorBench
//   BENCH_ROUNDS=9 ./build/bench/FlexVectorBench   three times the work, for reading one cell closely
// Sweep the branching factor by configuring a second build dir with -DVECTOR_BITS=5.
//
// Every row but the two build rows runs on a vector joined out of pieces of awkward length, so its
// trie is really relaxed. That is the shape the whole type exists for, and the one an index pays for.

#include "Bench.h"
#include "Vector.h"

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/algorithm.hpp>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#pragma clang diagnostic pop

#include <random>
#include <vector>

namespace {
using namespace bench;
using ImmerVector = immer::flex_vector<uint64_t>;

vec::FlexVector Built(const std::vector<uint64_t> &values) {
    vec::FlexVector v;
    for (const auto x : values) v = vec::PushBack(std::move(v), x);
    return v;
}

ImmerVector ImmerBuilt(const std::vector<uint64_t> &values) {
    auto t = ImmerVector{}.transient();
    for (const auto x : values) t.push_back(x);
    return t.persistent();
}

// The lengths the joined vector is assembled out of. Both libraries are handed the same ones, so the
// two hold the same elements while agreeing on nothing about their shapes.
std::vector<uint64_t> Pieces(uint64_t total) {
    std::mt19937_64 rng{2};
    std::vector<uint64_t> out;
    for (uint64_t done = 0; done < total;) {
        const auto piece = 1 + rng() % 400;
        out.push_back(piece < total - done ? piece : total - done);
        done += out.back();
    }
    return out;
}

vec::FlexVector Joined(const std::vector<uint64_t> &values, const std::vector<uint64_t> &pieces) {
    vec::FlexVector v;
    const auto *at = values.data();
    for (const auto piece : pieces) {
        v = vec::Concat(v, vec::FlexVector{at, at + piece});
        at += piece;
    }
    return v;
}

ImmerVector ImmerJoined(const std::vector<uint64_t> &values, const std::vector<uint64_t> &pieces) {
    ImmerVector v;
    const auto *at = values.data();
    for (const auto piece : pieces) {
        v = v + ImmerVector{at, at + piece};
        at += piece;
    }
    return v;
}

void Run(uint64_t n) {
    std::vector<uint64_t> values(n);
    std::mt19937_64 rng{1};
    for (auto &x : values) x = rng();
    // Read and write positions drawn up front, so no row pays for the random number generator.
    std::vector<uint64_t> indices(n), cuts(n);
    for (uint64_t i = 0; i < n; ++i) {
        indices[i] = rng() % n;
        cuts[i] = 1 + rng() % n;
    }

    const auto pieces = Pieces(n);
    const auto ours = Joined(values, pieces);
    const auto theirs = ImmerJoined(values, pieces);
    // The halves every join row puts back together.
    const auto ours_left = vec::Take(ours, n / 2), ours_right = vec::Drop(ours, n / 2);
    const auto theirs_left = theirs.take(n / 2), theirs_right = theirs.drop(n / 2);

    std::printf("\nn = %llu\n", (unsigned long long)n);
    if (const auto held = vec::Held())
        std::printf("  %-14s %8.2f MB of node spread over %.2f MB of slab, %.0f%% full (%.0f MB reserved)\n", "footprint", held->LiveBytes / 1048576.0, held->SpannedBytes / 1048576.0, 100.0 * double(held->LiveBytes) / double(held->SpannedBytes), held->ReservedBytes / 1048576.0);
    std::printf("  %-14s %8s %8s %9s\n", "", "ours", "immer", "ours vs");

    // The fastest route each library has from one element at a time to a vector: ours a moved-in vector
    // whose tail grows in place, immer's a transient. What comes out is strict: the shape every row
    // below gives up, and the point of measuring it here.
    Row("build move",
        Time(n, [&] { return Built(values).Size; }),
        Time(n, [&] { return ImmerBuilt(values).size(); }));
    Row("build range",
        Time(n, [&] { return vec::FlexVector{values.begin(), values.end()}.Size; }),
        Time(n, [&] { return ImmerVector{values.begin(), values.end()}.size(); }));
    // One join of two halves, so this cell is ns per concatenation and not per element.
    Row("concat",
        Time(1, [&] { return vec::Concat(ours_left, ours_right).Size; }),
        Time(1, [&] { return (theirs_left + theirs_right).size(); }));
    // A thousand of them onto the front of the vector, each one a join with a vector of one.
    Row("push front",
        Time(1'000, [&] { auto v = ours; for (uint64_t i = 0; i < 1'000; ++i) v = vec::PushFront(v, i); return v.Size; }),
        Time(1'000, [&] { auto v = theirs; for (uint64_t i = 0; i < 1'000; ++i) v = v.push_front(i); return v.size(); }));
    // Where a relaxed trie costs what a strict one does not: a search per level rather than a shift.
    Row("index random",
        Time(n, [&] { uint64_t s = 0; for (const auto i : indices) s += ours[i]; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto i : indices) s += theirs[i]; return s; }));
    // Both sides move, so a write can rewrite a path in place, and both spell it the same way: the
    // overload each library offers for a vector being given up.
    Row("set move",
        Time(n, [&] { auto v = ours; for (const auto i : indices) v = vec::Set(std::move(v), i, i); return v.Size; }),
        Time(n, [&] { auto v = theirs; for (const auto i : indices) v = std::move(v).set(i, i); return v.size(); }));
    Row("update move",
        Time(n, [&] { auto v = ours; for (const auto i : indices) v = vec::Update(std::move(v), i, [](uint64_t x) { return x + 1; }); return v.Size; }),
        Time(n, [&] { auto v = theirs; for (const auto i : indices) v = std::move(v).update(i, [](uint64_t x) { return x + 1; }); return v.size(); }));
    // Off a shared vector every time, so each cut copies the path it keeps rather than reusing the one
    // the cut before it left behind.
    Row("take",
        Time(n, [&] { uint64_t s = 0; for (const auto c : cuts) s += vec::Take(ours, c).Size; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto c : cuts) s += theirs.take(c).size(); return s; }));
    Row("drop",
        Time(n, [&] { uint64_t s = 0; for (const auto c : cuts) s += vec::Drop(ours, c).Size; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto c : cuts) s += theirs.drop(c).size(); return s; }));
    // Both libraries walk a vector two ways, worth separating: an iterator has to hold its position
    // between steps where a callback keeps it in the call stack.
    Row("iterate",
        Time(n, [&] { uint64_t s = 0; for (const auto x : ours) s += x; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto x : theirs) s += x; return s; }));
    Row("for each",
        Time(n, [&] { uint64_t s = 0; vec::ForEach(ours, [&s](uint64_t x) { s += x; }); return s; }),
        Time(n, [&] { uint64_t s = 0; immer::for_each(theirs, [&s](uint64_t x) { s += x; }); return s; }));
    // Joined separately from the same elements, so equality has no shared history to short-circuit on.
    const auto ours_twin = Joined(values, pieces);
    const auto theirs_twin = ImmerJoined(values, pieces);
    Row("equal",
        Time(n, [&] { return uint64_t(ours == ours_twin); }),
        Time(n, [&] { return uint64_t(theirs == theirs_twin); }));
}
} // namespace

int main(int argc, char **argv) {
    bench::Begin();
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) Run(std::strtoull(argv[i], nullptr, 10));
    } else {
        for (const uint64_t n : {uint64_t{1'000}, uint64_t{100'000}, uint64_t{1'000'000}}) Run(n);
    }
    bench::End();
}
