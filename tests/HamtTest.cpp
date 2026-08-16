// Tests for the from-scratch HAMT. The oracle is immer::map, driven through the same operation
// sequence and required to agree on every lookup. Both builds are worth running:
//   cmake --build build --target HamtTest HamtAuditTest
//   ./build/tests/HamtTest && ./build/tests/HamtAuditTest
// HamtAuditTest drops the node free list, which is what lets the reclamation check below see anything.

#include "Hamt.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/map.hpp>
#pragma clang diagnostic pop

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

// Ahead of `using namespace boost::ut`, so that `==` here is the vector's and not ut's expression builder.
namespace {
using Bindings = std::vector<std::pair<uint64_t, uint64_t>>;

// Iteration order is unspecified, so contents are compared as sorted key/value lists.
Bindings Sorted(Bindings v) {
    std::sort(v.begin(), v.end());
    return v;
}

Bindings Sorted(const hamt::Map &m) {
    Bindings v;
    for (const auto &e : m) v.emplace_back(e.Key, e.Value);
    return Sorted(std::move(v));
}

Bindings Sorted(const immer::map<uint64_t, uint64_t> &m) {
    Bindings v;
    for (const auto &[key, value] : m) v.emplace_back(key, value);
    return Sorted(std::move(v));
}

template<typename T> bool Iterates(const hamt::Map &m, const T &expected) { return Sorted(m) == Sorted(expected); }

bool Holds(const hamt::Map &m, uint64_t key, uint64_t value) {
    const auto *found = hamt::Get(m, key);
    return found && *found == value;
}
} // namespace

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const hamt::Map m{};
        expect(m.Size == 0_u64);
        expect(hamt::Get(m, 0) == nullptr);
        expect(hamt::Get(m, 42) == nullptr);
    };

    "set and get"_test = [] {
        auto m = hamt::Set({}, 1, 100);
        m = hamt::Set(m, 2, 200);
        m = hamt::Set(m, 3, 300);
        expect(m.Size == 3_u64);
        expect(Holds(m, 1, 100));
        expect(Holds(m, 2, 200));
        expect(Holds(m, 3, 300));
        expect(hamt::Get(m, 4) == nullptr);
    };

    "overwrite"_test = [] {
        const auto m = hamt::Set(hamt::Set({}, 1, 100), 1, 101);
        expect(m.Size == 1_u64);
        expect(Holds(m, 1, 101));
    };

    "persistence"_test = [] {
        const auto before = hamt::Set({}, 1, 100);
        const auto after = hamt::Set(before, 2, 200);
        expect(before.Size == 1_u64);
        expect(hamt::Get(before, 2) == nullptr);
        expect(after.Size == 2_u64);
        expect(Holds(after, 1, 100));

        const auto erased = hamt::Erase(after, 1);
        expect(erased.Size == 1_u64);
        expect(hamt::Get(erased, 1) == nullptr);
        expect(Holds(after, 1, 100)); // The erase left `after` intact.
    };

    "erase misses"_test = [] {
        const auto m = hamt::Set({}, 1, 100);
        const auto erased = hamt::Erase(m, 2);
        expect(erased.Size == 1_u64);
        expect(Holds(erased, 1, 100));
    };

    "iterate"_test = [] {
        const hamt::Map empty{};
        expect(hamt::begin(empty) == hamt::end(empty));

        hamt::Map m{};
        Bindings expected;
        for (uint64_t i = 0; i < 1'000; ++i) {
            m = hamt::Set(std::move(m), i * i, i);
            expected.emplace_back(i * i, i);
        }
        expect(Iterates(m, expected));
    };

    "equality"_test = [] {
        // Built in opposite orders, so equality has no shared history to fall back on.
        hamt::Map forward{}, backward{};
        for (uint64_t i = 0; i < 1'000; ++i) forward = hamt::Set(std::move(forward), i * 7, i);
        for (uint64_t i = 1'000; i-- > 0;) backward = hamt::Set(std::move(backward), i * 7, i);
        expect(forward == backward);
        expect(hamt::Map{} == hamt::Map{});
        expect(!(forward == hamt::Map{}));
        expect(!(forward == hamt::Set(forward, 1, 1))); // An extra binding.
        expect(!(forward == hamt::Set(forward, 0, 1))); // Same keys, one value changed.
        expect(hamt::Erase(hamt::Set(forward, 1, 1), 1) == forward); // Round trip back to canonical.
    };

    "full depth"_test = [] {
        // Keys whose hashes differ only in the top four bits, driving the trie to its full depth --
        // a path random keys never take. The step is what the hash's inverse maps 1 << 60 to.
        constexpr uint64_t Step = 0x1000100000000000ull;
        constexpr uint64_t N = 8;
        hamt::Map m{};
        Bindings expected;
        for (uint64_t i = 0; i < N; ++i) {
            m = hamt::Set(std::move(m), i * Step, i);
            expected.emplace_back(i * Step, i);
            expect(hamt::Check(m) >> fatal) << "after inserting" << i;
        }
        expect(m.Size == N);
        expect(Iterates(m, expected));
        for (uint64_t i = 0; i < N; ++i) expect(Holds(m, i * Step, i));

        // Erasing back down unwinds the whole chain, one collapsed level at a time.
        for (uint64_t i = 0; i < N; ++i) {
            m = hamt::Erase(std::move(m), i * Step);
            expect(hamt::Check(m) >> fatal) << "after erasing" << i;
        }
        expect(m.Size == 0_u64);
    };

    "shared low bits"_test = [] {
        // Keys whose hashes agree over their low bits leave the root standing above the levels they
        // agree on, as any set of aligned addresses does. A later key that disagrees down there has to
        // put those levels back, and erasing it has to take them away again.
        constexpr uint64_t Step = 0x1000100000000000ull; // Hashes varying in bits 12, 28, 44 and 60 only.
        constexpr uint64_t N = 8;
        hamt::Map base{};
        Bindings expected;
        for (uint64_t i = 1; i <= N; ++i) {
            base = hamt::Set(std::move(base), i * Step, i);
            expected.emplace_back(i * Step, i);
        }
        expect(hamt::Check(base) >> fatal);
        expect(base.Shift > 0u) << "twelve shared low bits should have moved the root up";
        expect(Iterates(base, expected));

        // A key below 2^32 hashes to itself, so each of these diverges at its lowest set bit: one at
        // the root's own level, one a level under it, and one far enough down to need a chain.
        for (const uint64_t key : {uint64_t{1} << 11, uint64_t{32}, uint64_t{1}}) {
            const auto grown = hamt::Set(base, key, key);
            expect(hamt::Check(grown) >> fatal) << "after inserting" << key;
            expect(grown.Size == N + 1);
            expect(Holds(grown, key, key));
            for (uint64_t i = 1; i <= N; ++i) expect(Holds(grown, i * Step, i));
            expect(hamt::Get(grown, key + 1) == nullptr);

            const auto back = hamt::Erase(grown, key);
            expect(hamt::Check(back) >> fatal) << "after erasing" << key;
            expect(back == base) << "the round trip should land on the same trie";
        }
    };

    "immer parity"_test = [] {
        // A small key range, so overwrites, misses, and collisions all occur.
        std::mt19937_64 rng{42};
        std::uniform_int_distribution<uint64_t> key_dist{0, 1 << 12};
        hamt::Map ours{};
        immer::map<uint64_t, uint64_t> theirs;
        for (uint64_t i = 0; i < 100'000; ++i) {
            const auto key = key_dist(rng);
            if (rng() % 4 == 0) {
                ours = hamt::Erase(ours, key);
                theirs = theirs.erase(key);
            } else {
                ours = hamt::Set(ours, key, i);
                theirs = theirs.set(key, i);
            }
            const auto *expected = theirs.find(key);
            const auto *actual = hamt::Get(ours, key);
            // Fatal, so the first divergence ends the test instead of flooding the report.
            expect((expected ? actual && *actual == *expected : actual == nullptr) >> fatal) << "key" << key << "after op" << i;
        }
        expect(ours.Size == uint64_t{theirs.size()});
        expect(hamt::Check(ours));
        expect(Iterates(ours, theirs));
    };

    "canonical form"_test = [] {
        // A key range small enough to reach the same contents by many routes, all of which
        // canonicality says must end at the same trie.
        std::mt19937_64 rng{11};
        std::uniform_int_distribution<uint64_t> key_dist{0, 64};
        hamt::Map ours{};
        for (uint64_t i = 0; i < 5'000; ++i) {
            const auto key = key_dist(rng);
            ours = rng() % 2 ? hamt::Set(std::move(ours), key, i) : hamt::Erase(std::move(ours), key);
            expect(hamt::Check(ours) >> fatal) << "after op" << i;
        }
        auto miscounted = ours; // The check has to be able to fail, so hand it something it should reject.
        ++miscounted.Size;
        expect(!hamt::Check(miscounted));
    };

    "in-place reuse"_test = [] {
        // Moving into an update lets it reuse the old map's nodes rather than copy them. A snapshot
        // taken part way through has to survive every update that follows.
        std::mt19937_64 rng{7};
        std::uniform_int_distribution<uint64_t> key_dist{0, 1 << 10};
        hamt::Map ours{};
        immer::map<uint64_t, uint64_t> theirs;
        for (uint64_t i = 0; i < 2'000; ++i) {
            const auto key = key_dist(rng);
            ours = hamt::Set(std::move(ours), key, i);
            theirs = theirs.set(key, i);
        }
        const auto ours_snapshot = ours;
        const auto theirs_snapshot = theirs;

        for (uint64_t i = 2'000; i < 20'000; ++i) {
            const auto key = key_dist(rng);
            if (rng() % 3 == 0) {
                ours = hamt::Erase(std::move(ours), key);
                theirs = theirs.erase(key);
            } else {
                ours = hamt::Set(std::move(ours), key, i);
                theirs = theirs.set(key, i);
            }
        }
        expect(hamt::Check(ours));
        expect(Iterates(ours, theirs));
        expect(hamt::Check(ours_snapshot));
        expect(Iterates(ours_snapshot, theirs_snapshot));
    };

    "iterating pins the structure"_test = [] {
        // The walk has to keep yielding the bindings the map had when it began, even as that same
        // map is rebound underneath it.
        constexpr uint64_t N = 200;
        hamt::Map m{};
        for (uint64_t i = 0; i < N; ++i) m = hamt::Set(std::move(m), i * 3, i);

        uint64_t seen = 0;
        for (auto it = hamt::begin(m); it != hamt::end(m); ++it) {
            expect(it->Value == it->Key / 3) << "the walk saw a rebound entry"; // Not the value below.
            ++seen;
            m = hamt::Set(std::move(m), it->Key, it->Value + 1'000); // Rebinds a key under the walk.
        }
        expect(seen == N);
        for (uint64_t i = 0; i < N; ++i) expect(Holds(m, i * 3, i + 1'000));
    };

    "reclamation"_test = [] {
        // Every other test here would pass just the same if a reference count leaked or went one too
        // far. Only the audit build can see it.
        constexpr uint64_t N = 5'000;
        {
            std::mt19937_64 rng{3};
            std::vector<uint64_t> keys;
            hamt::Map m{};
            for (uint64_t i = 0; i < N; ++i) {
                keys.push_back(rng());
                m = hamt::Set(std::move(m), keys.back(), i); // Moved, so paths are reused in place.
            }
            const auto shared = m; // So every erase below has to copy its path instead.
            for (const auto key : keys) m = hamt::Erase(m, key);
            expect(m.Size == 0_u64);
            expect(shared.Size == N);
        }
        // Every map this suite built has died by now, so the library should be holding nothing at all.
        if (const auto live = hamt::LiveNodes()) expect(*live == 0_u64);
    };
}
