// Tests for the relaxed radix balanced vector. The oracle is immer::flex_vector, driven through the
// same operations and required to agree element for element. Both builds are worth running:
//   cmake --build build --target FlexVectorTest FlexVectorAuditTest
//   ./build/tests/FlexVectorTest && ./build/tests/FlexVectorAuditTest
// FlexVectorAuditTest drops the node free list, so the reclamation check below can see anything at all.

#include "Vector.h"

#include "DiffCheck.h"

#include <boost/ut.hpp>

// immer is not included as a system header (see the top-level CMakeLists.txt), so silence its warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/flex_vector.hpp>
#pragma clang diagnostic pop

#include <algorithm>
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
        const auto left = vec::FlexVector{10}.transient();
        t.prepend(left);
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

    // Each case checks the answer and, in the audit build, the stage that produced it.
    // The stages differ in cost by orders of magnitude, so a case moving from the first search to the full one is a regression the answer alone would hide.
    "diff"_test = [] {
        using diffcheck::Diffed, diffcheck::Diffs, diffcheck::OneChange, diffcheck::Rebuilds, diffcheck::Marked, diffcheck::Settled, diffcheck::Asked;
        // `v` with the `n` elements at `from` taken out and put back in at `to`.
        const auto Moved = [](const vec::FlexVector &v, uint64_t from, uint64_t n, uint64_t to) { return vec::Insert(vec::Erase(v, from, from + n), to, vec::Take(vec::Drop(v, from), n)); };
        const vec::FlexVector empty;
        expect(Diffed(empty, empty).empty());

        // Stripping the common prefix and suffix is the whole answer for each of these.
        auto mark = Marked();
        for (const auto n : Sizes) {
            const auto v = Ragged(n);
            expect(Diffed(v, v).empty()) << "a vector against itself" << n;
            expect(Diffs(empty, v) >> fatal) << "everything against nothing" << n;
        }

        // The writes the relaxed shape adds, each of which moves one run and must be reported as one run.
        constexpr uint64_t N = 5'000;
        const auto v = Ragged(N);
        expect(OneChange(v, vec::Drop(v, 1'700), 0, 1'700, 0)) << "a cut from the front";
        for (const uint64_t at : {uint64_t{0}, uint64_t{1}, uint64_t{64}, uint64_t{2'500}, N})
            expect(OneChange(v, vec::Insert(v, at, 9'999), at, 0, 1)) << "one element put in at" << at;
        for (const uint64_t at : {uint64_t{0}, uint64_t{1}, uint64_t{64}, uint64_t{2'500}, N - 1})
            expect(OneChange(v, vec::Erase(v, at), at, 1, 0)) << "one element taken out at" << at;
        // A join reports only the elements added, whichever nodes it rebuilt where the two vectors meet.
        expect(OneChange(v, vec::Concat(v, Ragged(300, 11)), N, 0, 300)) << "a join on the end";
        expect(OneChange(v, vec::Erase(v, 1'000, 3'000), 1'000, 2'000, 0)) << "a stretch taken out";
        expect(Settled(mark, diffcheck::Stripped)) << "single runs, stages" << Asked(mark);

        // Writes spaced evenly, up to the most the first search finds, and then more.
        // Past that no chunk is unwritten, so there is no run to align on, and the full search resolves it.
        for (const uint64_t writes : {uint64_t{16}, uint64_t{64}, uint64_t{256}}) {
            mark = Marked();
            auto b = v;
            for (uint64_t i = 0; i < writes; ++i) b = vec::Set(std::move(b), i * (N / writes), ~i);
            const auto changes = Diffed(v, b);
            expect(Rebuilds(v, b) >> fatal) << writes << "writes spaced evenly";
            expect(changes.size() == writes) << writes << "writes spaced evenly";
            expect(diffcheck::Edits(changes) == writes) << writes << "writes spaced evenly";
            expect(Settled(mark, writes > 64 ? diffcheck::Probed | diffcheck::Searched : diffcheck::Probed)) << writes << "writes spaced evenly, stages" << Asked(mark);
        }

        // A moved block, which the relaxed vector makes cheap.
        // It differs by twice its own length, so past a few hundred elements the search fails and alignment on the shared run reports it.
        mark = Marked();
        for (const uint64_t block : {uint64_t{64}, uint64_t{512}, uint64_t{1'000}, uint64_t{2'000}}) {
            const auto moved = Moved(v, 1'000, block, 3'000);
            const auto changes = Diffed(v, moved);
            expect(Rebuilds(v, moved) >> fatal) << "a block of" << block << "moved";
            expect((changes.size() == 2u) >> fatal) << "a block of" << block << "moved";
            expect(diffcheck::Edits(changes) == 2 * block) << "a block of" << block << "moved";
        }

        // A block moved past one nearly its own size, so the two shared runs are within a factor of two of each other.
        // The shorter must be the one reported moved.
        // Over a densely chunked base, so only the chunks at the ends of the moved block lose their nodes.
        {
            const auto values = Counting(20'000);
            const vec::FlexVector dense = vec::Vector{values.begin(), values.end()};
            for (const uint64_t block : {uint64_t{600}, uint64_t{900}, uint64_t{1'500}}) {
                const auto moved = Moved(dense, 5'000, block, 6'000);
                expect(Rebuilds(dense, moved) >> fatal) << "a block of" << block << "moved past a thousand";
                expect(diffcheck::Edits(Diffed(dense, moved)) == 2 * std::min<uint64_t>(block, 1'000)) << "a block of" << block << "moved past a thousand";
            }
        }

        // The same over the ragged base, where a splice merges the short chunks around the cut and the insertion point, so the moved block keeps no node.
        for (const uint64_t block : {uint64_t{700}, uint64_t{900}, uint64_t{1'500}}) {
            const auto moved = Moved(v, 1'000, block, 2'000);
            expect(Rebuilds(v, moved) >> fatal) << "a block of" << block << "moved past a thousand, ragged";
            expect(diffcheck::Edits(Diffed(v, moved)) == 2 * std::min<uint64_t>(block, 1'000)) << "a block of" << block << "moved past a thousand, ragged";
        }
        // Every moved block is found by a shared run, by identity or by content, and none reaches the full search.
        expect(Settled(mark, diffcheck::Ran | diffcheck::Hashed)) << "moved blocks, stages" << Asked(mark);

        // Short vectors over a small alphabet, built from pieces so the two share no nodes.
        mark = Marked();
        std::mt19937_64 rng{9};
        for (uint64_t round = 0; round < 3'000; ++round) {
            vec::FlexVector left, right;
            for (uint64_t i = rng() % 13; i; --i) left = vec::PushBack(std::move(left), rng() % 4);
            for (uint64_t i = rng() % 13; i; --i) right = vec::Insert(right, rng() % (right.Size + 1), rng() % 4);
            expect(Diffs(left, right) >> fatal) << "round" << round;
            expect(diffcheck::Obeys(left, right) >> fatal) << "round" << round;
        }

        // Longer vectors over a small alphabet, with more splices than the first search finds.
        // The gaps between shared runs must be shortest scripts too.
        for (uint64_t round = 0; round < 200; ++round) {
            vec::FlexVector base;
            for (uint64_t i = 0; i < 150; ++i) base = vec::PushBack(std::move(base), rng() % 5);
            auto spliced = base;
            for (uint64_t i = 0; i < 40; ++i) {
                const auto at = rng() % spliced.Size;
                spliced = rng() % 2 ? vec::Insert(spliced, at, rng() % 5) : vec::Erase(spliced, at);
            }
            expect(Diffs(base, spliced) >> fatal) << "round" << round;
        }

        // Scattered splices, each of which shifts every index after it.
        for (uint64_t edits = 1; edits <= 8; ++edits) {
            auto b = v;
            for (uint64_t i = 0; i < edits; ++i) {
                const auto at = rng() % b.Size;
                b = rng() % 2 ? vec::Insert(b, at, rng()) : vec::Erase(b, at);
            }
            expect(Rebuilds(v, b) >> fatal) << "after" << edits << "splices";
            expect((Diffed(v, b).size() <= edits) >> fatal) << "after" << edits << "splices";
        }
        expect(Settled(mark, diffcheck::Probed)) << "small alphabets and scattered splices, stages" << Asked(mark);

        // Two unrelated vectors, more than `MaxEdits` edits apart, reported as one run.
        mark = Marked();
        vec::FlexVector other;
        for (uint64_t i = 0; i < N; ++i) other = vec::PushBack(std::move(other), rng());
        expect(Rebuilds(v, other)) << "unrelated vectors";
        expect(Settled(mark, diffcheck::Whole)) << "unrelated vectors, stages" << Asked(mark);
    };

    "apply"_test = [] {
        using diffcheck::Diffed, diffcheck::Obeys;
        const auto patch = [](const vec::FlexVector &a, const std::vector<vec::Change> &changes, const vec::FlexVector &source) {
            return vec::Apply(a, changes.data(), changes.data() + changes.size(), source);
        };
        const vec::FlexVector empty;
        expect(patch(empty, {}, empty).Size == 0_u64);

        // `Apply(a, Diff(a, b), b)` is `b`, over every write and every size in `Sizes`.
        constexpr uint64_t N = 3'000;
        const auto v = Ragged(N);
        expect(Obeys(v, v)) << "a vector against itself";
        expect(Obeys(empty, v) && Obeys(v, empty)) << "everything against nothing";
        for (const auto n : Sizes) expect(Obeys(v, Ragged(n, 21)) >> fatal) << "against a vector of" << n;
        expect(Obeys(v, vec::Insert(v, 1'500, 7)));
        expect(Obeys(v, vec::Erase(v, 200, 900)));
        expect(Obeys(v, vec::Drop(v, 1'100)));
        expect(Obeys(v, vec::Concat(v, Ragged(400, 13))));
        expect(Obeys(v, vec::Set(v, 2'000, 7)));

        // A change holds no reference into the diff, so it can be applied after the diff.
        const auto b = vec::Insert(vec::Set(v, 10, 5), 2'000, 9);
        const auto changes = Diffed(v, b);
        expect((changes.size() == 2u) >> fatal);
        expect(patch(v, changes, b) == b) << "applied after the diff";

        // A change holds indices and no elements, so the same changes can be applied to another vector.
        const auto other = Ragged(N, 31);
        const auto onto_other = patch(other, changes, b);
        expect(onto_other.Size == b.Size) << "the same changes on another vector";
        expect(vec::Check(onto_other) >> fatal);
        expect(onto_other[10] == b[10] && onto_other[2'000] == b[2'000]) << "the added elements";
        expect(onto_other[500] == other[500]) << "the rest unchanged";
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
