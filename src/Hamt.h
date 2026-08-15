#pragma once

#include <cstdint>
#include <optional>

namespace hamt {
struct Node;

struct Entry {
    uint64_t Key, Value;
};

// Persistent map from 64-bit keys to 64-bit values, backed by a hash array mapped trie.
// Every operation returns a new map and leaves its input intact, with the two sharing all unchanged structure.
// A map holds a reference to the structure it shares, and shared structure lives until the last map naming it dies.
struct Map {
    const Node *Root{};
    uint64_t Size{};

    Map() = default;
    Map(const Node *root, uint64_t size) : Root(root), Size(size) {} // Adopts an existing reference to `root`.
    Map(const Map &);
    Map(Map &&) noexcept;
    ~Map();
    Map &operator=(Map);
};

// `Set` and `Erase` take the map by value, so passing an rvalue hands them sole ownership of it.
// The map with `key` bound to `value`. Rebinding an existing key replaces its value.
Map Set(Map m, uint64_t key, uint64_t value);
// The map without `key`. A miss returns the map unchanged.
Map Erase(Map m, uint64_t key);
// The value bound to `key`, if any.
std::optional<uint64_t> Get(const Map &m, uint64_t key);

// Whether the two maps hold the same bindings. The trie is canonical -- shape is a function of
// contents -- so this is a structural comparison, and any subtree the two share settles by pointer.
bool operator==(const Map &a, const Map &b);

// Whether `m` is in canonical form and its size and hash placement agree with its contents.
// A standing check on the representation invariant, meant for tests: linear in the size of the map.
bool Check(const Map &m);

// Depth-first walk yielding every entry once. The order is unspecified, but it follows from the trie
// shape alone, so equal maps iterate identically.
struct Iterator {
    // A level consumes at least five hash bits, so a path is at most this long.
    static constexpr uint32_t MaxDepth = 13;
    // The children of one node still to be walked.
    struct Frame {
        const Node *const *Child, *const *ChildEnd;
    };

    const Entry *Cur{}, *End{}; // The entries of the node being yielded from. `Cur` is null at the end.
    Frame Stack[MaxDepth];
    uint32_t Depth{};

    const Entry &operator*() const { return *Cur; }
    const Entry *operator->() const { return Cur; }
    bool operator==(const Iterator &o) const { return Cur == o.Cur; }
    // A node's entries are contiguous, so a step is a pointer bump until the node runs out.
    Iterator &operator++() {
        if (++Cur == End) Advance();
        return *this;
    }

    // Moves to the next node holding entries, or reports the end by clearing `Cur`.
    void Advance();
};

Iterator begin(const Map &m);
inline Iterator end(const Map &) { return {}; }
} // namespace hamt
