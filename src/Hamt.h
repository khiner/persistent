#pragma once

#include <cstdint>
#include <optional>

namespace hamt {
struct Node;

// Persistent map from 64-bit keys to 64-bit values, backed by a hash array mapped trie.
// Every operation returns a new map and leaves its input intact, with the two sharing all unchanged structure.
struct Map {
    const Node *Root{};
    uint64_t Size{};
};

// The map with `key` bound to `value`. Rebinding an existing key replaces its value.
Map Set(Map m, uint64_t key, uint64_t value);
// The map without `key`. A miss returns the map unchanged.
Map Erase(Map m, uint64_t key);
// The value bound to `key`, if any.
std::optional<uint64_t> Get(Map m, uint64_t key);
} // namespace hamt
