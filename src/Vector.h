#pragma once

#include "Erased.h"

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <utility>

namespace vec {
struct Node;

// Persistent vector of 64-bit values, backed by a radix balanced trie. Every operation returns a new
// vector and leaves its input intact. Two vectors share the nodes they have in common, and a node is
// freed when the last vector holding it is. Single threaded, and not per vector: two threads sharing no
// structure still race.
struct Vector {
    // The trie, and the last partial chunk held outside it. An append writes the tail and touches the
    // trie only when it fills.
    const Node *Root{}, *Tail{};
    uint64_t Size{};
    // How many index bits the root stands above. Meaningless when there is no trie.
    uint32_t Shift{};

    Vector() = default;
    // Adopts an existing reference to each of `root` and `tail`.
    Vector(const Node *root, const Node *tail, uint64_t size, uint32_t shift) : Root(root), Tail(tail), Size(size), Shift(shift) {}
    Vector(const Vector &);
    Vector(Vector &&) noexcept;
    ~Vector();
    Vector &operator=(const Vector &);
    Vector &operator=(Vector &&) noexcept;

    // The vector holding every one of `values`, or all of `[first, last)`, in order.
    Vector(std::initializer_list<uint64_t> values);
    template<typename It> Vector(It first, It last);

    // The element at `index`, which has to be one the vector holds. The reference lasts as long as this
    // vector holds that element.
    const uint64_t &operator[](uint64_t index) const;
};

// Every write comes two ways, as immer's do. Given a vector to keep, it shares what it can and copies
// the rest. Given one to give up -- `PushBack(std::move(v), x)` -- it writes through the nodes it alone
// holds and returns the same vector. Use the second form in a loop: a push into a tail it alone holds
// is one store.
// The vector with `value` on the end.
Vector PushBack(const Vector &v, uint64_t value);
Vector &&PushBack(Vector &&v, uint64_t value);
// The vector with `index` bound to `value`, which has to be an index the vector already holds.
Vector Set(const Vector &v, uint64_t index, uint64_t value);
Vector &&Set(Vector &&v, uint64_t index, uint64_t value);
// The first `count` elements, or the whole vector when it holds no more than that.
Vector Take(const Vector &v, uint64_t count);
Vector &&Take(Vector &&v, uint64_t count);

// The vector holding `[first, last)`, assembled in one pass rather than one element at a time. Reached
// through the constructors, when they are handed a contiguous range.
Vector Build(const uint64_t *first, const uint64_t *last);

// Which ranges go straight to `Build`. A concept and not two conditions, so that they are checked in
// order: `iter_value_t` is an error, not a false, for an iterator lacking one -- as this vector's is.
template<typename It>
concept ValueRange = std::contiguous_iterator<It> && std::same_as<std::iter_value_t<It>, uint64_t>;

// Out of the struct, since a body written inside the class cannot see `PushBack`, declared after it.
template<typename It> Vector::Vector(It first, It last) {
    if constexpr (ValueRange<It>) {
        const uint64_t *begin = first == last ? nullptr : &*first;
        *this = Build(begin, begin + (last - first));
    } else {
        for (; first != last; ++first) *this = PushBack(std::move(*this), *first);
    }
}
inline Vector::Vector(std::initializer_list<uint64_t> values) : Vector(values.begin(), values.end()) {}

// The vector with `index` bound to `fn` of the element it holds there, as immer's `update` does. One
// descent rather than the two an index and a `Set` take.
Vector Update(const Vector &v, uint64_t index, uint64_t (*fn)(void *context, uint64_t current), void *context);
Vector &&Update(Vector &&v, uint64_t index, uint64_t (*fn)(void *context, uint64_t current), void *context);
// The same, for anything callable with a `uint64_t`.
template<typename F> Vector Update(const Vector &v, uint64_t index, F &&fn) { return Update(v, index, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }
template<typename F> Vector &&Update(Vector &&v, uint64_t index, F &&fn) { return Update(std::move(v), index, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }

// The element at `index`, or null when the vector is not that long.
const uint64_t *Get(const Vector &v, uint64_t index);
inline const uint64_t &Front(const Vector &v) { return v[0]; }
inline const uint64_t &Back(const Vector &v) { return v[v.Size - 1]; }

// Whether the two vectors hold the same elements in the same order. Cheap between vectors that share
// history, down to constant time for two that never diverged.
bool operator==(const Vector &a, const Vector &b);

// Whether `v` is canonical and its shape agrees with its size. A standing check on the representation
// invariant, meant for tests. Linear in the size of the vector.
bool Check(const Vector &v);

// How many nodes the library holds, so a test can confirm vectors release what they take. Kept only by an
// audit build (-DVECTOR_AUDIT). Empty elsewhere, meaning not counted rather than zero.
std::optional<uint64_t> LiveNodes();

// The bytes of node vectors still point at, against the bytes of slab those nodes are spread over: a walk
// reads the first and covers the second, so the gap is what a read pays for the holes a build leaves.
// `SpannedBytes` counts only slabs still holding something, `ReservedBytes` all ever taken. Empty in an
// audit build, which has no slabs.
struct Footprint {
    uint64_t LiveBytes, SpannedBytes, ReservedBytes;
};
std::optional<Footprint> Held();

// Every element once, in order, a chunk at a time. A chunk and not an element because `visit` is a
// pointer: calling it once per chunk leaves the loop over the chunk in the caller, where it inlines.
// The vector must outlive the call, and nothing may write to it from inside `visit`.
void ForEachChunk(const Vector &v, void (*visit)(void *context, const uint64_t *first, const uint64_t *last), void *context);
// The same, for anything callable with a pair of `const uint64_t *`.
template<typename F> void ForEachChunk(const Vector &v, F &&visit) { ForEachChunk(v, erased::Call<void, F, const uint64_t *, const uint64_t *>, erased::Context(visit)); }
// One element at a time. It costs nothing over the chunked form, since `visit` inlines into that loop.
template<typename F> void ForEach(const Vector &v, F &&visit) {
    ForEachChunk(v, [&visit](const uint64_t *first, const uint64_t *last) {
        for (; first != last; ++first) visit(*first);
    });
}

// Walk yielding every element once, in order. It holds the vector it walks, so it keeps yielding the
// elements that vector had when the walk began -- what it buys over `ForEach`, along with stopping early.
struct Iterator {
    // A level consumes at least five index bits, so a path is at most this long.
    static constexpr uint32_t MaxDepth = 13;
    // The children of one node still to be walked.
    struct Frame {
        const Node *const *Child, *const *ChildEnd;
    };

    // `Cur` is null exactly at the end, which is what `end()` compares equal to. The stack is left
    // uninitialized -- braced init would zero all 13 frames, and only those below `Depth` are read.
    Vector Source;
    const uint64_t *Cur{}, *End{};
    const Node *Rest{}; // The tail, until the walk reaches it.
    Frame Stack[MaxDepth];
    uint32_t Depth{};

    Iterator() = default;
    explicit Iterator(const Vector &v) : Source(v) {}

    const uint64_t &operator*() const { return *Cur; }
    const uint64_t *operator->() const { return Cur; }
    bool operator==(const Iterator &o) const { return Cur == o.Cur; }
    Iterator &operator++() {
        if (++Cur == End) Advance();
        return *this;
    }

    // Moves on to the next chunk, or reports the end by clearing `Cur`.
    void Advance();
};

Iterator begin(const Vector &v);
inline Iterator end(const Vector &) { return {}; }
} // namespace vec
