// Tests for the box:
//   cmake --build build --target BoxTest
//   ./build/tests/BoxTest
// No audit build pairs with this one -- `Tracked` and a sanitizer run are what catch a leak here.

#include "Box.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/box.hpp>
#pragma clang diagnostic pop

#include <string>
#include <utility>
#include <vector>

namespace {
// `Live` catches a cell that was never freed, `Compares` a comparison that reached `T` where the pointer
// should have settled it, and `Built` an update that replaced a cell it could have written through.
struct Tracked {
    static inline uint64_t Built = 0, Live = 0, Compares = 0;

    uint64_t V;

    Tracked(uint64_t v = 0) : V(v) {
        ++Built;
        ++Live;
    }
    // Declaring this suppresses the move constructor, so every way of building one is counted here.
    Tracked(const Tracked &o) : Tracked(o.V) {}
    // Assignment builds nothing, so the counts are right to leave alone.
    Tracked &operator=(const Tracked &) = default;
    ~Tracked() { --Live; }

    bool operator==(const Tracked &o) const {
        ++Compares;
        return V == o.V;
    }
};

// Neither copyable nor movable, a deleted copy constructor suppressing the move as well. A box holds one.
struct Immobile {
    uint64_t V;

    Immobile(uint64_t a, uint64_t b) : V(a + b) {}
    Immobile(const Immobile &) = delete;
};

// A box has to be usable as a member while its value type is still incomplete.
struct Later;
struct Early {
    box::Box<Later> Boxed;
};
struct Later {
    uint64_t V = 7;
};
} // namespace

using namespace boost::ut;

