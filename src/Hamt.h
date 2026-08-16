#pragma once

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

namespace hamt {
struct Node;

struct Entry {
    uint64_t Key, Value;
};

// Persistent map from 64-bit keys to 64-bit values, backed by a hash array mapped trie. Every
// operation returns a new map and leaves its input intact, sharing whatever the two have in common
// rather than copying it, and releasing it once no map is left holding it.
//
// Single threaded, and not per map but altogether: two threads whose maps share no structure at all
// still race.
struct Map {
    const Node *Root{};
    // How many low hash bits the root sits above, and what every key's hash holds in them. A level the
    // whole map agrees on tells a walk nothing, so the map records it here instead of spending a node
    // on it. Both are zero unless the keys share low bits, as aligned addresses do.
    uint32_t Shift{};
    uint64_t Prefix{}, Size{};

    Map() = default;
    // Adopts an existing reference to `root`.
    Map(const Node *root, uint64_t size, uint64_t prefix, uint32_t shift) : Root(root), Shift(shift), Prefix(prefix), Size(size) {}
    Map(const Map &);
    Map(Map &&) noexcept;
    ~Map();
    Map &operator=(Map);

    // The map holding every one of `entries`, or everything in `[first, last)`, where a repeated key
    // ends up bound by the last of them as a repeated `Set` would leave it. Any iterator over
    // something with a `Key` and a `Value` will do, this map's own included.
    Map(std::initializer_list<Entry> entries);
    template<typename It> Map(It first, It last);
};

// Every write here takes the map by value, so move into one when the old map is no longer wanted and
// it will reuse that map rather than copy it.
// The map with `key` bound to `value`. Rebinding an existing key replaces its value.
Map Set(Map m, uint64_t key, uint64_t value);
// The map without `key`. A miss returns the map unchanged.
Map Erase(Map m, uint64_t key);

// The map holding `[first, last)`, assembled in one pass instead of one key at a time: the entries
// are partitioned by hash into the nodes that will hold them, so every node is allocated once and
// written once, and nothing descends the trie or copies a path. A later duplicate of a key wins, as
// a repeated `Set` would leave it. The constructors are the spelling to reach for -- this is what
// they call when they are handed entries laid out contiguously. It takes scratch of twice the range
// while it runs, to partition between, and gives it back before it returns.
Map Build(const Entry *first, const Entry *last);

// A range already laid out as entries can go straight to `Build`. Written as a concept so that the
// two tests are checked in order: `iter_value_t` is an error rather than a false for an iterator
// that does not declare one, and this map's own is one of those.
template<typename It>
concept EntryRange = std::contiguous_iterator<It> && std::same_as<std::iter_value_t<It>, Entry>;

// Defined here rather than in the struct, since a member body written inside the class cannot see a
// namespace function declared after it, and `Set` needs `Map` complete before it can be declared.
// An iterator that is not contiguous has to be walked to be read, so that case inserts one at a time
// rather than buffering the whole range first.
template<typename It> Map::Map(It first, It last) {
    if constexpr (EntryRange<It>) {
        const Entry *begin = first == last ? nullptr : &*first;
        *this = Build(begin, begin + (last - first));
    } else {
        for (; first != last; ++first) *this = Set(std::move(*this), first->Key, first->Value);
    }
}
inline Map::Map(std::initializer_list<Entry> entries) : Map(entries.begin(), entries.end()) {}
// The map with `key` bound to `fn` of the value it is bound to now, or of zero when it is bound to
// nothing, as immer's `update` does. One descent rather than the two a `Get` and a `Set` take, since
// the value is computed where the walk finds the entry.
Map Update(Map m, uint64_t key, uint64_t (*fn)(void *context, uint64_t current), void *context);
// The same, except that a key the map does not hold leaves it unchanged and `fn` uncalled.
Map UpdateIfExists(Map m, uint64_t key, uint64_t (*fn)(void *context, uint64_t current), void *context);
// Both, for anything callable with a `uint64_t`. Passing the callable as a context rather than a
// `std::function` is what keeps the walk out of the header, as on `Diff`. A `const` callable loses
// its `const` only to make the hop through `void *`, and has it back before it is called.
template<typename F> Map Update(Map m, uint64_t key, F &&fn) {
    using Held = std::remove_reference_t<F>;
    return Update(
        std::move(m), key, [](void *context, uint64_t current) -> uint64_t { return (*static_cast<Held *>(context))(current); }, const_cast<std::remove_const_t<Held> *>(&fn)
    );
}
template<typename F> Map UpdateIfExists(Map m, uint64_t key, F &&fn) {
    using Held = std::remove_reference_t<F>;
    return UpdateIfExists(
        std::move(m), key, [](void *context, uint64_t current) -> uint64_t { return (*static_cast<Held *>(context))(current); }, const_cast<std::remove_const_t<Held> *>(&fn)
    );
}
// The value `key` is bound to, or null if it is bound to nothing. The value stays where it is for as
// long as `m` holds it, so the pointer outlives the call but not an update that displaces the entry.
// A pointer rather than an `optional`, so that a caller who only reads the value avoids the copy.
const uint64_t *Get(const Map &m, uint64_t key);

// Whether the two maps hold the same bindings, independent of the order they were built in.
// Cheap between maps that share history, down to constant time for two that never diverged.
bool operator==(const Map &a, const Map &b);

// One binding two maps disagree on. `Before` is null when `b` binds a key `a` did not, `After` is
// null when `a` bound one `b` does not, and both are set when the value changed. Both point into the
// maps themselves, so they last as long as those maps hold the entry.
struct Change {
    uint64_t Key;
    const uint64_t *Before, *After;
};

// Reports every binding `a` and `b` disagree on, once each, in an order that is unspecified but
// depends only on their contents. Structure the two maps share is settled by pointer rather than
// walked, so diffing a map against one it was built from costs what changed and not what did not.
void Diff(const Map &a, const Map &b, void (*report)(void *context, const Change &), void *context);
// The same, for anything callable with a `const Change &`. Nothing is allocated and the walk stays
// out of line, which is why the callable is passed as a context rather than a `std::function`.
template<typename F> void Diff(const Map &a, const Map &b, F &&report) {
    Diff(
        a, b, [](void *context, const Change &c) { (*static_cast<std::remove_reference_t<F> *>(context))(c); }, &report
    );
}

// Whether `m` is in canonical form and its size and hash placement agree with its contents.
// A standing check on the representation invariant, meant for tests. Linear in the size of the map.
bool Check(const Map &m);

// How many nodes the library is holding, so a test can confirm that maps release what they take.
// Only an audit build (-DHAMT_AUDIT) keeps it. Empty elsewhere, meaning not counted rather than zero.
std::optional<uint64_t> LiveNodes();

// The bytes of node that maps still point at, against the bytes of slab those nodes are spread over.
// A walk reads the first and covers the second, so the gap between them is what a lookup pays for the
// holes a build leaves behind. `SpannedBytes` counts only slabs still holding something, where
// `ReservedBytes` is everything the allocator has taken from the system and never given back.
// Empty in an audit build, which has no slabs.
struct Footprint {
    uint64_t LiveBytes, SpannedBytes, ReservedBytes;
};
std::optional<Footprint> Held();

// Every binding once, in the order the iterator below yields them, a node's worth of them at a time.
// A chunk and not an entry because the callback has to cross this header through a pointer, and
// crossing once per node instead of once per binding is the difference: the loop over a chunk is the
// caller's own and inlines, where a call per binding does not. The same reason immer's algorithms
// are built on `for_each_chunk` rather than on its iterator. The map has to outlive the call, and
// nothing may write to it from inside `visit`.
void ForEachChunk(const Map &m, void (*visit)(void *context, const Entry *first, const Entry *last), void *context);
// The same, for anything callable with a pair of `const Entry *`, as on `Diff` and `Update`.
template<typename F> void ForEachChunk(const Map &m, F &&visit) {
    using Held = std::remove_reference_t<F>;
    ForEachChunk(
        m, [](void *context, const Entry *first, const Entry *last) { (*static_cast<Held *>(context))(first, last); }, const_cast<std::remove_const_t<Held> *>(&visit)
    );
}
// One binding at a time, for a caller who does not want to write the inner loop. It costs nothing
// over the chunked form, since `visit` is inlined into the loop rather than called through it.
template<typename F> void ForEach(const Map &m, F &&visit) {
    ForEachChunk(m, [&visit](const Entry *first, const Entry *last) {
        for (; first != last; ++first) visit(*first);
    });
}

// Depth-first walk yielding every entry once. The order is unspecified but depends only on contents,
// so equal maps iterate identically. An iterator holds the map it walks, so it stays valid however
// that map is later updated, and keeps yielding the bindings it had when the walk began -- which is
// what it buys over `ForEach`, along with stopping early and composing with the standard algorithms.
struct Iterator {
    // A level consumes at least five hash bits, so a path is at most this long.
    static constexpr uint32_t MaxDepth = 13;
    // The children of one node still to be walked.
    struct Frame {
        const Node *const *Child, *const *ChildEnd;
    };

    // Walk state. `Cur` is null exactly at the end, which is what `end()` compares equal to. Only the
    // frames below `Depth` are meaningful, so the stack is left uninitialized. That is what the
    // constructors are for: braced initialization would zero all 13 frames instead.
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
