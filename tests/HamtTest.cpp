// Aspirational tests for the from-scratch HAMT. The oracle is immer::map (the reference implementation
// in /immer), driven through the same operation sequence and required to agree on every lookup.

#include "Hamt.h"

#include <boost/ut.hpp>

// Included with plain -I rather than -isystem (see tests/CMakeLists.txt), so silence immer's warnings here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/map.hpp>
#pragma clang diagnostic pop

#include <random>

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const hamt::Map m{};
        expect(m.Size == 0_u64);
        expect(!hamt::Get(m, 0).has_value());
        expect(!hamt::Get(m, 42).has_value());
    };

    "set and get"_test = [] {
        auto m = hamt::Set({}, 1, 100);
        m = hamt::Set(m, 2, 200);
        m = hamt::Set(m, 3, 300);
        expect(m.Size == 3_u64);
        expect(hamt::Get(m, 1) == std::optional<uint64_t>{100});
        expect(hamt::Get(m, 2) == std::optional<uint64_t>{200});
        expect(hamt::Get(m, 3) == std::optional<uint64_t>{300});
        expect(!hamt::Get(m, 4).has_value());
    };

    "overwrite"_test = [] {
        const auto m = hamt::Set(hamt::Set({}, 1, 100), 1, 101);
        expect(m.Size == 1_u64);
        expect(hamt::Get(m, 1) == std::optional<uint64_t>{101});
    };

    "persistence"_test = [] {
        const auto before = hamt::Set({}, 1, 100);
        const auto after = hamt::Set(before, 2, 200);
        expect(before.Size == 1_u64);
        expect(!hamt::Get(before, 2).has_value());
        expect(after.Size == 2_u64);
        expect(hamt::Get(after, 1) == std::optional<uint64_t>{100});

        const auto erased = hamt::Erase(after, 1);
        expect(erased.Size == 1_u64);
        expect(!hamt::Get(erased, 1).has_value());
        expect(hamt::Get(after, 1) == std::optional<uint64_t>{100}); // The erase left `after` intact.
    };

    "erase misses"_test = [] {
        const auto m = hamt::Set({}, 1, 100);
        const auto erased = hamt::Erase(m, 2);
        expect(erased.Size == 1_u64);
        expect(hamt::Get(erased, 1) == std::optional<uint64_t>{100});
    };

    "immer parity"_test = [] {
        // Random sets and erases over a small key range, so overwrites, misses, and collisions all occur.
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
            const auto actual = hamt::Get(ours, key);
            // Fatal, so the first divergence ends the test instead of flooding the report.
            expect((actual == (expected ? std::optional{*expected} : std::nullopt)) >> fatal) << "key" << key << "after op" << i;
        }
        expect(ours.Size == uint64_t{theirs.size()});
        for (const auto &[key, value] : theirs) expect((hamt::Get(ours, key) == std::optional{value}) >> fatal) << "key" << key;
    };
}
