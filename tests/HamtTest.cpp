// Tests for the from-scratch HAMT. The oracle is immer::map, driven through the same operations and
// required to agree on every lookup. Both builds are worth running:
//   cmake --build build --target HamtTest HamtAuditTest
//   ./build/tests/HamtTest && ./build/tests/HamtAuditTest
// HamtAuditTest drops the node free list, so the reclamation check below can see anything at all.

#include "Hamt.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/map.hpp>
#pragma clang diagnostic pop

#include <algorithm>
#include <optional>
#include <random>
#include <tuple>
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

// The callback walk and the iterator are two routes over the same trie, so they have to agree on the
// order and not merely on the contents.
bool WalksInIterationOrder(const hamt::Map &m) {
    Bindings walked, iterated;
    hamt::ForEach(m, [&](const hamt::Entry &e) { walked.emplace_back(e.Key, e.Value); });
    for (const auto &e : m) iterated.emplace_back(e.Key, e.Value);
    return walked == iterated;
}

bool Holds(const hamt::Map &m, uint64_t key, uint64_t value) {
    const auto *found = hamt::Get(m, key);
    return found && *found == value;
}

// A change as plain values, so two lists of them can be sorted and compared.
using Change = std::tuple<uint64_t, std::optional<uint64_t>, std::optional<uint64_t>>;

std::vector<Change> Diffed(const hamt::Map &a, const hamt::Map &b) {
    std::vector<Change> v;
    hamt::Diff(a, b, [&](const hamt::Change &c) {
        v.emplace_back(c.Key, c.Before ? std::optional{*c.Before} : std::nullopt, c.After ? std::optional{*c.After} : std::nullopt);
    });
    std::sort(v.begin(), v.end());
    return v;
}

// The same answer reached by looking every key up, which the trie walk has to agree with.
std::vector<Change> DiffedByHand(const hamt::Map &a, const hamt::Map &b) {
    std::vector<Change> v;
    for (const auto &e : a) {
        const auto *after = hamt::Get(b, e.Key);
        if (!after) v.emplace_back(e.Key, e.Value, std::nullopt);
        else if (*after != e.Value) v.emplace_back(e.Key, e.Value, *after);
    }
    for (const auto &e : b)
        if (!hamt::Get(a, e.Key)) v.emplace_back(e.Key, std::nullopt, e.Value);
    std::sort(v.begin(), v.end());
    return v;
}

// Both directions, since a diff has to be as right about what was added as about what was removed.
bool Diffs(const hamt::Map &a, const hamt::Map &b) { return Diffed(a, b) == DiffedByHand(a, b) && Diffed(b, a) == DiffedByHand(b, a); }

