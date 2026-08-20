#pragma once

#include "Erased.h"

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vec {
struct Node;

// The rule a trie's shape is held to, and the one thing the two vectors differ in.
enum struct Shape {
    // Every node is full but the rightmost path, so the trie is a function of the size alone and an
    // index is a path: no node is searched.
    Strict,
    // That rule given up. It buys concatenation, a cut from the front, and insertion and erasure in the
    // middle, all logarithmic where the strict vector would have to rebuild. It costs the index: a
    // partly filled node carries a table of running totals, and has to be searched rather than addressed.
    Relaxed,
};

template<Shape S> struct Transient;

// Persistent vector of 64-bit values, backed by a radix balanced trie with the last partial chunk held
// outside it. Every operation returns a new vector and leaves its input intact. Two vectors share the
// nodes they have in common, and a node is freed when the last vector holding it is. Single threaded,
// and not per vector: two threads sharing no structure still race.
//
// One struct for both shapes rather than two standing on a common one: the fields, their lifetime, and
// every walk that reads no more than `Count` are the same either way. What differs is how a slot is
// found and what an append has to work out, and `Shape` settles that at each of those, rather than at a
// type boundary that would have the rest written twice.
template<Shape S> struct Trie {
    const Node *Root{}, *Tail{};
    uint64_t Size{};
    // How many index bits the root stands above. Meaningless when there is no trie.
    uint32_t Shift{};

    Trie() = default;
    // Adopts an existing reference to each of `root` and `tail`.
    Trie(const Node *root, const Node *tail, uint64_t size, uint32_t shift) : Root(root), Tail(tail), Size(size), Shift(shift) {}
    Trie(const Trie &);
    Trie(Trie &&) noexcept;
    ~Trie();
    Trie &operator=(const Trie &);
    Trie &operator=(Trie &&) noexcept;

    // A strict trie is already a relaxed one, so the widening is a reference and nothing else. It goes
    // this way only: the relaxed shape cannot be narrowed without rebuilding.
    Trie(const Trie<Shape::Strict> &v)
        requires(S == Shape::Relaxed);

    // The vector holding every one of `values`, or all of `[first, last)`, in order.
    Trie(std::initializer_list<uint64_t> values);
    template<typename It> Trie(It first, const It &last);

    // The element at `index`, which has to be one the vector holds. The reference lasts as long as this
    // vector holds that element.
    const uint64_t &operator[](uint64_t index) const;

    using transient_type = Transient<S>;
    // A mutable view over the same copy-on-write vector. The lvalue form preserves this vector and the
    // rvalue form transfers it, matching immer's two `transient()` affordances.
    transient_type transient() const &;
    transient_type transient() &&;
};

using Vector = Trie<Shape::Strict>;
using FlexVector = Trie<Shape::Relaxed>;

// Every write comes two ways, as immer's do. Given a vector to keep, it shares what it can and copies
// the rest. Given one to give up -- `PushBack(std::move(v), x)` -- it writes through the nodes it alone
// holds and returns the same vector. Use the second form in a loop: a push into a tail it alone holds
// is one store.
// The vector with `value` on the end.
template<Shape S> Trie<S> PushBack(const Trie<S> &v, uint64_t value);
template<Shape S> Trie<S> &&PushBack(Trie<S> &&v, uint64_t value);
// The vector with `index` bound to `value`, which has to be an index the vector already holds.
template<Shape S> Trie<S> Set(const Trie<S> &v, uint64_t index, uint64_t value);
template<Shape S> Trie<S> &&Set(Trie<S> &&v, uint64_t index, uint64_t value);
// The first `count` elements, or the whole vector when it holds no more than that.
template<Shape S> Trie<S> Take(const Trie<S> &v, uint64_t count);
template<Shape S> Trie<S> &&Take(Trie<S> &&v, uint64_t count);

// The vector with `index` bound to `fn` of the element it holds there, as immer's `update` does. One
// descent rather than the two an index and a `Set` take.
template<Shape S> Trie<S> Update(const Trie<S> &v, uint64_t index, uint64_t (*fn)(void *context, uint64_t current), void *context);
template<Shape S> Trie<S> &&Update(Trie<S> &&v, uint64_t index, uint64_t (*fn)(void *context, uint64_t current), void *context);
// The same, for anything callable with a `uint64_t`.
template<Shape S, typename F> Trie<S> Update(const Trie<S> &v, uint64_t index, F &&fn) { return Update(v, index, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }
template<Shape S, typename F> Trie<S> &&Update(Trie<S> &&v, uint64_t index, F &&fn) { return Update(std::move(v), index, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }

// Everything past the first `count` elements, or nothing when the vector is no longer than that.
FlexVector Drop(const FlexVector &v, uint64_t count);
FlexVector &&Drop(FlexVector &&v, uint64_t count);
// The elements of `a` followed by those of `b`. Logarithmic in the longer of the two, and it shares with
// both: only the spine where they meet is built, and every subtree either side is kept whole.
FlexVector Concat(const FlexVector &a, const FlexVector &b);
// The vector with `value` on the front, which costs a concatenation rather than a push.
FlexVector PushFront(const FlexVector &v, uint64_t value);
// The vector with `value`, or all of `values`, put in at `pos`, which may be the end.
FlexVector Insert(const FlexVector &v, uint64_t pos, uint64_t value);
FlexVector Insert(const FlexVector &v, uint64_t pos, const FlexVector &values);
// The vector without the element at `pos`, or without `[first, last)`.
FlexVector Erase(const FlexVector &v, uint64_t pos);
FlexVector Erase(const FlexVector &v, uint64_t first, uint64_t last);

// The vector holding `[first, last)`, assembled in one pass rather than one element at a time. Reached
// through the constructors, when they are handed a contiguous range. Builds the strict shape either way,
// which a relaxed vector is free to stand on.
template<Shape S> Trie<S> Build(const uint64_t *first, const uint64_t *last);

// Which ranges go straight to `Build`. A concept and not two conditions, so that they are checked in
// order: `iter_value_t` is an error, not a false, for an iterator lacking one -- as this vector's is.
template<typename It>
concept ValueRange = std::contiguous_iterator<It> && std::same_as<std::iter_value_t<It>, uint64_t>;

// Out of the struct, since a body written inside it cannot see `PushBack`, declared after it.
template<Shape S> template<typename It> Trie<S>::Trie(It first, const It &last) {
    if constexpr (ValueRange<It>) {
        const uint64_t *begin = first == last ? nullptr : &*first;
        *this = Build<S>(begin, begin + (last - first));
    } else {
        for (; first != last; ++first) *this = PushBack(std::move(*this), *first);
    }
}
template<Shape S> Trie<S>::Trie(std::initializer_list<uint64_t> values) : Trie(values.begin(), values.end()) {}

// The element at `index`, or null when the vector is not that long.
template<Shape S> const uint64_t *Get(const Trie<S> &v, uint64_t index) { return index < v.Size ? &v[index] : nullptr; }
template<Shape S> const uint64_t &Front(const Trie<S> &v) { return v[0]; }
template<Shape S> const uint64_t &Back(const Trie<S> &v) { return v[v.Size - 1]; }

// Whether the two vectors hold the same elements in the same order. Constant time between two that never
// diverged. Cheaper than that for the strict shape: equal sizes mean equal shapes there, so any node the
// two share settles a whole subtrie, where the relaxed shape has nothing short of that to go on.
template<Shape S> bool operator==(const Trie<S> &a, const Trie<S> &b);

// Whether `v` is canonical and its shape agrees with its size. A standing check on the representation
// invariant, meant for tests. Linear in the size of the vector.
template<Shape S> bool Check(const Trie<S> &v);

// How many nodes the library holds, so a test can confirm vectors release what they take. Kept only by an
// audit build (-DVECTOR_AUDIT). Empty elsewhere, meaning not counted rather than zero.
std::optional<uint64_t> LiveNodes();

// The bytes of node vectors still point at, against the bytes of slab those nodes are spread over: a walk
// reads the first and covers the second, so a read pays the gap for the holes a build leaves.
// `SpannedBytes` counts only slabs still holding something, `ReservedBytes` all ever taken. Empty in an
// audit build, which has no slabs.
struct Footprint {
    uint64_t LiveBytes, SpannedBytes, ReservedBytes;
};
std::optional<Footprint> Held();

// Every element once, in order, a chunk at a time. A chunk and not an element because `visit` is a
// pointer: calling it once per chunk leaves the loop over the chunk in the caller, where it inlines. The
// chunks of a strict vector are all one length, where a join leaves a relaxed one's whatever length it
// has to. The vector must outlive the call, and nothing may write to it from inside `visit`.
template<Shape S> void ForEachChunk(const Trie<S> &v, void (*visit)(void *context, const uint64_t *first, const uint64_t *last), void *context);
// The same, for anything callable with a pair of `const uint64_t *`.
template<Shape S, typename F> void ForEachChunk(const Trie<S> &v, F &&visit) { ForEachChunk(v, erased::Call<void, F, const uint64_t *, const uint64_t *>, erased::Context(visit)); }
// One element at a time. It costs nothing over the chunked form, since `visit` inlines into that loop.
template<Shape S, typename F> void ForEach(const Trie<S> &v, F &&visit) {
    ForEachChunk(v, [&visit](const uint64_t *first, const uint64_t *last) {
        for (; first != last; ++first) visit(*first);
    });
}

// Walk yielding every element once, in order. It holds the vector it walks, so it keeps yielding the
// elements that vector had when the walk began -- what it buys over `ForEach`, along with stopping early.
//
// It steps by the count a node carries and never by where a slot sits, so one walk serves both shapes,
// and it holds the relaxed one: a strict vector widens into it for free.
struct Iterator {
    // A level consumes at least five index bits, so a path is at most this long.
    static constexpr uint32_t MaxDepth = 13;
    // The children of one node still to be walked.
    struct Frame {
        const Node *const *Child, *const *ChildEnd;
    };

    // `Cur` is null exactly at the end, and comparison against `end()` tests no more than that. The
    // stack is left uninitialized -- braced init would zero all 13 frames, and only those below `Depth`
    // are read.
    FlexVector Source;
    const uint64_t *Cur{}, *End{};
    const Node *Rest{}; // The tail, until the walk reaches it.
    Frame Stack[MaxDepth];
    uint32_t Depth{};

    Iterator() = default;
    template<Shape S> explicit Iterator(const Trie<S> &v) : Source(v) {}

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

template<Shape S> Iterator begin(const Trie<S> &v);
template<Shape S> Iterator end(const Trie<S> &) { return {}; }

// Mutable batching interface over a persistent vector. An lvalue `persistent()` takes a snapshot and
// leaves the transient usable; the rvalue form hands its contents out.
template<Shape S> struct Transient {
    using persistent_type = Trie<S>;
    using value_type = uint64_t;
    using size_type = uint64_t;
    using iterator = Iterator;
    using const_iterator = iterator;

    Transient() = default;
    template<Shape O> Transient(Transient<O> v)
        requires(S == Shape::Relaxed && O == Shape::Strict)
        : Data(v.Data) {}

    size_type size() const { return Data.Size; }
    bool empty() const { return Data.Size == 0; }
    const uint64_t &operator[](uint64_t index) const { return Data[index]; }
    const uint64_t &at(uint64_t index) const {
        if (index >= Data.Size) throw std::out_of_range{"vec::Transient::at"};
        return Data[index];
    }
    iterator begin() const { return vec::begin(Data); }
    iterator end() const { return {}; }

    void push_back(uint64_t value) { Data = PushBack(std::move(Data), value); }
    void set(uint64_t index, uint64_t value) { Data = Set(std::move(Data), index, value); }
    template<typename F> void update(uint64_t index, F &&fn) { Data = Update(std::move(Data), index, std::forward<F>(fn)); }
    void take(uint64_t count) { Data = Take(std::move(Data), count); }
    void drop(uint64_t count) requires(S == Shape::Relaxed) { Data = Drop(std::move(Data), count); }
    void append(const Transient &right) requires(S == Shape::Relaxed) { Data = Concat(Data, right.Data); }
    void prepend(const Transient &left) requires(S == Shape::Relaxed) { Data = Concat(left.Data, Data); }

    persistent_type persistent() & { return Data; }
    persistent_type persistent() && { return std::move(Data); }

private:
    template<Shape> friend struct Transient;
    friend persistent_type;
    explicit Transient(persistent_type data) : Data(std::move(data)) {}

    persistent_type Data;
};

using VectorTransient = Transient<Shape::Strict>;
using FlexVectorTransient = Transient<Shape::Relaxed>;

template<Shape S> auto Trie<S>::transient() const & -> transient_type { return transient_type{*this}; }
template<Shape S> auto Trie<S>::transient() && -> transient_type { return transient_type{std::move(*this)}; }
} // namespace vec