int main() {
    "construction"_test = [] {
        // There is no empty box: a default one holds `T{}`.
        const box::Box<uint64_t> zero{};
        expect(*zero == 0_u64);
        const box::Box<std::string> blank{};
        expect(blank->empty());

        // Implicit at one argument.
        const box::Box<std::string> text = "hello";
        expect(*text == std::string{"hello"});

        // More than one builds the value in place, which is what lets a box hold one that cannot be moved.
        const box::Box<Immobile> immobile{10, 32};
        expect(immobile->V == 42_u64);

        // Through `Early`, declared while `Later` was incomplete.
        const Early early{};
        expect(early.Boxed->V == 7_u64);

        // A conversion on the way out, so a box passes where the value is read.
        const auto length = [](const std::string &s) { return s.size(); };
        expect(length(text) == 5_ul);
    };

    "copies share the cell"_test = [] {
        const box::Box<Tracked> b{5};
        auto copy = b;
        expect((copy.Held == b.Held) >> fatal) << "a copy should be a counter bump and nothing else";
        expect(b.Held->Refs == 2_u) << "and both should be holding the cell";
        expect(Tracked::Live == 1_u64) << "and should not have built a second value";

        copy = box::Box<Tracked>{6};
        expect(b.Held->Refs == 1_u) << "the cell should have been released once";

        // A box built from an equal value gets a cell of its own -- nothing dedupes.
        const box::Box<Tracked> twin{5};
        expect(twin.Held != b.Held);
    };

    "moves"_test = [] {
        // `b` is left holding nothing and not read again -- only the destructor may look at a box moved from.
        box::Box<Tracked> b{9};
        auto *cell = b.Held;
        const auto moved = std::move(b);
        expect((moved.Held == cell) >> fatal) << "a move should hand the cell over, not copy it";
        expect(cell->Refs == 1_u) << "and should not have touched the count";

        // Assignment takes its argument by value, so both a copy and a move reach it.
        box::Box<Tracked> target{0};
        target = moved;
        expect(target.Held == cell);
        expect(cell->Refs == 2_u);
        target = box::Box<Tracked>{11};
        expect(target.Held != cell);
        expect(cell->Refs == 1_u) << "the cell target let go of should have been released";
        expect(target->V == 11_u64);
    };

    "equality"_test = [] {
        const box::Box<Tracked> b{5};
        auto same = b;
        const box::Box<Tracked> twin{5}, other{6};

        Tracked::Compares = 0;
        expect((b == same) >> fatal);
        expect(Tracked::Compares == 0_u64) << "two boxes over one cell should settle on the pointer";

        same = twin; // Equal, and now a cell of its own, so the pointer settles nothing.
        expect((b == same));
        expect(Tracked::Compares == 1_u64) << "separate cells have nothing but the value to go on";
        expect(!(b == other));
        expect((b != other)) << "which C++20 should have given us from `==`";

        // Against the bare value, in both orders, and against something that only converts to it.
        const box::Box<std::string> text = "hello";
        expect((text == std::string{"hello"}));
        expect((std::string{"hello"} == text));
        expect((text == "hello"));
        expect((text != "goodbye"));
    };

    "update"_test = [] {
        // Given a box to keep, an update leaves it alone and builds a cell for the result.
        const box::Box<uint64_t> before{21};
        const auto after = box::Update(before, [](uint64_t n) { return n * 2; });
        expect(*before == 21_u64) << "the update should have left the box it was given alone";
        expect(*after == 42_u64);
        expect(after.Held != before.Held);

        // Given a box to give up, it writes through the cell it alone holds. Counted in values built and
        // not in cell pointers: `new` hands a freed cell back, so a replaced cell lands on the same address.
        const auto bump = [](const Tracked &t) { return Tracked{t.V + 1}; };
        box::Box<Tracked> alone{0};
        Tracked::Built = 0;
        for (uint64_t i = 0; i < 10; ++i) alone = box::Update(std::move(alone), bump);
        expect(alone->V == 10_u64);
        expect(Tracked::Built == 10_u64) << "ten updates through a box nothing else holds should build ten values";

        // The same, with the cell shared, which has to leave the other holder as it was.
        const auto shared = alone;
        Tracked::Built = 0;
        auto written = box::Update(std::move(alone), bump);
        expect(written->V == 11_u64);
        expect(Tracked::Built == 2_u64) << "a shared cell cannot be written through, so the result needs one of its own";
        expect(written.Held != shared.Held);
        expect(shared->V == 10_u64);
        expect(shared.Held->Refs == 1_u) << "and the box that gave up should have let go of it";
    };

    "agrees with immer"_test = [] {
        // A thin oracle: a box has no structure to get wrong, and what matters here -- sharing, counts,
        // writing through a cell nothing else holds -- is about our internals. Both update forms.
        const auto step = [](uint64_t n) { return n * 3 + 1; };
        box::Box<uint64_t> kept{7}, given{7};
        immer::box<uint64_t> theirs{7};
        for (uint64_t i = 0; i < 20; ++i) {
            kept = box::Update(kept, step);
            given = box::Update(std::move(given), step);
            theirs = theirs.update(step);
        }
        expect(*kept == theirs.get());
        expect(*given == theirs.get());
    };

    "reclamation"_test = [] {
        // Every other test would pass just the same if a cell leaked or was freed one time too many.
        expect(Tracked::Live == 0_u64) << "the tests above should have let go of everything";
        {
            std::vector<box::Box<Tracked>> boxes;
            boxes.reserve(1'000);
            for (uint64_t i = 0; i < 1'000; ++i) boxes.emplace_back(i);
            expect(Tracked::Live == 1'000_u64);

            const auto copies = boxes; // Every cell held twice, so dropping one list frees nothing.
            boxes.clear();
            expect(Tracked::Live == 1'000_u64);

            auto updated = copies;
            for (auto &b : updated) b = box::Update(std::move(b), [](const Tracked &t) { return Tracked{t.V + 1}; });
            expect(Tracked::Live == 2'000_u64) << "the cells were shared, so each update built one";
        }
        expect(Tracked::Live == 0_u64);
    };
}