// Kept out of `expect`, which would otherwise try to print a tuple.
bool IsOnly(const std::vector<Change> &changes, const Change &only) { return changes.size() == 1 && changes[0] == only; }
} // namespace

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const hamt::Map m{};
        expect(m.Size == 0_u64);
        expect(hamt::Get(m, 0) == nullptr);
        expect(hamt::Get(m, 42) == nullptr);
    };

    "transient"_test = [] {
        const hamt::Map original{{1, 10}, {2, 20}};
        auto t = original.transient();
        expect(t.size() == 2_u64);
        expect(!t.empty());
        expect(t.count(1) == 1_u64 && t.count(9) == 0_u64);
        expect(t.find(2) && *t.find(2) == 20_u64);
        expect(t[9] == 0_u64);

        t.set(1, 11);
        t.insert({3, 30});
        t.update(2, [](uint64_t value) { return value + 2; });
        t.update(4, [](uint64_t value) { return value + 40; });
        t.update_if_exists(9, [](uint64_t value) { return value + 1; });
        t.erase(3);
        const auto snapshot = t.persistent();
        expect(hamt::Check(snapshot) >> fatal);
        expect(Holds(snapshot, 1, 11) && Holds(snapshot, 2, 22) && Holds(snapshot, 4, 40));
        expect(Holds(original, 1, 10) && original.Size == 2_u64);

        t.set(1, 100);
        expect(Holds(snapshot, 1, 11));
        const auto finished = std::move(t).persistent();
        expect(Holds(finished, 1, 100));
        uint64_t visited = 0;
        for (const auto &entry : original.transient()) visited += entry.Value;
        expect(visited == 30_u64);
    };

    "set and get"_test = [] {
        auto m = hamt::InsertOrAssign({}, 1, 100);
        m = hamt::InsertOrAssign(m, 2, 200);
        m = hamt::InsertOrAssign(m, 3, 300);
        expect(m.Size == 3_u64);
        expect(Holds(m, 1, 100));
        expect(Holds(m, 2, 200));
        expect(Holds(m, 3, 300));
        expect(hamt::Get(m, 4) == nullptr);
    };

    "overwrite"_test = [] {
        const auto m = hamt::InsertOrAssign(hamt::InsertOrAssign({}, 1, 100), 1, 101);
        expect(m.Size == 1_u64);
        expect(Holds(m, 1, 101));
    };

    "persistence"_test = [] {
        const auto before = hamt::InsertOrAssign({}, 1, 100);
        const auto after = hamt::InsertOrAssign(before, 2, 200);
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
        const auto m = hamt::InsertOrAssign({}, 1, 100);
        const auto erased = hamt::Erase(m, 2);
        expect(erased.Size == 1_u64);
        expect(Holds(erased, 1, 100));
    };

    "built from entries"_test = [] {
        expect(hamt::Map{}.Size == 0_u64);
        const hamt::Map listed{{1, 100}, {2, 200}, {3, 300}};
        expect(listed.Size == 3_u64);
        expect(Holds(listed, 2, 200));
        expect(hamt::Check(listed) >> fatal);
        // A repeated key lands where the last of them puts it, as a repeated `InsertOrAssign` would.
        const hamt::Map repeated{{1, 100}, {1, 101}};
        expect(repeated.Size == 1_u64);
        expect(Holds(repeated, 1, 101));

        // From a range, over anything with a `Key` and a `Value` -- including another map, which is
        // the shortest way to a twin that shares no structure with what it copied.
        std::vector<hamt::Entry> entries;
        Bindings expected;
        for (uint64_t i = 0; i < 1'000; ++i) {
            entries.emplace_back(i * 37, i);
            expected.emplace_back(i * 37, i);
        }
        const hamt::Map ranged{entries.begin(), entries.end()};
        expect(hamt::Check(ranged) >> fatal);
        expect(Iterates(ranged, expected));
        const hamt::Map twin{hamt::begin(ranged), hamt::end(ranged)};
        expect(twin == ranged);
        expect(twin.Root != ranged.Root) << "built again from scratch, so it shares no node";
        expect(hamt::Map{entries.begin(), entries.begin()}.Size == 0_u64) << "an empty range";

        // Building in one pass has to land on the same trie as inserting one at a time: canonicality
        // is nothing more than that, and nothing else makes the two interchangeable. Each shape below
        // reaches a different corner.
        constexpr uint64_t Step = 0x1000100000000000ull;
        std::mt19937_64 rng{13};
        const auto keyed = [](uint64_t n, auto key) {
            std::vector<hamt::Entry> v;
            v.reserve(n);
            for (uint64_t i = 0; i < n; ++i) v.emplace_back(key(i), i);
            return v;
        };
        const std::vector<std::pair<const char *, std::vector<hamt::Entry>>> shapes{
            {"random", keyed(2'000, [&](uint64_t) { return rng(); })},
            {"shared low bits", keyed(2'000, [](uint64_t i) { return i * 64; })},
            {"full depth", keyed(8, [](uint64_t i) { return i * Step; })},
            {"heavy repeats", keyed(500, [&](uint64_t) { return rng() % 64; })},
            {"one key only", {{7, 7}, {7, 8}, {7, 9}}},
        };
        for (const auto &[shape, in] : shapes) {
            hamt::Map folded{};
            for (const auto &e : in) folded = hamt::InsertOrAssign(std::move(folded), e.Key, e.Value);
            const hamt::Map built{in.begin(), in.end()};
            expect(hamt::Check(built) >> fatal) << shape;
            expect(built.Size == folded.Size) << shape;
            expect(built.Shift == folded.Shift && built.Prefix == folded.Prefix) << shape;
            expect(built == folded) << shape << "should be the same trie either way";
        }
    };

    "update"_test = [] {
        const auto inc = [](uint64_t v) { return v + 1; };
        // A key the map does not hold is updated from zero, matching immer's default-constructed
        // value. `UpdateIfExists` leaves the same key alone.
        const auto fresh = hamt::Update({}, 7, inc);
        expect(fresh.Size == 1_u64);
        expect(Holds(fresh, 7, 1));
        expect(Holds(hamt::Update(fresh, 7, inc), 7, 2));
        expect(hamt::UpdateIfExists(hamt::Map{}, 7, inc).Size == 0_u64);
        const auto missed = hamt::UpdateIfExists(fresh, 8, inc);
        expect(missed.Size == 1_u64);
        expect(hamt::Get(missed, 8) == nullptr);
        expect(missed.Root == fresh.Root) << "a miss should leave the trie where it was";
        expect(Holds(hamt::UpdateIfExists(fresh, 7, inc), 7, 2));

        // The callable sees what the key holds now, and is called once or not at all.
        uint64_t calls = 0;
        hamt::Update(fresh, 7, [&](uint64_t v) { ++calls; expect(v == 1_u64); return v; });
        hamt::UpdateIfExists(fresh, 8, [&](uint64_t v) { ++calls; return v; });
        expect(calls == 1_u64);

        // On a map deep enough that a write copies a path, which the input has to survive.
        hamt::Map m{};
        for (uint64_t i = 0; i < 1'000; ++i) m = hamt::InsertOrAssign(std::move(m), i * 37, i);
        for (const auto &bumped : {hamt::Update(m, 500 * 37, inc), hamt::UpdateIfExists(m, 500 * 37, inc)}) {
            expect(hamt::Check(bumped) >> fatal);
            expect(bumped.Size == m.Size);
            expect(Holds(bumped, 500 * 37, 501));
            expect(Holds(m, 500 * 37, 500)) << "the update left its input alone";
        }
        // A key it does not hold grows it, so the trie has to come out canonical either way.
        const auto grown = hamt::Update(m, 1, inc);
        expect(hamt::Check(grown) >> fatal);
        expect(grown.Size == m.Size + 1);
        expect(Holds(grown, 1, 1));
    };

    "iterate"_test = [] {
        const hamt::Map empty{};
        expect(hamt::begin(empty) == hamt::end(empty));
        uint64_t visits = 0;
        hamt::ForEach(empty, [&](const hamt::Entry &) { ++visits; });
        expect(visits == 0_u64) << "walking an empty map should call nothing";

        hamt::Map m{};
        Bindings expected;
        for (uint64_t i = 0; i < 1'000; ++i) {
            m = hamt::InsertOrAssign(std::move(m), i * i, i);
            expected.emplace_back(i * i, i);
        }
        expect(Iterates(m, expected));
        expect(WalksInIterationOrder(m));
        hamt::ForEach(m, [&](const hamt::Entry &) { ++visits; });
        expect(visits == m.Size) << "every binding once and no more";
    };

    "equality"_test = [] {
        // Built in opposite orders, so equality has no shared history to fall back on.
        hamt::Map forward{}, backward{};
        for (uint64_t i = 0; i < 1'000; ++i) forward = hamt::InsertOrAssign(std::move(forward), i * 7, i);
        for (uint64_t i = 1'000; i-- > 0;) backward = hamt::InsertOrAssign(std::move(backward), i * 7, i);
        expect(forward == backward);
        expect(hamt::Map{} == hamt::Map{});
        expect(!(forward == hamt::Map{}));
        expect(!(forward == hamt::InsertOrAssign(forward, 1, 1))); // An extra binding.
        expect(!(forward == hamt::InsertOrAssign(forward, 0, 1))); // Same keys, one value changed.
        expect(hamt::Erase(hamt::InsertOrAssign(forward, 1, 1), 1) == forward); // Round trip back to canonical.
    };

    "full depth"_test = [] {
        // Keys whose hashes differ only in the top four bits, driving the trie to its full depth --
        // a path random keys never take. The step is the hash's inverse of 1 << 60.
        constexpr uint64_t Step = 0x1000100000000000ull;
        constexpr uint64_t N = 8;
        hamt::Map m{};
        Bindings expected;
        for (uint64_t i = 0; i < N; ++i) {
            m = hamt::InsertOrAssign(std::move(m), i * Step, i);
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
        // Keys whose hashes agree over their low bits leave the root above the levels they agree on, as
        // aligned addresses do. A later key that disagrees puts those levels back, and erasing it takes
        // them away again.
        constexpr uint64_t Step = 0x1000100000000000ull; // Hashes varying in bits 12, 28, 44 and 60 only.
        constexpr uint64_t N = 8;
        hamt::Map base{};
        Bindings expected;
        for (uint64_t i = 1; i <= N; ++i) {
            base = hamt::InsertOrAssign(std::move(base), i * Step, i);
            expected.emplace_back(i * Step, i);
        }
        expect(hamt::Check(base) >> fatal);
        expect(base.Shift > 0u) << "twelve shared low bits should have moved the root up";
        expect(Iterates(base, expected));

        // A key below 2^32 hashes to itself, so each of these diverges at its lowest set bit: one at
        // the root's own level, one a level under it, and one far enough down to need a chain.
        for (const uint64_t key : {uint64_t{1} << 11, uint64_t{32}, uint64_t{1}}) {
            const auto grown = hamt::InsertOrAssign(base, key, key);
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
            // Both maps update a missing key from a zero of their own: ours by definition, immer's by
            // default construction. Driving them together holds the two definitions to each other.
            const auto bump = [i](uint64_t v) { return v + i; };
            switch (rng() % 4) {
                case 0:
                    ours = hamt::Erase(ours, key);
                    theirs = theirs.erase(key);
                    break;
                case 1:
                    ours = hamt::Update(ours, key, bump);
                    theirs = theirs.update(key, bump);
                    break;
                case 2:
                    ours = hamt::UpdateIfExists(ours, key, bump);
                    theirs = theirs.update_if_exists(key, bump);
                    break;
                default:
                    ours = hamt::InsertOrAssign(ours, key, i);
                    theirs = theirs.set(key, i);
                    break;
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
        // A key range small enough to reach the same contents by many routes, all of which canonicality
        // says must end at the same trie.
        std::mt19937_64 rng{11};
        std::uniform_int_distribution<uint64_t> key_dist{0, 64};
        hamt::Map ours{};
        for (uint64_t i = 0; i < 5'000; ++i) {
            const auto key = key_dist(rng);
            ours = rng() % 2 ? hamt::InsertOrAssign(std::move(ours), key, i) : hamt::Erase(std::move(ours), key);
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
            ours = hamt::InsertOrAssign(std::move(ours), key, i);
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
                ours = hamt::InsertOrAssign(std::move(ours), key, i);
                theirs = theirs.set(key, i);
            }
        }
        expect(hamt::Check(ours));
        expect(Iterates(ours, theirs));
        expect(hamt::Check(ours_snapshot));
        expect(Iterates(ours_snapshot, theirs_snapshot));
    };

    "iterating pins the structure"_test = [] {
        // The walk keeps yielding the bindings the map had when it began, even as that map is rebound.
        constexpr uint64_t N = 200;
        hamt::Map m{};
        for (uint64_t i = 0; i < N; ++i) m = hamt::InsertOrAssign(std::move(m), i * 3, i);

        uint64_t seen = 0;
        for (auto it = hamt::begin(m); it != hamt::end(m); ++it) {
            expect(it->Value == it->Key / 3) << "the walk saw a rebound entry"; // Not the value below.
            ++seen;
            m = hamt::InsertOrAssign(std::move(m), it->Key, it->Value + 1'000); // Rebinds a key under the walk.
        }
        expect(seen == N);
        for (uint64_t i = 0; i < N; ++i) expect(Holds(m, i * 3, i + 1'000));
    };

    "diff"_test = [] {
        const hamt::Map empty{};
        expect(Diffed(empty, empty).empty());

        hamt::Map m{};
        for (uint64_t i = 0; i < 500; ++i) m = hamt::InsertOrAssign(std::move(m), i * 37, i);
        expect(Diffed(m, m).empty()) << "a map against itself";
        expect(Diffs(empty, m)) << "everything against nothing";

        // One binding at a time, so each of the three kinds of change is pinned down on its own.
        expect(IsOnly(Diffed(m, hamt::InsertOrAssign(m, 1, 1)), {1, std::nullopt, 1})) << "a key added";
        expect(IsOnly(Diffed(m, hamt::InsertOrAssign(m, 0, 9)), {0, 0, 9})) << "a value rebound";
        expect(IsOnly(Diffed(m, hamt::Erase(m, 37)), {37, 1, std::nullopt})) << "a key removed";

        // Maps that share history, and maps built separately from overlapping keys, which share none.
        std::mt19937_64 rng{5};
        std::uniform_int_distribution<uint64_t> key_dist{0, 2'000};
        for (uint64_t round = 0; round < 50; ++round) {
            auto b = m;
            for (uint64_t i = 0; i < round; ++i) {
                const auto key = key_dist(rng) * 37;
                b = rng() % 3 ? hamt::InsertOrAssign(std::move(b), key, rng()) : hamt::Erase(std::move(b), key);
            }
            expect(Diffs(m, b) >> fatal) << "after" << round << "edits";
        }

        hamt::Map twin{};
        for (uint64_t i = 250; i < 750; ++i) twin = hamt::InsertOrAssign(std::move(twin), i * 37, i + 1);
        expect(Diffs(m, twin)) << "no shared structure, half the keys in common";

        // Roots at different levels, which only aligning them can get right: `deep`'s keys agree over
        // twelve low bits and hold its root above them, and the other two do not.
        constexpr uint64_t Step = 0x1000100000000000ull;
        hamt::Map deep{};
        for (uint64_t i = 1; i <= 8; ++i) deep = hamt::InsertOrAssign(std::move(deep), i * Step, i);
        expect((deep.Shift > 0u) >> fatal);
        expect(Diffs(deep, hamt::InsertOrAssign(deep, 1, 1))) << "a key diverging below the root";
        expect(Diffs(deep, hamt::InsertOrAssign(deep, 32, 32))) << "a key diverging one level under the root";
        expect(Diffs(deep, m)) << "no key in common, and roots at different levels";
        expect(Diffs(deep, hamt::Erase(deep, Step))) << "still deep, one key fewer";
    };

    "reclamation"_test = [] {
        // Every other test would pass just the same if a reference count leaked or went one too far.
        constexpr uint64_t N = 5'000;
        {
            std::mt19937_64 rng{3};
            std::vector<uint64_t> keys;
            hamt::Map m{};
            for (uint64_t i = 0; i < N; ++i) {
                keys.push_back(rng());
                m = hamt::InsertOrAssign(std::move(m), keys.back(), i); // Moved, so paths are reused in place.
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
