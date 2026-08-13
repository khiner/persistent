#include "Hamt.h"

#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace hamt {
namespace {
constexpr uint32_t Bits = 5, Mask = (1u << Bits) - 1;

struct Entry {
    uint64_t Key, Value;
};

// splitmix64's finalizer. It is a bijection on uint64, so distinct keys never share a hash,
// and 13 levels of 5 bits cover all 64 hash bits -- any two keys separate before the trie
// bottoms out. That is why there are no collision nodes here.
// Mixing is not what buys that (identity would too). It buys a shape that does not depend on
// the key distribution, so pointer-like keys with zeroed low bits stay shallow.
constexpr uint64_t Hash(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

constexpr uint32_t Idx(uint64_t hash, uint32_t shift) { return (hash >> shift) & Mask; }
} // namespace

// A CHAMP node. `Datamap` marks the slots holding an inline entry, `Nodemap` the slots holding a
// child, and no slot is in both. Entries and children trail the header in that order, each packed
// to the set bits of its map, so a slot's position is the popcount of the lower bits.
// `Refs` counts the maps and parent nodes naming this one. It is not atomic: sharing a map across
// threads is not supported, and rpds measures the atomic version at up to 2x on this traffic.
struct alignas(8) Node {
    mutable uint32_t Refs;
    uint32_t Datamap, Nodemap;
};

namespace {
uint32_t DataCount(const Node &n) { return std::popcount(n.Datamap); }
uint32_t ChildCount(const Node &n) { return std::popcount(n.Nodemap); }
uint32_t DataOffset(const Node &n, uint32_t bit) { return std::popcount(n.Datamap & (bit - 1)); }
uint32_t ChildOffset(const Node &n, uint32_t bit) { return std::popcount(n.Nodemap & (bit - 1)); }

Entry *Entries(Node *n) { return reinterpret_cast<Entry *>(n + 1); }
const Entry *Entries(const Node *n) { return reinterpret_cast<const Entry *>(n + 1); }
const Node **Children(Node *n) { return reinterpret_cast<const Node **>(Entries(n) + DataCount(*n)); }
const Node *const *Children(const Node *n) { return reinterpret_cast<const Node *const *>(Entries(n) + DataCount(*n)); }

// Nodes live in malloc'd storage, never in read-only memory, so writing through this is well formed.
// Only reachable when the node is uniquely owned, which is what makes the write unobservable.
Node *Mutable(const Node *n) { return const_cast<Node *>(n); }

void Retain(const Node *n) { ++n->Refs; }

void Release(const Node *n) {
    if (--n->Refs) return;
    const auto *const *cs = Children(n);
    for (uint32_t i = 0, nc = ChildCount(*n); i < nc; ++i) Release(cs[i]);
    std::free(const_cast<Node *>(n));
}

void CopyEntries(Entry *dst, const Entry *src, uint32_t n) { std::memcpy(dst, src, n * sizeof(Entry)); }

// Children are copied by reference, so every clone claims one. A child left out of a clone keeps
// the reference its old parent holds, and a freshly built child is assigned rather than cloned.
void CloneChildren(const Node **dst, const Node *const *src, uint32_t n) {
    std::memcpy(static_cast<void *>(dst), static_cast<const void *>(src), n * sizeof(const Node *));
    for (uint32_t i = 0; i < n; ++i) Retain(dst[i]);
}

// The returned node carries the one reference its caller is expected to hand on.
Node *Alloc(uint32_t datamap, uint32_t nodemap) {
    auto *n = static_cast<Node *>(std::malloc(sizeof(Node) + std::popcount(datamap) * sizeof(Entry) + std::popcount(nodemap) * sizeof(const Node *)));
    n->Refs = 1;
    n->Datamap = datamap;
    n->Nodemap = nodemap;
    return n;
}

Node *CopyOf(const Node *n) {
    auto *c = Alloc(n->Datamap, n->Nodemap);
    CopyEntries(Entries(c), Entries(n), DataCount(*n));
    CloneChildren(Children(c), Children(n), ChildCount(*n));
    return c;
}

Node *WithChildReplaced(const Node *n, uint32_t off, const Node *child) {
    const auto nc = ChildCount(*n);
    auto *c = Alloc(n->Datamap, n->Nodemap);
    CopyEntries(Entries(c), Entries(n), DataCount(*n));
    auto **cs = Children(c);
    CloneChildren(cs, Children(n), off);
    cs[off] = child;
    CloneChildren(cs + off + 1, Children(n) + off + 1, nc - off - 1);
    return c;
}

Node *WithEntryInserted(const Node *n, uint32_t bit, Entry e) {
    const auto dc = DataCount(*n), off = DataOffset(*n, bit);
    auto *c = Alloc(n->Datamap | bit, n->Nodemap);
    auto *es = Entries(c);
    CopyEntries(es, Entries(n), off);
    es[off] = e;
    CopyEntries(es + off + 1, Entries(n) + off, dc - off);
    CloneChildren(Children(c), Children(n), ChildCount(*n));
    return c;
}

Node *WithEntryRemoved(const Node *n, uint32_t bit) {
    const auto dc = DataCount(*n), off = DataOffset(*n, bit);
    auto *c = Alloc(n->Datamap ^ bit, n->Nodemap);
    auto *es = Entries(c);
    CopyEntries(es, Entries(n), off);
    CopyEntries(es + off, Entries(n) + off + 1, dc - off - 1);
    CloneChildren(Children(c), Children(n), ChildCount(*n));
    return c;
}

// The slot moves from the entry side to the child side, for when two keys collide on it.
Node *WithEntryPromoted(const Node *n, uint32_t bit, const Node *child) {
    const auto dc = DataCount(*n), nc = ChildCount(*n);
    const auto doff = DataOffset(*n, bit), coff = ChildOffset(*n, bit);
    auto *c = Alloc(n->Datamap ^ bit, n->Nodemap | bit);
    auto *es = Entries(c);
    CopyEntries(es, Entries(n), doff);
    CopyEntries(es + doff, Entries(n) + doff + 1, dc - doff - 1);
    auto **cs = Children(c);
    CloneChildren(cs, Children(n), coff);
    cs[coff] = child;
    CloneChildren(cs + coff + 1, Children(n) + coff, nc - coff);
    return c;
}

// The reverse: a child that has collapsed to its last entry folds back into the entry side.
// This is what keeps the trie canonical, so shape is a function of contents alone.
Node *WithChildInlined(const Node *n, uint32_t bit, Entry e) {
    const auto dc = DataCount(*n), nc = ChildCount(*n);
    const auto doff = DataOffset(*n, bit), coff = ChildOffset(*n, bit);
    auto *c = Alloc(n->Datamap | bit, n->Nodemap ^ bit);
    auto *es = Entries(c);
    CopyEntries(es, Entries(n), doff);
    es[doff] = e;
    CopyEntries(es + doff + 1, Entries(n) + doff, dc - doff);
    auto **cs = Children(c);
    CloneChildren(cs, Children(n), coff);
    CloneChildren(cs + coff, Children(n) + coff + 1, nc - coff - 1);
    return c;
}

// Two entries whose hashes agree up to `shift`, as a subtrie: one node per level they still share.
const Node *Merged(Entry a, uint64_t ha, Entry b, uint64_t hb, uint32_t shift) {
    assert(shift < 64); // Distinct hashes always diverge before this.
    const auto ia = Idx(ha, shift), ib = Idx(hb, shift);
    if (ia == ib) {
        auto *n = Alloc(0, 1u << ia);
        Children(n)[0] = Merged(a, ha, b, hb, shift + Bits);
        return n;
    }
    auto *n = Alloc((1u << ia) | (1u << ib), 0);
    auto *es = Entries(n);
    es[ia < ib ? 0 : 1] = a;
    es[ia < ib ? 1 : 0] = b;
    return n;
}

struct SetResult {
    const Node *Root;
    bool Added;
};

// `owned` means every node from the root down to `n` is named exactly once, so no other map can
// see `n` and rewriting it in place is unobservable. Returning `n` itself says that is what happened,
// which lets the whole path above it stay untouched.
SetResult DoSet(const Node *n, Entry e, uint64_t hash, uint32_t shift, bool owned) {
    const uint32_t bit = 1u << Idx(hash, shift);
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        const auto old = Entries(n)[off];
        if (old.Key == e.Key) { // A rebind leaves the shape alone, so an owned node takes it directly.
            if (owned) {
                Entries(Mutable(n))[off] = e;
                return {n, false};
            }
            auto *c = CopyOf(n);
            Entries(c)[off] = e;
            return {c, false};
        }
        return {WithEntryPromoted(n, bit, Merged(old, Hash(old.Key), e, hash, shift + Bits)), true};
    }
    if (n->Nodemap & bit) {
        const auto off = ChildOffset(*n, bit);
        const auto *child = Children(n)[off];
        const auto r = DoSet(child, e, hash, shift + Bits, owned && child->Refs == 1);
        if (r.Root == child) return {n, r.Added};
        if (owned) { // The slot is ours to repoint, and the old child loses its last name here.
            Children(Mutable(n))[off] = r.Root;
            Release(child);
            return {n, r.Added};
        }
        return {WithChildReplaced(n, off, r.Root), r.Added};
    }
    return {WithEntryInserted(n, bit, e), true};
}

enum class EraseStatus {
    NotFound,
    Replaced, // `Replacement` stands in for this node.
    Singleton, // This node held only `Lone`, which the parent must absorb.
    Empty, // The whole map is gone. Only the root can report this.
};

struct EraseResult {
    EraseStatus Status;
    const Node *Replacement{};
    Entry Lone{};
};

EraseResult DoErase(const Node *n, uint64_t key, uint64_t hash, uint32_t shift, bool owned) {
    const uint32_t bit = 1u << Idx(hash, shift);
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        if (Entries(n)[off].Key != key) return {EraseStatus::NotFound};
        const auto dc = DataCount(*n);
        if (n->Nodemap || dc > 2) return {EraseStatus::Replaced, WithEntryRemoved(n, bit)};
        if (dc == 2) {
            const auto lone = Entries(n)[off ^ 1];
            if (shift > 0) return {EraseStatus::Singleton, nullptr, lone};
            auto *c = Alloc(n->Datamap ^ bit, 0); // Nothing above the root to absorb it.
            Entries(c)[0] = lone;
            return {EraseStatus::Replaced, c};
        }
        assert(shift == 0); // Below the root, one entry and no children would have collapsed.
        return {EraseStatus::Empty};
    }
    if (!(n->Nodemap & bit)) return {EraseStatus::NotFound};
    const auto off = ChildOffset(*n, bit);
    const auto *child = Children(n)[off];
    const auto r = DoErase(child, key, hash, shift + Bits, owned && child->Refs == 1);
    if (r.Status == EraseStatus::Replaced) {
        if (r.Replacement == child) return {EraseStatus::Replaced, n};
        if (owned) {
            Children(Mutable(n))[off] = r.Replacement;
            Release(child);
            return {EraseStatus::Replaced, n};
        }
        return {EraseStatus::Replaced, WithChildReplaced(n, off, r.Replacement)};
    }
    if (r.Status == EraseStatus::Singleton) {
        // Keep passing it up while this node has nothing of its own to hold it.
        if (!n->Datamap && ChildCount(*n) == 1 && shift > 0) return r;
        return {EraseStatus::Replaced, WithChildInlined(n, bit, r.Lone)};
    }
    assert(r.Status == EraseStatus::NotFound); // A child collapses to a singleton, never to empty.
    return {EraseStatus::NotFound};
}
} // namespace

