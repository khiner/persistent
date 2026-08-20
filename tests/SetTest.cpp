// Tests for the set, the same trie over bare keys. The oracle is immer::set, driven through the same
// operations and required to agree on every lookup. Both builds are worth running:
//   cmake --build build --target SetTest SetAuditTest
//   ./build/tests/SetTest && ./build/tests/SetAuditTest
// SetAuditTest drops the node free list, so the reclamation check below can see anything at all.
//
// The trie is compiled once per element type, so this repeats HamtTest's structural tests rather than
// leaning on them: a node here is half the width, and lands in its own size class.

#include "Hamt.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/set.hpp>
#pragma clang diagnostic pop

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

// Ahead of `using namespace boost::ut`, so that `==` here is the vector's and not ut's expression builder.
namespace {
using Keys = std::vector<uint64_t>;

// Iteration order is unspecified, so contents are compared as sorted key lists.
Keys Sorted(Keys v) {
    std::sort(v.begin(), v.end());
    return v;
}

// A loop and not the iterator-pair constructor: the trie's iterator carries no `iterator_traits`.
Keys Sorted(const hamt::Set &s) {
    Keys v;
    for (const auto key : s) v.push_back(key);
    return Sorted(std::move(v));
}

Keys Sorted(const immer::set<uint64_t> &s) {
    Keys v;
    for (const auto key : s) v.push_back(key);
    return Sorted(std::move(v));
}

template<typename T> bool Iterates(const hamt::Set &s, const T &expected) { return Sorted(s) == Sorted(expected); }

// The callback walk and the iterator are two routes over the same trie, so they have to agree on the
// order and not merely on the contents.
bool WalksInIterationOrder(const hamt::Set &s) {
    Keys walked, iterated;
    hamt::ForEach(s, [&](uint64_t key) { walked.push_back(key); });
    for (const auto key : s) iterated.push_back(key);
    return walked == iterated;
}

// A change as plain values, so two lists of them can be sorted and compared.
using Change = std::pair<uint64_t, bool>;

std::vector<Change> Diffed(const hamt::Set &a, const hamt::Set &b) {
    std::vector<Change> v;
    hamt::Diff(a, b, [&](uint64_t key, bool added) { v.emplace_back(key, added); });
    std::sort(v.begin(), v.end());
    return v;
}

// The same answer reached by looking every key up, which the trie walk has to agree with.
std::vector<Change> DiffedByHand(const hamt::Set &a, const hamt::Set &b) {
    std::vector<Change> v;
    for (const auto key : a)
        if (!hamt::Contains(b, key)) v.emplace_back(key, false);
    for (const auto key : b)
        if (!hamt::Contains(a, key)) v.emplace_back(key, true);
    std::sort(v.begin(), v.end());
    return v;
}

// Both directions, since a diff has to be as right about what was added as about what was removed.
bool Diffs(const hamt::Set &a, const hamt::Set &b) { return Diffed(a, b) == DiffedByHand(a, b) && Diffed(b, a) == DiffedByHand(b, a); }

// Kept out of `expect`, which would otherwise try to print a pair.
bool IsOnly(const std::vector<Change> &changes, const Change &only) { return changes.size() == 1 && changes[0] == only; }
} // namespace

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const hamt::Set s{};
        expect(s.Size == 0_u64);
        expect(!hamt::Contains(s, 0));
        expect(!hamt::Contains(s, 42));
    };

    "transient"_test = [] {
        const hamt::Set original{1, 2};
        auto t = original.transient();
        expect(t.size() == 2_u64 && !t.empty());
        expect(t.count(1) == 1_u64 && t.count(9) == 0_u64);
        expect(t.find(2) && *t.find(2) == 2_u64);
        t.insert(3);
        t.insert(3);
        t.erase(1);
        const auto snapshot = t.persistent();
        expect(hamt::Check(snapshot) >> fatal);
        expect(snapshot.Size == 2_u64 && hamt::Contains(snapshot, 2) && hamt::Contains(snapshot, 3));
        expect(hamt::Contains(original, 1) && !hamt::Contains(original, 3));

        t.insert(4);
        expect(!hamt::Contains(snapshot, 4));
        const auto finished = std::move(t).persistent();
        expect(hamt::Contains(finished, 4));
        uint64_t visited = 0;
        for (const auto key : finished.transient()) visited += key;
        expect(visited == 9_u64);
    };

    "insert and contains"_test = [] {
        auto s = hamt::Insert({}, 1);
        s = hamt::Insert(s, 2);
        s = hamt::Insert(s, 3);
        expect(s.Size == 3_u64);
        expect(hamt::Contains(s, 1));
        expect(hamt::Contains(s, 2));
        expect(hamt::Contains(s, 3));
        expect(!hamt::Contains(s, 4));
    };

    "reinserting keeps the trie"_test = [] {
        // What the set has over the map's write: a key already held is not rebound, so no node on the
        // path down to it is copied and the result is the trie that went in.
        hamt::Set s{};
        for (uint64_t i = 0; i < 500; ++i) s = hamt::Insert(std::move(s), i * 37);
        for (uint64_t i = 0; i < 500; ++i) {
            const auto again = hamt::Insert(s, i * 37); // Copied and not moved, so a rewrite would have to copy the path.
            expect((again.Root == s.Root) >> fatal) << "reinserting" << i * 37 << "copied a path";
            expect(again.Size == s.Size);
        }
    };

    "persistence"_test = [] {
        const auto before = hamt::Insert({}, 1);
        const auto after = hamt::Insert(before, 2);
        expect(before.Size == 1_u64);
        expect(!hamt::Contains(before, 2));
        expect(after.Size == 2_u64);
        expect(hamt::Contains(after, 1));

        const auto erased = hamt::Erase(after, 1);
        expect(erased.Size == 1_u64);
        expect(!hamt::Contains(erased, 1));
        expect(hamt::Contains(after, 1)); // The erase left `after` intact.
    };

    "erase misses"_test = [] {
        const auto s = hamt::Insert({}, 1);
        const auto erased = hamt::Erase(s, 2);
        expect(erased.Size == 1_u64);
        expect(hamt::Contains(erased, 1));
    };

    "built from keys"_test = [] {
        expect(hamt::Set{}.Size == 0_u64);
        const hamt::Set listed{1, 2, 3};
        expect(listed.Size == 3_u64);
        expect(hamt::Contains(listed, 2));
        expect(hamt::Check(listed) >> fatal);
        // A repeated key is held once, as a repeated `Insert` would leave it.
        const hamt::Set repeated{1, 1};
        expect(repeated.Size == 1_u64);
        expect(hamt::Contains(repeated, 1));

        // From a range, and then from another set, which is the shortest way to a twin that shares no
        // structure with what it copied.
        Keys keys;
        for (uint64_t i = 0; i < 1'000; ++i) keys.push_back(i * 7);
        const hamt::Set ranged{keys.begin(), keys.end()};
        expect(ranged.Size == 1'000_u64);
        expect(hamt::Check(ranged) >> fatal);
        expect(Iterates(ranged, keys));

        const hamt::Set twin{hamt::begin(ranged), hamt::end(ranged)};
        expect(hamt::Check(twin) >> fatal);
        expect(twin == ranged);
        expect(twin.Root != ranged.Root) << "the walk should have built a trie of its own";

        // Shapes that put the one-pass build through each of its cases: a lone key, keys sharing a slot
        // all the way down, and keys that collapse to one.
        const std::vector<std::pair<const char *, Keys>> shapes{
            {"one key", {5}},
            {"one key repeated", {5, 5, 5}},
            {"a full chain", {0x1000100000000000ull, 0x2000200000000000ull}},
            {"dense", Sorted([] { Keys v; for (uint64_t i = 0; i < 300; ++i) v.push_back(i); return v; }())},
        };
        for (const auto &[shape, in] : shapes) {
            const hamt::Set built{in.begin(), in.end()};
            expect(hamt::Check(built) >> fatal) << shape;
            hamt::Set folded{};
            for (const auto key : in) folded = hamt::Insert(std::move(folded), key);
            expect(built == folded) << shape << "should reach the trie inserting one at a time does";
        }
    };

    "iterate"_test = [] {
        const hamt::Set empty{};
        expect(hamt::begin(empty) == hamt::end(empty));
        uint64_t visits = 0;
        hamt::ForEach(empty, [&](uint64_t) { ++visits; });
        expect(visits == 0_u64) << "walking an empty set should call nothing";

        hamt::Set s{};
        Keys expected;
        for (uint64_t i = 0; i < 1'000; ++i) {
            s = hamt::Insert(std::move(s), i * i);
            expected.push_back(i * i);
        }
        expect(Iterates(s, expected));
        expect(WalksInIterationOrder(s));
        hamt::ForEach(s, [&](uint64_t) { ++visits; });
        expect(visits == s.Size) << "every key once and no more";
    };

    "equality"_test = [] {
        // Built in opposite orders, so equality has no shared history to fall back on.
        hamt::Set forward{}, backward{};
        for (uint64_t i = 0; i < 1'000; ++i) forward = hamt::Insert(std::move(forward), i * 7);
        for (uint64_t i = 1'000; i-- > 0;) backward = hamt::Insert(std::move(backward), i * 7);
        expect(forward == backward);
        expect(hamt::Set{} == hamt::Set{});
        expect(!(forward == hamt::Set{}));
        expect(!(forward == hamt::Insert(forward, 1))); // An extra key.
        expect(hamt::Erase(hamt::Insert(forward, 1), 1) == forward); // Round trip back to canonical.
    };

    "full depth"_test = [] {
        // Keys whose hashes differ only in the top four bits, driving the trie to its full depth --
        // a path random keys never take. The step is the hash's inverse of 1 << 60.
        constexpr uint64_t Step = 0x1000100000000000ull;
        constexpr uint64_t N = 8;
        hamt::Set s{};
        Keys expected;
        for (uint64_t i = 0; i < N; ++i) {
            s = hamt::Insert(std::move(s), i * Step);
            expected.push_back(i * Step);
            expect(hamt::Check(s) >> fatal) << "after inserting" << i;
        }
        expect(s.Size == N);
        expect(Iterates(s, expected));
        for (uint64_t i = 0; i < N; ++i) expect(hamt::Contains(s, i * Step));

        // Erasing back down unwinds the whole chain, one collapsed level at a time.
        for (uint64_t i = 0; i < N; ++i) {
            s = hamt::Erase(std::move(s), i * Step);
            expect(hamt::Check(s) >> fatal) << "after erasing" << i;
        }
        expect(s.Size == 0_u64);
    };

    "shared low bits"_test = [] {
        // Keys whose hashes agree over their low bits leave the root above the levels they agree on, as
        // aligned addresses do. A later key that disagrees puts those levels back, and erasing it takes
        // them away again.
        constexpr uint64_t Step = 0x1000100000000000ull; // Hashes varying in bits 12, 28, 44 and 60 only.
        constexpr uint64_t N = 8;
        hamt::Set base{};
        Keys expected;
        for (uint64_t i = 1; i <= N; ++i) {
            base = hamt::Insert(std::move(base), i * Step);
            expected.push_back(i * Step);
        }
        expect(hamt::Check(base) >> fatal);
        expect(base.Shift > 0u) << "twelve shared low bits should have moved the root up";
        expect(Iterates(base, expected));

        // A key below 2^32 hashes to itself, so each of these diverges at its lowest set bit: one at
        // the root's own level, one a level under it, and one far enough down to need a chain.
        for (const uint64_t key : {uint64_t{1} << 11, uint64_t{32}, uint64_t{1}}) {
            const auto grown = hamt::Insert(base, key);
            expect(hamt::Check(grown) >> fatal) << "after inserting" << key;
            expect(grown.Size == N + 1);
            expect(hamt::Contains(grown, key));
            for (uint64_t i = 1; i <= N; ++i) expect(hamt::Contains(grown, i * Step));
            expect(!hamt::Contains(grown, key + 1));

            const auto back = hamt::Erase(grown, key);
            expect(hamt::Check(back) >> fatal) << "after erasing" << key;
            expect(back == base) << "the round trip should land on the same trie";
        }
    };

    "immer parity"_test = [] {
        // A small key range, so repeats, misses and collisions all occur.
        std::mt19937_64 rng{42};
        std::uniform_int_distribution<uint64_t> key_dist{0, 1 << 12};
        hamt::Set ours{};
        immer::set<uint64_t> theirs;
        for (uint64_t i = 0; i < 100'000; ++i) {
            const auto key = key_dist(rng);
            if (rng() % 3 == 0) {
                ours = hamt::Erase(ours, key);
                theirs = theirs.erase(key);
            } else {
                ours = hamt::Insert(ours, key);
                theirs = theirs.insert(key);
            }
            // Fatal, so the first divergence ends the test instead of flooding the report.
            expect((hamt::Contains(ours, key) == bool(theirs.count(key))) >> fatal) << "key" << key << "after op" << i;
        }
        expect(ours.Size == uint64_t{theirs.size()});
        expect(hamt::Check(ours));
        expect(Iterates(ours, theirs));
    };

    "canonical form"_test = [] {
        // A key range small enough to reach the same contents by many routes, all of which canonicality
        // says must end at the same trie.
        std::mt19937_64 rng{11};
        std::uniform_int_distribution<uint64_t> key_dist{0, 64};
        hamt::Set ours{};
        for (uint64_t i = 0; i < 5'000; ++i) {
            const auto key = key_dist(rng);
            ours = rng() % 2 ? hamt::Insert(std::move(ours), key) : hamt::Erase(std::move(ours), key);
            expect(hamt::Check(ours) >> fatal) << "after op" << i;
        }
        auto miscounted = ours; // The check has to be able to fail, so hand it something it should reject.
        ++miscounted.Size;
        expect(!hamt::Check(miscounted));
    };

    "in-place reuse"_test = [] {
        // Moving into a write lets it reuse the old set's nodes rather than copy them. A snapshot taken
        // part way through has to survive every write that follows.
        std::mt19937_64 rng{7};
        std::uniform_int_distribution<uint64_t> key_dist{0, 1 << 10};
        hamt::Set ours{};
        immer::set<uint64_t> theirs;
        for (uint64_t i = 0; i < 2'000; ++i) {
            const auto key = key_dist(rng);
            ours = hamt::Insert(std::move(ours), key);
            theirs = theirs.insert(key);
        }
        const auto ours_snapshot = ours;
        const auto theirs_snapshot = theirs;

        for (uint64_t i = 2'000; i < 20'000; ++i) {
            const auto key = key_dist(rng);
            if (rng() % 3 == 0) {
                ours = hamt::Erase(std::move(ours), key);
                theirs = theirs.erase(key);
            } else {
                ours = hamt::Insert(std::move(ours), key);
                theirs = theirs.insert(key);
            }
        }
        expect(hamt::Check(ours));
        expect(Iterates(ours, theirs));
        expect(hamt::Check(ours_snapshot));
        expect(Iterates(ours_snapshot, theirs_snapshot));
    };

    "iterating pins the structure"_test = [] {
        // The walk keeps yielding the keys the set had when it began, even as that set is written to.
        constexpr uint64_t N = 200;
        hamt::Set s{};
        for (uint64_t i = 0; i < N; ++i) s = hamt::Insert(std::move(s), i * 3);

        uint64_t seen = 0;
        for (auto it = hamt::begin(s); it != hamt::end(s); ++it) {
            expect(*it % 3 == 0_u64) << "the walk saw a key it should not have";
            ++seen;
            s = hamt::Erase(std::move(s), *it); // Takes the key out from under the walk.
        }
        expect(seen == N);
        expect(s.Size == 0_u64);
    };

    "diff"_test = [] {
        const hamt::Set empty{};
        expect(Diffed(empty, empty).empty());

        hamt::Set s{};
        for (uint64_t i = 0; i < 500; ++i) s = hamt::Insert(std::move(s), i * 37);
        expect(Diffed(s, s).empty()) << "a set against itself";
        expect(Diffs(empty, s)) << "everything against nothing";

        // One key at a time, so both directions of change are pinned down on their own.
        expect(IsOnly(Diffed(s, hamt::Insert(s, 1)), {1, true})) << "a key added";
        expect(IsOnly(Diffed(s, hamt::Erase(s, 37)), {37, false})) << "a key removed";
        expect(Diffed(s, hamt::Insert(s, 37)).empty()) << "a key it already held is no change at all";

        // Sets that share history, and sets built separately from overlapping keys, which share none.
        std::mt19937_64 rng{5};
        std::uniform_int_distribution<uint64_t> key_dist{0, 2'000};
        for (uint64_t round = 0; round < 50; ++round) {
            auto b = s;
            for (uint64_t i = 0; i < round; ++i) {
                const auto key = key_dist(rng) * 37;
                b = rng() % 3 ? hamt::Insert(std::move(b), key) : hamt::Erase(std::move(b), key);
            }
            expect(Diffs(s, b) >> fatal) << "after" << round << "edits";
        }

        hamt::Set twin{};
        for (uint64_t i = 250; i < 750; ++i) twin = hamt::Insert(std::move(twin), i * 37);
        expect(Diffs(s, twin)) << "no shared structure, half the keys in common";

        // Roots at different levels, which only aligning them can get right: `deep`'s keys agree over
        // twelve low bits and hold its root above them, and the other two do not.
        constexpr uint64_t Step = 0x1000100000000000ull;
        hamt::Set deep{};
        for (uint64_t i = 1; i <= 8; ++i) deep = hamt::Insert(std::move(deep), i * Step);
        expect((deep.Shift > 0u) >> fatal);
        expect(Diffs(deep, hamt::Insert(deep, 1))) << "a key diverging below the root";
        expect(Diffs(deep, hamt::Insert(deep, 32))) << "a key diverging one level under the root";
        expect(Diffs(deep, s)) << "no key in common, and roots at different levels";
        expect(Diffs(deep, hamt::Erase(deep, Step))) << "still deep, one key fewer";
    };

    "reclamation"_test = [] {
        // Every other test would pass just the same if a reference count leaked or went one too far.
        constexpr uint64_t N = 5'000;
        {
            std::mt19937_64 rng{3};
            Keys keys;
            hamt::Set s{};
            for (uint64_t i = 0; i < N; ++i) {
                keys.push_back(rng());
                s = hamt::Insert(std::move(s), keys.back()); // Moved, so paths are reused in place.
            }
            const auto shared = s; // So every erase below has to copy its path instead.
            for (const auto key : keys) s = hamt::Erase(s, key);
            expect(s.Size == 0_u64);
            expect(shared.Size == N);
        }
        // Every set this suite built has died by now, so the library should be holding nothing at all.
        if (const auto live = hamt::LiveNodes()) expect(*live == 0_u64);
    };
}
