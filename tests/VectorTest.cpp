// Tests for the from-scratch radix balanced vector. The oracle is immer::vector, driven through the same
// operations and required to agree element for element. Both builds are worth running:
//   cmake --build build --target VectorTest VectorAuditTest
//   ./build/tests/VectorTest && ./build/tests/VectorAuditTest
// VectorAuditTest drops the node free list, so the reclamation check below can see anything at all.

#include "Vector.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/vector.hpp>
#pragma clang diagnostic pop

#include <random>
#include <vector>

// Ahead of `using namespace boost::ut`, so that `==` here is the vector's and not ut's expression builder.
namespace {
using Values = std::vector<uint64_t>;
using ImmerVector = immer::vector<uint64_t>;

// Sizes either branching factor makes something of: a chunk, a root, and a level, either side of each.
constexpr uint64_t Sizes[]{0, 1, 2, 31, 32, 33, 63, 64, 65, 1023, 1024, 1025, 2000, 4095, 4096, 4097, 32'769};

Values Elements(const ImmerVector &v) { return {v.begin(), v.end()}; }

Values Iterated(const vec::Vector &v) {
    Values out;
    for (const auto x : v) out.push_back(x);
    return out;
}

Values Walked(const vec::Vector &v) {
    Values out;
    vec::ForEach(v, [&out](uint64_t x) { out.push_back(x); });
    return out;
}

Values Indexed(const vec::Vector &v) {
    Values out;
    for (uint64_t i = 0; i < v.Size; ++i) out.push_back(v[i]);
    return out;
}

// Every route over the vector has to agree, and all of them with immer. A vector that disagrees with
// itself is as broken as one that disagrees with the oracle.
bool Holds(const vec::Vector &ours, const Values &expected) {
    if (!vec::Check(ours) || ours.Size != expected.size()) return false;
    if (!expected.empty() && (vec::Front(ours) != expected.front() || vec::Back(ours) != expected.back())) return false;
    return Indexed(ours) == expected && Iterated(ours) == expected && Walked(ours) == expected;
}

bool Holds(const vec::Vector &ours, const ImmerVector &theirs) { return Holds(ours, Elements(theirs)); }

// Distinct values that are not their own index, so a walk landing on the wrong element shows up.
Values Counting(uint64_t n) {
    Values out(n);
    for (uint64_t i = 0; i < n; ++i) out[i] = i * 7;
    return out;
}

vec::Vector Pushed(const Values &values) {
    vec::Vector v;
    for (const auto x : values) v = vec::PushBack(std::move(v), x);
    return v;
}

ImmerVector ImmerPushed(const Values &values) {
    ImmerVector v;
    for (const auto x : values) v = std::move(v).push_back(x);
    return v;
}
} // namespace

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const vec::Vector v;
        expect(v.Size == 0_u64);
        expect(vec::Check(v));
        expect(vec::Get(v, 0) == nullptr);
        expect(Iterated(v).empty());
        expect(Walked(v).empty());
        expect(v == vec::Vector{});
    };

    "transient"_test = [] {
        const vec::Vector original{1, 2, 3};
        auto t = original.transient();
        expect(t.size() == 3_u64 && !t.empty());
        expect(t[1] == 2_u64 && t.at(2) == 3_u64);
        t.push_back(4);
        t.set(0, 10);
        t.update(1, [](uint64_t value) { return value * 10; });
        const auto snapshot = t.persistent();
        expect((snapshot == vec::Vector{10, 20, 3, 4}) >> fatal);
        expect(original == vec::Vector{1, 2, 3});

        t.take(3);
        t.set(2, 30);
        expect(snapshot == vec::Vector{10, 20, 3, 4});
        const auto finished = std::move(t).persistent();
        expect(vec::Check(finished) >> fatal);
        expect(finished == vec::Vector{10, 20, 30});
        uint64_t visited = 0;
        for (const auto value : original.transient()) visited += value;
        expect(visited == 6_u64);
    };

    "push back"_test = [] {
        // Every size either branching factor makes something of, and the whole vector read back three
        // ways at each of them. Growth is the operation with the most shapes to get wrong.
        for (const auto n : Sizes) {
            const auto expected = Counting(n);
            expect(Holds(Pushed(expected), ImmerPushed(expected)) >> fatal) << "pushed" << n;
        }
    };

    "push back copies"_test = [] {
        // Without the move the old vector stays live, so every push has to leave it alone rather than
        // grow the tail it is standing on.
        const auto expected = Counting(1'100);
        vec::Vector v;
        std::vector<vec::Vector> snapshots;
        for (uint64_t i = 0; i < 1'100; ++i) {
            v = vec::PushBack(v, expected[i]);
            if (i % 97 == 0) snapshots.push_back(v);
        }
        expect(Holds(v, expected));
        for (uint64_t i = 0; i < snapshots.size(); ++i) {
            expect(Holds(snapshots[i], Values{expected.begin(), expected.begin() + 97 * i + 1}) >> fatal) << "snapshot" << i;
        }
    };

    "build"_test = [] {
        // The bulk build assembles the same shape the one-at-a-time route does, which `Check` pins down
        // and equality with a pushed twin confirms.
        for (const auto n : Sizes) {
            const auto expected = Counting(n);
            const vec::Vector built{expected.begin(), expected.end()};
            expect(Holds(built, expected) >> fatal) << "built" << n;
            expect(built == Pushed(expected)) << "built and pushed agree" << n;
        }
        const vec::Vector listed{1, 2, 3};
        expect(Holds(listed, Values{1, 2, 3}));
    };

    "set"_test = [] {
        for (const auto n : Sizes) {
            if (!n) continue;
            auto expected = Counting(n);
            auto ours = Pushed(expected);
            auto theirs = ImmerPushed(expected);
            // The ends and the middle, and enough of the rest to reach every level of a deep trie.
            std::mt19937_64 rng{n};
            for (uint64_t round = 0; round < 40; ++round) {
                const uint64_t index = round < 2 ? round * (n - 1) : rng() % n;
                const auto value = 1'000'000 + round;
                expected[index] = value;
                ours = vec::Set(std::move(ours), index, value);
                theirs = std::move(theirs).set(index, value);
            }
            expect(Holds(ours, theirs) >> fatal) << "set into" << n;
            expect(Holds(ours, expected) >> fatal) << "set into" << n;
        }
    };

    "set copies"_test = [] {
        // A shared trie has to be copied down the path rather than written through.
        const auto expected = Counting(5'000);
        const auto ours = Pushed(expected);
        auto edited = ours;
        for (uint64_t i = 0; i < 5'000; i += 17) edited = vec::Set(std::move(edited), i, ~i);
        expect(Holds(ours, expected)) << "the original is untouched";
        auto changed = expected;
        for (uint64_t i = 0; i < 5'000; i += 17) changed[i] = ~i;
        expect(Holds(edited, changed));
    };

    "update"_test = [] {
        const auto values = Counting(3'000);
        auto ours = Pushed(values);
        auto theirs = ImmerPushed(values);
        for (uint64_t i = 0; i < 3'000; i += 13) {
            ours = vec::Update(std::move(ours), i, [](uint64_t current) { return current * 2 + 1; });
            theirs = std::move(theirs).update(i, [](uint64_t current) { return current * 2 + 1; });
        }
        expect(Holds(ours, theirs));

        // Again without the move, which has to leave `ours` standing on what it holds now.
        const auto held = Iterated(ours);
        const auto once_more = vec::Update(ours, 7, [](uint64_t current) { return current + 100; });
        expect(Holds(ours, held)) << "the original is untouched";
        auto changed = held;
        changed[7] += 100;
        expect(Holds(once_more, changed));
    };

    "take"_test = [] {
        // Every cut point of a vector deep enough to have levels to drop, since a cut that empties a
        // level has to leave the root standing as low as it can.
        const auto values = Counting(2'100);
        const auto ours = Pushed(values);
        const auto theirs = ImmerPushed(values);
        for (uint64_t n = 0; n <= 2'100; ++n) {
            expect(Holds(vec::Take(ours, n), theirs.take(n)) >> fatal) << "took" << n;
        }
        expect(Holds(vec::Take(ours, 3'000), values)) << "a cut past the end is the whole vector";
        expect(Holds(ours, values)) << "the original is untouched";

        // Deep enough that the cut has more than one level to drop at once.
        const auto deep_values = Counting(70'000);
        const auto deep = Pushed(deep_values);
        const auto deep_theirs = ImmerPushed(deep_values);
        for (const uint64_t n : {uint64_t{1}, uint64_t{31}, uint64_t{32}, uint64_t{33}, uint64_t{1'024}, uint64_t{40'000}, uint64_t{69'999}}) {
            expect(Holds(vec::Take(deep, n), deep_theirs.take(n)) >> fatal) << "took" << n << "of 70,000";
        }
        // A cut and then a push, so the tail the cut left behind has to be a real one.
        auto cut = vec::Take(deep, 40'000);
        auto immer_cut = deep_theirs.take(40'000);
        for (uint64_t i = 0; i < 5'000; ++i) {
            cut = vec::PushBack(std::move(cut), i);
            immer_cut = std::move(immer_cut).push_back(i);
        }
        expect(Holds(cut, immer_cut));
    };

    "equality"_test = [] {
        for (const auto n : Sizes) {
            const auto values = Counting(n);
            const auto a = Pushed(values);
            // Built separately, so equality has no shared structure to short-circuit on, and by the other
            // route as well, so the two builds have to agree on the shape and not merely the contents.
            const vec::Vector b{values.begin(), values.end()};
            expect((a == b) >> fatal) << "twins of" << n;
            // A copy stands on the same nodes, the shortcut equality takes before reading any.
            expect((a == vec::Vector{a}) >> fatal) << "itself" << n;
            if (!n) continue;
            expect(!(a == vec::PushBack(a, 0))) << "one longer" << n;
            expect(!(a == vec::Set(a, n - 1, ~uint64_t{0}))) << "one element apart" << n;
            expect(!(a == vec::Take(a, n - 1))) << "one shorter" << n;
        }
    };

    "mixed writes against the oracle"_test = [] {
        // Moving into a write lets it reuse the old vector's nodes rather than copy them. A snapshot taken
        // part way through has to survive every write that follows.
        std::mt19937_64 rng{7};
        vec::Vector ours;
        ImmerVector theirs;
        for (uint64_t i = 0; i < 3'000; ++i) {
            ours = vec::PushBack(std::move(ours), rng());
            theirs = std::move(theirs).push_back(ours[i]);
        }
        const auto ours_snapshot = ours;
        const auto theirs_snapshot = theirs;

        for (uint64_t i = 0; i < 20'000; ++i) {
            switch (rng() % 4) {
                case 0: {
                    const auto value = rng();
                    ours = vec::PushBack(std::move(ours), value);
                    theirs = std::move(theirs).push_back(value);
                    break;
                }
                case 1: {
                    const auto index = rng() % ours.Size, value = rng();
                    ours = vec::Set(std::move(ours), index, value);
                    theirs = std::move(theirs).set(index, value);
                    break;
                }
                case 2: {
                    const auto index = rng() % ours.Size;
                    ours = vec::Update(std::move(ours), index, [](uint64_t current) { return current ^ 0x5555; });
                    theirs = std::move(theirs).update(index, [](uint64_t current) { return current ^ 0x5555; });
                    break;
                }
                default: {
                    // Never all the way down, so the vector stays deep enough to be worth writing to.
                    const auto n = 1'000 + rng() % (ours.Size - 999);
                    ours = vec::Take(std::move(ours), n);
                    theirs = std::move(theirs).take(n);
                    break;
                }
            }
        }
        expect(Holds(ours, theirs));
        expect(Holds(ours_snapshot, theirs_snapshot)) << "the snapshot survived every write after it";
    };

    "iterating pins the structure"_test = [] {
        // The walk keeps yielding the elements the vector had when it began, even as that vector is
        // written to underneath it.
        constexpr uint64_t N = 2'000;
        auto v = Pushed(Counting(N));
        uint64_t seen = 0;
        for (auto it = vec::begin(v); it != vec::end(v); ++it) {
            expect(*it == seen * 7) << "the walk saw a rewritten element";
            v = vec::Set(std::move(v), seen, 1'000'000 + seen);
            ++seen;
        }
        expect(seen == N);
        for (uint64_t i = 0; i < N; ++i) expect(v[i] == 1'000'000 + i);
    };

    "chunks"_test = [] {
        // The element walks are built on the chunked walk, so its boundaries are worth pinning down:
        // whole chunks all the way to the tail, and never an empty one.
        for (const auto n : Sizes) {
            const auto v = Pushed(Counting(n));
            std::vector<uint64_t> lengths;
            vec::ForEachChunk(v, [&lengths](const uint64_t *first, const uint64_t *last) { lengths.push_back(uint64_t(last - first)); });
            uint64_t total = 0;
            bool shaped = true;
            for (uint64_t i = 0; i < lengths.size(); ++i) {
                total += lengths[i];
                if (lengths[i] == 0 || (i + 1 < lengths.size() && lengths[i] != lengths[0])) shaped = false;
            }
            expect(total == n) << "chunks of" << n;
            expect(shaped) << "chunks of" << n;
        }
    };

    "reclamation"_test = [] {
        // Every other test would pass just the same if a reference count leaked or went one too far.
        {
            const auto values = Counting(20'000);
            auto v = Pushed(values);
            const auto shared = v; // A second handle, so every write below copies its path.
            for (uint64_t i = 0; i < 20'000; i += 3) v = vec::Set(v, i, i);
            v = vec::Take(std::move(v), 7'000);
            const vec::Vector built{values.begin(), values.end()};
            expect(v.Size == 7'000_u64);
            expect(shared.Size == 20'000_u64);
            expect(built.Size == 20'000_u64);
        }
        // Every vector this suite built has died by now, so the library should be holding nothing at all.
        if (const auto live = vec::LiveNodes()) expect(*live == 0_u64);
    };
}