Map::Map(const Map &o) : Root(o.Root), Size(o.Size) {
    if (Root) Retain(Root);
}
Map::Map(Map &&o) noexcept : Root(o.Root), Size(o.Size) {
    o.Root = nullptr;
    o.Size = 0;
}
Map::~Map() {
    if (Root) Release(Root);
}
Map &Map::operator=(Map o) {
    std::swap(Root, o.Root);
    std::swap(Size, o.Size);
    return *this;
}

Map Set(Map m, uint64_t key, uint64_t value) {
    const Entry e{key, value};
    const auto hash = Hash(key);
    if (!m.Root) {
        auto *n = Alloc(1u << Idx(hash, 0), 0);
        Entries(n)[0] = e;
        return {n, 1};
    }
    const auto r = DoSet(m.Root, e, hash, 0, m.Root->Refs == 1);
    // A root rewritten in place is still the one `m` holds, so hand that reference on rather than
    // claiming a second one against it.
    if (r.Root == m.Root) {
        m.Size += r.Added;
        return m;
    }
    return {r.Root, m.Size + r.Added};
}

Map Erase(Map m, uint64_t key) {
    if (!m.Root) return m;
    const auto r = DoErase(m.Root, key, Hash(key), 0, m.Root->Refs == 1);
    if (r.Status == EraseStatus::Replaced) {
        if (r.Replacement == m.Root) {
            --m.Size;
            return m;
        }
        return {r.Replacement, m.Size - 1};
    }
    if (r.Status == EraseStatus::Empty) return {};
    assert(r.Status == EraseStatus::NotFound); // Singletons never escape the root.
    return m;
}

std::optional<uint64_t> Get(const Map &m, uint64_t key) {
    const auto hash = Hash(key);
    uint32_t shift = 0;
    for (const auto *n = m.Root; n; shift += Bits) {
        const uint32_t bit = 1u << Idx(hash, shift);
        if (n->Datamap & bit) {
            const auto &e = Entries(n)[DataOffset(*n, bit)];
            return e.Key == key ? std::optional{e.Value} : std::nullopt;
        }
        if (!(n->Nodemap & bit)) break;
        n = Children(n)[ChildOffset(*n, bit)];
    }
    return {};
}
} // namespace hamt
