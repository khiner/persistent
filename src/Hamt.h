#pragma once

#include <cstdint>
#include <optional>

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
};

// `Set` and `Erase` take the map by value, so move into them when the old map is no longer wanted
// and they will reuse it rather than copy.
// The map with `key` bound to `value`. Rebinding an existing key replaces its value.
Map Set(Map m, uint64_t key, uint64_t value);
// The map without `key`. A miss returns the map unchanged.
Map Erase(Map m, uint64_t key);
// The value `key` is bound to, or null if it is bound to nothing. The value stays where it is for as
// long as `m` holds it, so the pointer outlives the call but not an update that displaces the entry.
// A pointer rather than an `optional`, so that a caller who only reads the value avoids the copy.
const uint64_t *Get(const Map &m, uint64_t key);

// Whether the two maps hold the same bindings, independent of the order they were built in.
// Cheap between maps that share history, down to constant time for two that never diverged.
bool operator==(const Map &a, const Map &b);

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

// Depth-first walk yielding every entry once. The order is unspecified but depends only on contents,
// so equal maps iterate identically. An iterator holds the map it walks, so it stays valid however
// that map is later updated, and keeps yielding the bindings it had when the walk began.
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
