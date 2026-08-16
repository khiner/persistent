#pragma once

// The trie node both vector shapes are built from, and the allocator it is cut from. Internal: nothing
// outside Vector.cpp includes this, and the node is opaque in the header.
//
// `inline` rather than an anonymous namespace, because Hamt.cpp has the same allocator and a slab's free
// list cannot be split in two.

#include <cstdint>
#include <cstdlib>
#include <cstring>

// Branching factor exponent: 32 slots per node at 5, 64 at 6. CMakeLists.txt has the choice and what
// was measured either way, and this is the same default for a build that does not set it.
#ifndef VECTOR_BITS
#define VECTOR_BITS 6
#endif

namespace vec {
constexpr uint32_t Bits = VECTOR_BITS;
static_assert(Bits == 5 || Bits == 6, "The two widths the trade has been measured at");

constexpr uint64_t Mask = (uint64_t{1} << Bits) - 1;
constexpr uint32_t Slots = 1u << Bits;
// How many elements a node standing at `shift` covers.
constexpr uint64_t Covers(uint32_t shift) { return uint64_t{1} << (shift + Bits); }

// A trie node: children when it stands above the leaves, values when it is one. Which it is comes from
// where it sits, which every walk already knows, so a node does not say. `Refs` is not atomic: no
// cross-thread sharing.
struct alignas(16) Node {
    // The slots first and the header behind them, so a child load folds the slot index into its
    // addressing mode rather than stepping over a header first. Worth 6% on a random index.
    union {
        const Node *Children[Slots];
        uint64_t Values[Slots];
    };
    // Null when the children are full for the level they stand on, so the slot an index falls in is a
    // shift and a mask. Otherwise a node whose values are the running totals -- `Values[i]` is how many
    // elements sit under children `0` through `i` -- and finding a slot is a search. Always null in a
    // strict vector. Refcounted like any other node, so a write that moves no element hands its copy
    // the same table rather than another `Slots` words of it.
    const Node *Sizes;
    mutable uint32_t Refs;
    // How many of the slots are taken. Rounding the node up to its alignment leaves room for it, so
    // keeping it costs nothing over deriving it from where the node sits, and a copy reads it straight off.
    uint32_t Count;
};
static_assert(sizeof(Node) == Slots * sizeof(uint64_t) + 16, "The header has to sit in the padding the slots leave");

inline const Node **Children(Node *n) { return n->Children; }
inline const Node *const *Children(const Node *n) { return n->Children; }
inline uint64_t *Values(Node *n) { return n->Values; }
inline const uint64_t *Values(const Node *n) { return n->Values; }

// Nodes are malloc'd, never in fact const. Only reached when the node is uniquely owned.
inline Node *Mutable(const Node *n) { return const_cast<Node *>(n); }

inline void Retain(const Node *n) { ++n->Refs; }

// Every node is cut to the same size, a full slot count of pointers being a full slot count of values,
// so a tail can grow in place: the room a push wants is already there.
constexpr uint32_t NodeBytes = uint32_t(sizeof(Node));

// Nodes are cut from slabs, and a freed node goes back to its own slab rather than to the allocator, so
// the memory stays ours until exit. That blinds the sanitizers -- a freed node stays reachable -- so
// -DVECTOR_AUDIT frees to the allocator instead and counts the live ones. Hamt.cpp has the same
// allocator, and why reuse is kept inside a slab.
#ifdef VECTOR_AUDIT
inline uint64_t Live = 0;
#else
constexpr uint32_t SlabBytes = 64 * 1024;
static_assert(SlabBytes > 2 * NodeBytes, "A slab has to hold more than one node");

// A slab is aligned to its own size, so clearing the low bits of a node's address finds it and a release
// needs no lookup.
struct alignas(16) Slab {
    Slab *Next; // The next slab with room in it.
    Slab *All; // Every slab ever taken, so the footprint can be totted up.
    const Node *Free; // Nodes released back into this slab, linked through their dead headers.
    uint32_t Bump; // How far allocation has cut into it.
    uint32_t Live; // Nodes it has handed out and not had back.
    bool Listed; // A full slab leaves the list until a release puts something back.
};
static_assert(sizeof(Slab) % 16 == 0, "The first node behind the header has to stay 16-byte aligned");

inline Slab *Partial = nullptr, *AllSlabs = nullptr;

inline Slab *SlabOf(const Node *n) { return reinterpret_cast<Slab *>(reinterpret_cast<uintptr_t>(n) & ~uintptr_t(SlabBytes - 1)); }
#endif

// A leaf, carrying one reference, which passes to the caller. The slots are left as they were, so
// `count` of them are the caller's to fill -- and so is `Sizes`, which nothing reads on a leaf.
// Clearing it here rather than in `AllocInner` costs 9 to 15% on `build copy` and `take`, a leaf being
// all but one node in `Slots + 1`.
inline Node *Alloc(uint32_t count) {
    Node *n;
#ifdef VECTOR_AUDIT
    ++Live;
    n = static_cast<Node *>(std::malloc(NodeBytes));
#else
    auto *slab = Partial;
    if (!slab) {
        slab = static_cast<Slab *>(std::aligned_alloc(SlabBytes, SlabBytes));
        *slab = {nullptr, AllSlabs, nullptr, uint32_t(sizeof(Slab)), 0, true};
        Partial = AllSlabs = slab;
    }
    ++slab->Live;
    if (slab->Free) {
        n = Mutable(slab->Free);
        slab->Free = *reinterpret_cast<const Node *const *>(n);
    } else {
        n = reinterpret_cast<Node *>(reinterpret_cast<char *>(slab) + slab->Bump);
        slab->Bump += NodeBytes;
    }
    if (!slab->Free && slab->Bump + NodeBytes > SlabBytes) { // Nothing left in it, so off the list.
        Partial = slab->Next;
        slab->Listed = false;
    }
#endif
    n->Refs = 1;
    n->Count = count;
    return n;
}

// A node standing above the leaves. Only these can carry a size table, so only these have to say
// whether they do. The strict vector's inner nodes come from here too.
inline Node *AllocInner(uint32_t count) {
    auto *n = Alloc(count);
    n->Sizes = nullptr;
    return n;
}

// `shift` is the level `n` stands on, and says whether its slots hold children.
inline void Release(const Node *n, uint32_t shift) {
    if (--n->Refs) return;
    if (shift) {
        const auto *const *cs = Children(n);
        for (uint32_t i = 0, nc = n->Count; i < nc; ++i) Release(cs[i], shift - Bits);
        if (n->Sizes) Release(n->Sizes, 0); // The table is a node, and goes the way its own does.
    }
#ifdef VECTOR_AUDIT
    --Live;
    std::free(Mutable(n));
#else
    auto *slab = SlabOf(n);
    // A slab holding nothing is reset. Dropping a vector empties the slabs it filled, and resetting the
    // bump gives the next vector contiguous memory rather than a list of holes.
    const bool emptied = --slab->Live == 0;
    if (emptied) {
        slab->Free = nullptr;
        slab->Bump = uint32_t(sizeof(Slab));
    } else {
        // The node is dead now, so the link to the next free one lives in its first slot.
        *reinterpret_cast<const Node **>(Mutable(n)) = slab->Free;
        slab->Free = n;
    }
    if (!slab->Listed) {
        // An emptied slab goes in front, being the one worth allocating from. One that has only lost a
        // node goes behind the slab being filled, so allocation stays put until that slab is used up.
        if (emptied || !Partial) {
            slab->Next = Partial;
            Partial = slab;
        } else {
            slab->Next = Partial->Next;
            Partial->Next = slab;
        }
        slab->Listed = true;
    }
#endif
}

// A fresh leaf holding the first `count` of `n`'s values, or all of them and room for more.
inline Node *CopiedLeaf(const Node *n, uint32_t count) {
    auto *c = Alloc(count);
    std::memcpy(Values(c), Values(n), (count < n->Count ? count : n->Count) * sizeof(uint64_t));
    return c;
}

// The levels between `shift` and the leaves, as a chain holding `leaf` at the bottom. One child each,
// since the chunk it carries is the first thing to reach that far.
inline const Node *NewPath(uint32_t shift, const Node *leaf) {
    for (; shift; shift -= Bits) {
        auto *n = AllocInner(1);
        Children(n)[0] = leaf;
        leaf = n;
    }
    return leaf;
}
} // namespace vec
