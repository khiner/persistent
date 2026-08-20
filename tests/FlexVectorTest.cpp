// Tests for the relaxed radix balanced vector. The oracle is immer::flex_vector, driven through the
// same operations and required to agree element for element. Both builds are worth running:
//   cmake --build build --target FlexVectorTest FlexVectorAuditTest
//   ./build/tests/FlexVectorTest && ./build/tests/FlexVectorAuditTest
// FlexVectorAuditTest drops the node free list, so the reclamation check below can see anything at all.

#include "Vector.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/flex_vector.hpp>
#pragma clang diagnostic pop

#include <random>
#include <vector>

// Ahead of `using namespace boost::ut`, so that `==` here is the vector's and not ut's expression builder.
namespace {
using Values = std::vector<uint64_t>;
using ImmerVector = immer::flex_vector<uint64_t>;

// Sizes either branching factor makes something of: a chunk, a root, and a level, either side of each.
constexpr uint64_t Sizes[]{0, 1, 2, 31, 32, 33, 63, 64, 65, 1023, 1024, 1025, 2000, 4095, 4096, 4097, 32'769};

Values Elements(const ImmerVector &v) { return {v.begin(), v.end()}; }

Values Iterated(const vec::FlexVector &v) {
    Values out;
    for (const auto x : v) out.push_back(x);
    return out;
}

Values Walked(const vec::FlexVector &v) {
    Values out;
    vec::ForEach(v, [&out](uint64_t x) { out.push_back(x); });
    return out;
}

Values Indexed(const vec::FlexVector &v) {
    Values out;
    for (uint64_t i = 0; i < v.Size; ++i) out.push_back(v[i]);
    return out;
}

// Every route over the vector has to agree, and all of them with immer. A vector that disagrees with
// itself is as broken as one that disagrees with the oracle.
bool Holds(const vec::FlexVector &ours, const Values &expected) {
    if (!vec::Check(ours) || ours.Size != expected.size()) return false;
    if (!expected.empty() && (vec::Front(ours) != expected.front() || vec::Back(ours) != expected.back())) return false;
    return Indexed(ours) == expected && Iterated(ours) == expected && Walked(ours) == expected;
}

bool Holds(const vec::FlexVector &ours, const ImmerVector &theirs) { return Holds(ours, Elements(theirs)); }

// Distinct values that are not their own index, so a walk landing on the wrong element shows up.
Values Counting(uint64_t n, uint64_t from = 0) {
    Values out(n);
    for (uint64_t i = 0; i < n; ++i) out[i] = (from + i) * 7 + 1;
    return out;
}

vec::FlexVector Pushed(const Values &values) {
    vec::FlexVector v;
    for (const auto x : values) v = vec::PushBack(std::move(v), x);
    return v;
}

ImmerVector ImmerPushed(const Values &values) {
    ImmerVector v;
    for (const auto x : values) v = std::move(v).push_back(x);
    return v;
}

// The shape that matters here: joined out of pieces of awkward length, so the trie is genuinely
// relaxed rather than the strict one a push builds. Both libraries are handed the same pieces, so the
// two hold the same elements while agreeing on nothing about their shapes.
std::vector<uint64_t> Pieces(uint64_t total, uint64_t seed) {
    std::mt19937_64 rng{seed};
    std::vector<uint64_t> out;
    for (uint64_t done = 0; done < total;) {
        const auto piece = 1 + rng() % 200;
        out.push_back(piece < total - done ? piece : total - done);
        done += out.back();
    }
    return out;
}

vec::FlexVector Ragged(uint64_t total, uint64_t seed = 3) {
    vec::FlexVector v;
    uint64_t at = 0;
    for (const auto piece : Pieces(total, seed)) {
        const auto values = Counting(piece, at);
        v = vec::Concat(v, vec::FlexVector{values.begin(), values.end()});
        at += piece;
    }
    return v;
}

ImmerVector ImmerRagged(uint64_t total, uint64_t seed = 3) {
    ImmerVector v;
    uint64_t at = 0;
    for (const auto piece : Pieces(total, seed)) {
        const auto values = Counting(piece, at);
        v = v + ImmerVector{values.begin(), values.end()};
        at += piece;
    }
    return v;
}
} // namespace

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const vec::FlexVector v;
        expect(v.Size == 0_u64);
        expect(vec::Check(v));
        expect(vec::Get(v, 0) == nullptr);
        expect(Iterated(v).empty());
        expect(Walked(v).empty());
        expect(v == vec::FlexVector{});
        expect(Holds(vec::Concat(v, v), Values{}));
    };

    "transient"_test = [] {
        const vec::FlexVector original{1, 2, 3, 4};
        auto t = original.transient();
        t.drop(1);
        t.take(2);
        t.push_back(40);
        t.set(0, 20);
        t.update(1, [](uint64_t value) { return value * 10; });
        const auto snapshot = t.persistent();
        expect((snapshot == vec::FlexVector{20, 30, 40}) >> fatal);
        expect(original == vec::FlexVector{1, 2, 3, 4});

        auto right = vec::FlexVector{50, 60}.transient();
        t.append(right);
        auto left = vec::FlexVector{10}.transient();
        t.prepend(std::move(left));
        expect(std::move(right).persistent() == vec::FlexVector{50, 60});
        const auto finished = std::move(t).persistent();
        expect(vec::Check(finished) >> fatal);
        expect(finished == vec::FlexVector{10, 20, 30, 40, 50, 60});
        expect(snapshot == vec::FlexVector{20, 30, 40});

        vec::FlexVectorTransient widened{vec::Vector{7, 8}.transient()};
        widened.push_back(9);
        expect(std::move(widened).persistent() == vec::FlexVector{7, 8, 9});
    };

    "from a strict vector"_test = [] {
        // The conversion is a retain and nothing else, so shape and elements both carry over.
        for (const auto n : Sizes) {
            const auto expected = Counting(n);
            const vec::Vector strict{expected.begin(), expected.end()};
            const vec::FlexVector v = strict;
            expect(Holds(v, expected) >> fatal) << "converted" << n;
            expect(v.Root == strict.Root && v.Tail == strict.Tail) << "shared rather than rebuilt" << n;
        }
    };

    "push back"_test = [] {
        for (const auto n : Sizes) {
            const auto expected = Counting(n);
            expect(Holds(Pushed(expected), ImmerPushed(expected)) >> fatal) << "pushed" << n;
            const vec::FlexVector built{expected.begin(), expected.end()};
            expect(Holds(built, expected) >> fatal) << "built" << n;
        }
    };

    "push back onto a relaxed vector"_test = [] {
        // The interesting half: appending where the rightmost path is no longer full, so the chunk
        // lands beside a partial node and the size tables have to follow it.
        auto ours = Ragged(3'000);
        auto theirs = ImmerRagged(3'000);
        for (uint64_t i = 0; i < 500; ++i) {
            ours = vec::PushBack(std::move(ours), 900'000 + i);
            theirs = std::move(theirs).push_back(900'000 + i);
        }
        expect(Holds(ours, theirs));
    };

    "push front"_test = [] {
        vec::FlexVector ours;
        ImmerVector theirs;
        for (uint64_t i = 0; i < 2'000; ++i) {
            ours = vec::PushFront(ours, i * 7 + 1);
            theirs = theirs.push_front(i * 7 + 1);
            if (i % 199 == 0) expect(Holds(ours, theirs) >> fatal) << "pushed front" << i;
        }
        expect(Holds(ours, theirs));
    };

    "concat"_test = [] {
        // Every pairing of the sizes, so two tries of different heights are joined both ways round.
        for (const auto a : Sizes) {
            for (const auto b : Sizes) {
                const auto left = Counting(a), right = Counting(b, a);
                auto expected = left;
                expected.insert(expected.end(), right.begin(), right.end());
                const vec::FlexVector ours = vec::Concat(Pushed(left), Pushed(right));
                expect(Holds(ours, expected) >> fatal) << "joined" << a << "and" << b;
            }
        }
    };

    "concat of two relaxed vectors"_test = [] {
        // Both sides already joined, so the run being repacked is made of nodes that are short to
        // begin with. That is the case the slack rule exists for, and a join of two strict vectors
        // never reaches it.
        for (const auto a : {uint64_t{1}, uint64_t{70}, uint64_t{900}, uint64_t{9'000}}) {
            for (const auto b : {uint64_t{1}, uint64_t{70}, uint64_t{900}, uint64_t{9'000}}) {
                const auto ours = vec::Concat(Ragged(a, a), Ragged(b, b + 1));
                const auto theirs = ImmerRagged(a, a) + ImmerRagged(b, b + 1);
                expect(Holds(ours, theirs) >> fatal) << "joined relaxed" << a << "and" << b;
            }
        }
    };

    "concat leaves both sides alone"_test = [] {
        const auto left = Counting(5'000), right = Counting(3'000, 5'000);
        const auto a = Pushed(left), b = Pushed(right);
        const auto joined = vec::Concat(a, b);
        expect(joined.Size == 8'000_u64);
        expect(Holds(a, left)) << "the left is untouched";
        expect(Holds(b, right)) << "the right is untouched";
    };

    "repeated concat"_test = [] {
        // Joining a joined vector again compounds the slack. Checked against immer at every
        // step, since a trie that drifts out of shape still reads correctly.
        for (const auto n : {uint64_t{500}, uint64_t{5'000}, uint64_t{40'000}}) {
            expect(Holds(Ragged(n), ImmerRagged(n)) >> fatal) << "ragged" << n;
        }
    };

    "index and set on a relaxed vector"_test = [] {
        constexpr uint64_t N = 20'000;
        auto ours = Ragged(N);
        auto theirs = ImmerRagged(N);
        expect(Holds(ours, theirs) >> fatal);
        std::mt19937_64 rng{11};
        for (uint64_t round = 0; round < 2'000; ++round) {
            const auto index = rng() % N, value = 1'000'000 + round;
            ours = vec::Set(std::move(ours), index, value);
            theirs = std::move(theirs).set(index, value);
        }
        expect(Holds(ours, theirs));
        // And without the move, which has to leave the vector it was handed standing on what it holds.
        const auto held = Iterated(ours);
        const auto once_more = vec::Update(ours, 7, [](uint64_t current) { return current + 100; });
        expect(Holds(ours, held)) << "the original is untouched";
        auto changed = held;
        changed[7] += 100;
        expect(Holds(once_more, changed));
    };

    "take and drop"_test = [] {
        // Every cut point of a relaxed vector, from both ends. A cut lands anywhere in a node that is
        // no longer full, so none of the arithmetic a strict cut relies on survives.
        constexpr uint64_t N = 2'100;
        const auto ours = Ragged(N);
        const auto theirs = ImmerRagged(N);
        for (uint64_t n = 0; n <= N; ++n) {
            expect(Holds(vec::Take(ours, n), theirs.take(n)) >> fatal) << "took" << n;
            expect(Holds(vec::Drop(ours, n), theirs.drop(n)) >> fatal) << "dropped" << n;
        }
        expect(Holds(vec::Take(ours, N + 100), Elements(theirs))) << "a cut past the end is the whole vector";
        expect(Holds(vec::Drop(ours, N + 100), Values{})) << "and from the other side, nothing";

        // Deep enough that a cut has more than one level to drop at once.
        constexpr uint64_t Deep = 70'000;
        const auto deep = Ragged(Deep, 5);
        const auto deep_theirs = ImmerRagged(Deep, 5);
        for (const uint64_t n : {uint64_t{1}, uint64_t{63}, uint64_t{64}, uint64_t{65}, uint64_t{4'096}, uint64_t{40'000}, uint64_t{69'999}}) {
            expect(Holds(vec::Take(deep, n), deep_theirs.take(n)) >> fatal) << "took" << n << "of 70,000";
            expect(Holds(vec::Drop(deep, n), deep_theirs.drop(n)) >> fatal) << "dropped" << n << "of 70,000";
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

    "slice"_test = [] {
        // Both cuts at once, the shape a middle-of-the-vector read takes.
        constexpr uint64_t N = 5'000;
        const auto ours = Ragged(N, 9);
        const auto theirs = ImmerRagged(N, 9);
        std::mt19937_64 rng{13};
        for (uint64_t round = 0; round < 200; ++round) {
            const auto from = rng() % N, to = from + rng() % (N - from + 1);
            expect(Holds(vec::Drop(vec::Take(ours, to), from), theirs.take(to).drop(from)) >> fatal) << "sliced" << from << to;
        }
    };

    "insert and erase"_test = [] {
        constexpr uint64_t N = 3'000;
        auto ours = Ragged(N, 21);
        auto theirs = ImmerRagged(N, 21);
        std::mt19937_64 rng{17};
        for (uint64_t round = 0; round < 300; ++round) {
            const auto pos = rng() % (ours.Size + 1);
            ours = vec::Insert(ours, pos, 500'000 + round);
            theirs = theirs.insert(pos, 500'000 + round);
        }
        expect(Holds(ours, theirs) >> fatal) << "inserted";
        for (uint64_t round = 0; round < 300; ++round) {
            const auto pos = rng() % ours.Size;
            ours = vec::Erase(ours, pos);
            theirs = theirs.erase(pos);
        }
        expect(Holds(ours, theirs) >> fatal) << "erased one at a time";
        for (uint64_t round = 0; round < 40 && ours.Size; ++round) {
            const auto first = rng() % ours.Size, last = first + rng() % (ours.Size - first + 1);
            ours = vec::Erase(ours, first, last);
            theirs = theirs.erase(first, last);
        }
        expect(Holds(ours, theirs)) << "erased in runs";
        const auto piece = Ragged(700, 33);
        const auto at = ours.Size / 3;
        auto spliced = Elements(theirs);
        const auto inserted = Iterated(piece);
        spliced.insert(spliced.begin() + int64_t(at), inserted.begin(), inserted.end());
        expect(Holds(vec::Insert(ours, at, piece), spliced)) << "a whole vector put in";
    };

    "equality"_test = [] {
        for (const auto n : Sizes) {
            const auto values = Counting(n);
            const auto a = Pushed(values);
            // Built three ways, so equality has to agree across shapes with nothing in common but
            // their elements.
            const vec::FlexVector b{values.begin(), values.end()};
            expect((a == b) >> fatal) << "twins of" << n;
            expect((a == vec::FlexVector{a}) >> fatal) << "itself" << n;
            if (!n) continue;
            const auto split = n / 3;
            const auto joined = vec::Concat(vec::Take(a, split), vec::Drop(a, split));
            expect((a == joined) >> fatal) << "cut and rejoined" << n;
            expect(!(a == vec::PushBack(a, 0))) << "one longer" << n;
            expect(!(a == vec::Set(a, n - 1, ~uint64_t{0}))) << "one element apart" << n;
            expect(!(a == vec::Take(a, n - 1))) << "one shorter" << n;
        }
    };

    "chunks"_test = [] {
        // The element walks are built on the chunked walk. With the one chunk length given up, they
        // need only be whole, non-empty and in order.
        for (const auto n : {uint64_t{0}, uint64_t{1}, uint64_t{64}, uint64_t{5'000}}) {
            const auto v = Ragged(n);
            uint64_t total = 0;
            bool shaped = true;
            vec::ForEachChunk(v, [&](const uint64_t *first, const uint64_t *last) {
                if (first == last) shaped = false;
                total += uint64_t(last - first);
            });
            expect(total == n) << "chunks of" << n;
            expect(shaped) << "chunks of" << n;
        }
    };

    "mixed writes against the oracle"_test = [] {
        // Every operation against immer over one long run, so a shape one of them leaves behind is
        // what the next one is handed. A snapshot taken part way through has to survive all of it.
        std::mt19937_64 rng{7};
        auto ours = Ragged(3'000, 41);
        auto theirs = ImmerRagged(3'000, 41);
        const auto ours_snapshot = ours;
        const auto theirs_snapshot = theirs;

        for (uint64_t i = 0; i < 4'000; ++i) {
            switch (rng() % 7) {
                case 0: {
                    const auto value = rng();
                    ours = vec::PushBack(std::move(ours), value);
                    theirs = std::move(theirs).push_back(value);
                    break;
                }
                case 1: {
                    const auto value = rng();
                    ours = vec::PushFront(ours, value);
                    theirs = theirs.push_front(value);
                    break;
                }
                case 2: {
                    const auto index = rng() % ours.Size, value = rng();
                    ours = vec::Set(std::move(ours), index, value);
                    theirs = std::move(theirs).set(index, value);
                    break;
                }
                case 3: {
                    const auto index = rng() % ours.Size;
                    ours = vec::Update(std::move(ours), index, [](uint64_t current) { return current ^ 0x5555; });
                    theirs = std::move(theirs).update(index, [](uint64_t current) { return current ^ 0x5555; });
                    break;
                }
                case 4: {
                    // Never all the way down, so the vector stays deep enough to be worth writing to.
                    const auto n = 1'000 + rng() % (ours.Size - 999);
                    ours = vec::Take(std::move(ours), n);
                    theirs = std::move(theirs).take(n);
                    break;
                }
                case 5: {
                    const auto n = rng() % (ours.Size - 999);
                    ours = vec::Drop(std::move(ours), n);
                    theirs = std::move(theirs).drop(n);
                    break;
                }
                default: {
                    const auto values = Counting(1 + rng() % 300, rng() % 1'000);
                    const vec::FlexVector piece{values.begin(), values.end()};
                    ours = vec::Concat(ours, piece);
                    theirs = theirs + ImmerVector{values.begin(), values.end()};
                    break;
                }
            }
            if (i % 500 == 0) expect(Holds(ours, theirs) >> fatal) << "round" << i;
        }
        expect(Holds(ours, theirs));
        expect(Holds(ours_snapshot, theirs_snapshot)) << "the snapshot survived every write after it";
    };

    "iterating pins the structure"_test = [] {
        // The walk keeps yielding the elements the vector had when it began, even as that vector is
        // written to underneath it.
        constexpr uint64_t N = 2'000;
        auto v = Ragged(N);
        const auto expected = Iterated(v);
        uint64_t seen = 0;
        for (auto it = vec::begin(v); it != vec::end(v); ++it) {
            expect(*it == expected[seen]) << "the walk saw a rewritten element";
            v = vec::Set(std::move(v), seen, 1'000'000 + seen);
            ++seen;
        }
        expect(seen == N);
        for (uint64_t i = 0; i < N; ++i) expect(v[i] == 1'000'000 + i);
    };

    "reclamation"_test = [] {
        // Every other test would pass just the same if a reference count leaked or went one too far,
        // and there are size tables to lose track of here as well as nodes.
        {
            auto v = Ragged(20'000);
            const auto shared = v; // A second handle, so every write below copies its path.
            for (uint64_t i = 0; i < 20'000; i += 3) v = vec::Set(v, i, i);
            v = vec::Concat(vec::Take(std::move(v), 7'000), Ragged(900));
            v = vec::Drop(std::move(v), 500);
            const auto values = Counting(5'000);
            const vec::FlexVector built = vec::Vector{values.begin(), values.end()};
            expect(v.Size == 7'400_u64);
            expect(shared.Size == 20'000_u64);
            expect(built.Size == 5'000_u64);
        }
        // Every vector this suite built has died by now, so the library should be holding nothing at all.
        if (const auto live = vec::LiveNodes()) expect(*live == 0_u64);
    };
}
