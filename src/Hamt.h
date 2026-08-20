#pragma once

#include "Erased.h"

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hamt {
struct Node;

struct Entry {
    uint64_t Key, Value;
};

template<typename E> struct Transient;

// Persistent trie over 64-bit keys, backed by a hash array mapped trie. Every operation returns a new
// trie and leaves its input intact, sharing what the two have in common and releasing it once no trie is
// left holding it. Single threaded, and not per trie: two threads sharing no structure still race.
//
// `E` is what one slot holds. One struct rather than one per element type: the fields, their lifetime,
// and every walk that reads no more than a key are the same whatever an element carries beside it.
template<typename E> struct Trie {
    const Node *Root{};
    // How many low hash bits the root sits above, and what every key holds in them. A level they all
    // agree on costs a field here rather than a node. Zero unless keys share low bits, as addresses do.
    uint32_t Shift{};
    uint64_t Prefix{}, Size{};

    Trie() = default;
    // Adopts an existing reference to `root`.
    Trie(const Node *root, uint64_t size, uint64_t prefix, uint32_t shift) : Root(root), Shift(shift), Prefix(prefix), Size(size) {}
    Trie(const Trie &);
    Trie(Trie &&) noexcept;
    ~Trie();
    Trie &operator=(Trie);

    // The trie holding every one of `elements`, or all of `[first, last)`, where a repeated key ends up
    // as a repeated write would leave it. Any iterator over the element type will do.
    Trie(std::initializer_list<E> elements);
    template<typename It> Trie(It first, const It &last);

    using transient_type = Transient<E>;
    // A mutable view over the same copy-on-write trie. An lvalue keeps this trie alive; an rvalue hands
    // its reference to the transient, just as immer's `transient()` overloads do.
    transient_type transient() const &;
    transient_type transient() &&;
};

using Map = Trie<Entry>;
// The same trie holding bare keys. An element is its own key, so a node is half the width the map's is.
using Set = Trie<uint64_t>;

// Every write takes the trie by value, so move into one when the old trie is done with and it will be
// reused rather than copied.
// The map with `key` bound to `value`. Rebinding an existing key replaces its value, which is why this
// is not `Insert`: the standard library's insert leaves an existing key alone, and `insert_or_assign` is
// the one that does what this does.
Map InsertOrAssign(Map m, uint64_t key, uint64_t value);
// The set with `key` in it. A key it already holds gives the set back untouched, having copied nothing
// on the way to finding that out -- there being nothing to replace, where the map has a value.
Set Insert(Set s, uint64_t key);
// The trie without `key`. A miss returns it unchanged.
template<typename E> Trie<E> Erase(Trie<E> t, uint64_t key);

// The trie holding `[first, last)`, assembled in one pass rather than one key at a time: elements are
// partitioned by hash into the nodes that will hold them, so each node is allocated and written once and
// nothing descends the trie or copies a path. A later duplicate of a key wins. Reached through the
// constructors, when they are handed elements laid out contiguously. Takes scratch of twice the range.
template<typename E> Trie<E> Build(const E *first, const E *last);

// A range already laid out as elements goes straight to `Build`. A concept so the two tests are checked
// in order: `iter_value_t` is an error, not a false, for an iterator lacking one -- as this trie's is.
template<typename It, typename E>
concept ElementRange = std::contiguous_iterator<It> && std::same_as<std::iter_value_t<It>, E>;

// Out of the struct, since a body written inside the class cannot see the writes, declared after it. A
// non-contiguous iterator has to be walked to be read, so that case inserts one at a time.
template<typename E> template<typename It> Trie<E>::Trie(It first, const It &last) {
    if constexpr (ElementRange<It, E>) {
        const E *begin = first == last ? nullptr : &*first;
        *this = Build<E>(begin, begin + (last - first));
    } else if constexpr (std::is_same_v<E, Entry>) {
        for (; first != last; ++first) *this = InsertOrAssign(std::move(*this), first->Key, first->Value);
    } else {
        for (; first != last; ++first) *this = Insert(std::move(*this), *first);
    }
}
template<typename E> Trie<E>::Trie(std::initializer_list<E> elements) : Trie(elements.begin(), elements.end()) {}
// The map with `key` bound to `fn` of the value it holds now, or of zero when it holds none, as
// immer's `update` does. One descent rather than the two a `Get` and an `InsertOrAssign` take.
Map Update(Map m, uint64_t key, uint64_t (*fn)(void *context, uint64_t current), void *context);
// The same, except that a key the map does not hold leaves it unchanged and `fn` uncalled.
Map UpdateIfExists(Map m, uint64_t key, uint64_t (*fn)(void *context, uint64_t current), void *context);
// Both, for anything callable with a `uint64_t`.
template<typename F> Map Update(Map m, uint64_t key, F &&fn) { return Update(std::move(m), key, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }
template<typename F> Map UpdateIfExists(Map m, uint64_t key, F &&fn) { return UpdateIfExists(std::move(m), key, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }
// The value `key` is bound to, or null. It stays put for as long as `m` holds it, so the pointer
// outlives the call but not an update that displaces the entry. A pointer, so a reader avoids a copy.
const uint64_t *Get(const Map &m, uint64_t key);
// The held key itself, or null. The pointer lasts as long as `s` holds the key.
const uint64_t *Get(const Set &s, uint64_t key);
// Whether `s` holds `key`. The same walk `Get` makes, with nothing to hand back at the end of it.
bool Contains(const Set &s, uint64_t key);

// Whether the two tries hold the same elements, whatever order they were built in. Cheap between tries
// that share history, down to constant time for two that never diverged.
template<typename E> bool operator==(const Trie<E> &a, const Trie<E> &b);

// One binding two maps disagree on. `Before` is null for a key only `b` binds, `After` for one only
// `a` binds, and both are set when the value changed. Both point into the maps and last as long as they do.
struct Change {
    uint64_t Key;
    const uint64_t *Before, *After;
};

// Reports every binding `a` and `b` disagree on, once each, in an unspecified order that depends only
// on contents. Shared structure is settled by pointer, so a diff costs what changed and not what did not.
void Diff(const Map &a, const Map &b, void (*report)(void *context, const Change &), void *context);
// The same, for anything callable with a `const Change &`.
template<typename F> void Diff(const Map &a, const Map &b, F &&report) { Diff(a, b, erased::Call<void, F, const Change &>, erased::Context(report)); }

// The same over two sets, where a key can only be gained or lost and never rebound, so a report is the
// key and which way it went rather than a `Change`. `added` says `b` holds it and `a` does not.
void Diff(const Set &a, const Set &b, void (*report)(void *context, uint64_t key, bool added), void *context);
// The same, for anything callable with a `uint64_t` and a `bool`.
template<typename F> void Diff(const Set &a, const Set &b, F &&report) { Diff(a, b, erased::Call<void, F, uint64_t, bool>, erased::Context(report)); }

// Whether `t` is canonical and its size and hash placement agree with its contents. A standing check
// on the representation invariant, meant for tests. Linear in the size of the trie.
template<typename E> bool Check(const Trie<E> &t);

// How many nodes the library holds, so a test can confirm tries release what they take. Kept only by an
// audit build (-DHAMT_AUDIT). Empty elsewhere, meaning not counted rather than zero.
std::optional<uint64_t> LiveNodes();

// The bytes of node tries still point at, against the bytes of slab those nodes are spread over: a walk
// reads the first and covers the second, so a lookup pays the gap for the holes a build leaves.
// `SpannedBytes` counts only slabs still holding something, `ReservedBytes` all ever taken. Empty in an
// audit build, which has no slabs.
struct Footprint {
    uint64_t LiveBytes, SpannedBytes, ReservedBytes;
};
std::optional<Footprint> Held();

// Every element once, in the iterator's order, a node's worth at a time. A chunk and not an element because
// the callback crosses this header through a pointer: crossing once per node leaves the loop over a chunk
// to the caller, where it inlines -- the same reason immer's algorithms are built on `for_each_chunk`.
// The trie must outlive the call, and nothing may write to it from inside `visit`.
template<typename E> void ForEachChunk(const Trie<E> &t, void (*visit)(void *context, const E *first, const E *last), void *context);
// The same, for anything callable with a pair of `const E *`.
template<typename E, typename F> void ForEachChunk(const Trie<E> &t, F &&visit) { ForEachChunk(t, erased::Call<void, F, const E *, const E *>, erased::Context(visit)); }
// One element at a time, for a caller who does not want to write the inner loop. It costs nothing over
// the chunked form, since `visit` inlines into that loop.
template<typename E, typename F> void ForEach(const Trie<E> &t, F &&visit) {
    ForEachChunk(t, [&visit](const E *first, const E *last) {
        for (; first != last; ++first) visit(*first);
    });
}

// Depth-first walk yielding every element once, in an unspecified order that depends only on contents,
// so equal tries iterate identically. It holds the trie it walks, so it keeps yielding the elements that
// trie had when the walk began -- what it buys over `ForEach`, along with stopping early.
template<typename E> struct Iterator {
    // A level consumes at least five hash bits, so a path is at most this long.
    static constexpr uint32_t MaxDepth = 13;
    // The children of one node still to be walked.
    struct Frame {
        const Node *const *Child, *const *ChildEnd;
    };

    // `Cur` is null exactly at the end, and comparison against `end()` tests no more than that. Only
    // frames below `Depth` are meaningful, so the stack is left uninitialized -- braced init would zero
    // all 13.
    Trie<E> Source;
    const E *Cur{}, *End{};
    Frame Stack[MaxDepth];
    uint32_t Depth{};

    Iterator() = default;
    explicit Iterator(const Trie<E> &t) : Source(t) {}

    const E &operator*() const { return *Cur; }
    const E *operator->() const { return Cur; }
    bool operator==(const Iterator &o) const { return Cur == o.Cur; }
    Iterator &operator++() {
        if (++Cur == End) Advance();
        return *this;
    }

    // Moves on to the next elements there are, or reports the end by clearing `Cur`.
    void Advance();
};

template<typename E> Iterator<E> begin(const Trie<E> &t);
template<typename E> Iterator<E> end(const Trie<E> &) { return {}; }

// Mutable batching interface over a persistent trie. An lvalue `persistent()` takes a snapshot and
// leaves the transient usable; the rvalue form hands its contents out.
template<typename E> struct Transient {
    using persistent_type = Trie<E>;
    using value_type = E;
    using size_type = uint64_t;
    using iterator = Iterator<E>;
    using const_iterator = iterator;

    Transient() = default;

    size_type size() const { return Data.Size; }
    bool empty() const { return Data.Size == 0; }
    iterator begin() const { return hamt::begin(Data); }
    iterator end() const { return {}; }

    size_type count(uint64_t key) const { return find(key) ? 1 : 0; }
    const uint64_t *find(uint64_t key) const { return Get(Data, key); }
    const uint64_t &operator[](uint64_t key) const
        requires(std::is_same_v<E, Entry>) {
        static constexpr uint64_t Missing{};
        const auto *found = Get(Data, key);
        return found ? *found : Missing;
    }
    const uint64_t &at(uint64_t key) const
        requires(std::is_same_v<E, Entry>) {
        const auto *found = Get(Data, key);
        if (!found) throw std::out_of_range{"hamt::MapTransient::at"};
        return *found;
    }

    void insert(E value) {
        if constexpr (std::is_same_v<E, Entry>) Data = InsertOrAssign(std::move(Data), value.Key, value.Value);
        else Data = Insert(std::move(Data), value);
    }
    void set(uint64_t key, uint64_t value) requires(std::is_same_v<E, Entry>) {
        Data = InsertOrAssign(std::move(Data), key, value);
    }
    template<typename F> void update(uint64_t key, F &&fn) requires(std::is_same_v<E, Entry>) {
        Data = Update(std::move(Data), key, std::forward<F>(fn));
    }
    template<typename F> void update_if_exists(uint64_t key, F &&fn) requires(std::is_same_v<E, Entry>) {
        Data = UpdateIfExists(std::move(Data), key, std::forward<F>(fn));
    }
    void erase(uint64_t key) { Data = Erase(std::move(Data), key); }

    persistent_type persistent() & { return Data; }
    persistent_type persistent() && { return std::move(Data); }

private:
    friend persistent_type;
    explicit Transient(persistent_type data) : Data(std::move(data)) {}

    persistent_type Data;
};

using MapTransient = Transient<Entry>;
using SetTransient = Transient<uint64_t>;

template<typename E> auto Trie<E>::transient() const & -> transient_type { return transient_type{*this}; }
template<typename E> auto Trie<E>::transient() && -> transient_type { return transient_type{std::move(*this)}; }
} // namespace hamt
