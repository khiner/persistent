#pragma once

#include "Erased.h"

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <utility>

namespace hamt {
struct Node;

struct Entry {
    uint64_t Key, Value;
};

// Persistent map from 64-bit keys to values, backed by a hash array mapped trie. Every operation returns
// a new map and leaves its input intact, sharing what the two have in common and releasing it once no
// map is left holding it. Single threaded, and not per map: two threads sharing no structure still race.
struct Map {
    const Node *Root{};
    // How many low hash bits the root sits above, and what every key holds in them. A level they all
    // agree on costs a field here rather than a node. Zero unless keys share low bits, as addresses do.
    uint32_t Shift{};
    uint64_t Prefix{}, Size{};

    Map() = default;
    // Adopts an existing reference to `root`.
    Map(const Node *root, uint64_t size, uint64_t prefix, uint32_t shift) : Root(root), Shift(shift), Prefix(prefix), Size(size) {}
    Map(const Map &);
    Map(Map &&) noexcept;
    ~Map();
    Map &operator=(Map);

    // The map holding every one of `entries`, or all of `[first, last)`, where a repeated key ends up
    // as a repeated `Set` would leave it. Any iterator over something with a `Key` and a `Value` will do.
    Map(std::initializer_list<Entry> entries);
    template<typename It> Map(It first, const It &last);
};

// Every write takes the map by value, so move into one when the old map is done with and it will be
// reused rather than copied.
// The map with `key` bound to `value`. Rebinding an existing key replaces its value.
Map Set(Map m, uint64_t key, uint64_t value);
// The map without `key`. A miss returns the map unchanged.
Map Erase(Map m, uint64_t key);

// The map holding `[first, last)`, assembled in one pass rather than one key at a time: entries are
// partitioned by hash into the nodes that will hold them, so each node is allocated and written once and
// nothing descends the trie or copies a path. A later duplicate of a key wins. Reached through the
// constructors, when they are handed entries laid out contiguously. Takes scratch of twice the range.
Map Build(const Entry *first, const Entry *last);

// A range already laid out as entries goes straight to `Build`. A concept so the two tests are checked
// in order: `iter_value_t` is an error, not a false, for an iterator lacking one -- as this map's is.
template<typename It>
concept EntryRange = std::contiguous_iterator<It> && std::same_as<std::iter_value_t<It>, Entry>;

// Out of the struct, since a body written inside the class cannot see `Set`, declared after it. A
// non-contiguous iterator has to be walked to be read, so that case inserts one at a time.
template<typename It> Map::Map(It first, const It &last) {
    if constexpr (EntryRange<It>) {
        const Entry *begin = first == last ? nullptr : &*first;
        *this = Build(begin, begin + (last - first));
    } else {
        for (; first != last; ++first) *this = Set(std::move(*this), first->Key, first->Value);
    }
}
inline Map::Map(std::initializer_list<Entry> entries) : Map(entries.begin(), entries.end()) {}
// The map with `key` bound to `fn` of the value it holds now, or of zero when it holds none, as
// immer's `update` does. One descent rather than the two a `Get` and a `Set` take.
Map Update(Map m, uint64_t key, uint64_t (*fn)(void *context, uint64_t current), void *context);
// The same, except that a key the map does not hold leaves it unchanged and `fn` uncalled.
Map UpdateIfExists(Map m, uint64_t key, uint64_t (*fn)(void *context, uint64_t current), void *context);
// Both, for anything callable with a `uint64_t`.
template<typename F> Map Update(Map m, uint64_t key, F &&fn) { return Update(std::move(m), key, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }
template<typename F> Map UpdateIfExists(Map m, uint64_t key, F &&fn) { return UpdateIfExists(std::move(m), key, erased::Call<uint64_t, F, uint64_t>, erased::Context(fn)); }
// The value `key` is bound to, or null. It stays put for as long as `m` holds it, so the pointer
// outlives the call but not an update that displaces the entry. A pointer, so a reader avoids a copy.
const uint64_t *Get(const Map &m, uint64_t key);

// Whether the two maps hold the same bindings, whatever order they were built in. Cheap between maps
// that share history, down to constant time for two that never diverged.
bool operator==(const Map &a, const Map &b);

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

// Whether `m` is canonical and its size and hash placement agree with its contents. A standing check
// on the representation invariant, meant for tests. Linear in the size of the map.
bool Check(const Map &m);

// How many nodes the library holds, so a test can confirm maps release what they take. Kept only by an
// audit build (-DHAMT_AUDIT). Empty elsewhere, meaning not counted rather than zero.
std::optional<uint64_t> LiveNodes();

// The bytes of node maps still point at, against the bytes of slab those nodes are spread over: a walk
// reads the first and covers the second, so a lookup pays the gap for the holes a build leaves.
// `SpannedBytes` counts only slabs still holding something, `ReservedBytes` all ever taken. Empty in an
// audit build, which has no slabs.
struct Footprint {
    uint64_t LiveBytes, SpannedBytes, ReservedBytes;
};
std::optional<Footprint> Held();

// Every binding once, in the iterator's order, a node's worth at a time. A chunk and not an entry because
// the callback crosses this header through a pointer: crossing once per node leaves the loop over a chunk
// to the caller, where it inlines -- the same reason immer's algorithms are built on `for_each_chunk`.
// The map must outlive the call, and nothing may write to it from inside `visit`.
void ForEachChunk(const Map &m, void (*visit)(void *context, const Entry *first, const Entry *last), void *context);
// The same, for anything callable with a pair of `const Entry *`.
template<typename F> void ForEachChunk(const Map &m, F &&visit) { ForEachChunk(m, erased::Call<void, F, const Entry *, const Entry *>, erased::Context(visit)); }
// One binding at a time, for a caller who does not want to write the inner loop. It costs nothing over
// the chunked form, since `visit` inlines into that loop.
template<typename F> void ForEach(const Map &m, F &&visit) {
    ForEachChunk(m, [&visit](const Entry *first, const Entry *last) {
        for (; first != last; ++first) visit(*first);
    });
}

// Depth-first walk yielding every entry once, in an unspecified order that depends only on contents,
// so equal maps iterate identically. It holds the map it walks, so it keeps yielding the bindings that
// map had when the walk began -- what it buys over `ForEach`, along with stopping early.
struct Iterator {
    // A level consumes at least five hash bits, so a path is at most this long.
    static constexpr uint32_t MaxDepth = 13;
    // The children of one node still to be walked.
    struct Frame {
        const Node *const *Child, *const *ChildEnd;
    };

    // `Cur` is null exactly at the end, and comparison against `end()` tests no more than that. Only
    // frames below `Depth` are meaningful, so the stack is left uninitialized -- braced init would zero
    // all 13.
    Map Source;
    const Entry *Cur{}, *End{};
    Frame Stack[MaxDepth];
    uint32_t Depth{};

    Iterator() = default;
    explicit Iterator(const Map &m) : Source(m) {}

    const Entry &operator*() const { return *Cur; }
    const Entry *operator->() const { return Cur; }
    bool operator==(const Iterator &o) const { return Cur == o.Cur; }
    Iterator &operator++() {
        if (++Cur == End) Advance();
        return *this;
    }

    // Moves on to the next entries there are, or reports the end by clearing `Cur`.
    void Advance();
};

Iterator begin(const Map &m);
inline Iterator end(const Map &) { return {}; }
} // namespace hamt
