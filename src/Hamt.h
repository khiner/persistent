#pragma once

#include <cstdint>
#include <optional>

namespace hamt {
struct Node;

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
} // namespace hamt
