#include "Hamt.h"

#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

// Branching factor exponent, so 32 slots per node at 5 and 64 at 6. Set from CMake to sweep it.
#ifndef HAMT_BITS
#define HAMT_BITS 5
#endif

namespace hamt {
namespace {
constexpr uint32_t Bits = HAMT_BITS;
static_assert(Bits == 5 || Bits == 6, "A slot map has to be exactly one machine word wide");

using Bitmap = std::conditional_t<Bits == 6, uint64_t, uint32_t>;
constexpr Bitmap Mask = (Bitmap{1} << Bits) - 1;

// Fold the high bits down twice, then read the trie from the bottom up. Each fold is a bijection on
// uint64, so distinct keys never share a hash, and the levels between them consume all 64 hash bits
// -- any two keys separate before the trie bottoms out. That is why there are no collision nodes here.
//
// What the folding is for is the two ways a real key set is not random. A fold maps [0, 2^k) into
// itself, so a dense key set stays dense, and dense is the best case a trie has: every node full,
// every key at the same depth, neighbouring keys in the same node. A counter or a table of object
// ids hands us that for free, and multiplying would throw it away. Meanwhile a key set that arrives
// aligned -- heap addresses, anything scaled by a power of two -- has low bits that never vary, and
// reading those first would spend whole levels resolving nothing. Folding fills them in from bits
// that do vary. An identity hash keeps the first property and loses the second.
constexpr uint64_t Hash(uint64_t x) {
    x ^= x >> 32;
    x ^= x >> 16;
    return x;
}

// The next `Bits` of the hash above the `shift` bits the path here has already consumed.
constexpr uint32_t Idx(uint64_t hash, uint32_t shift) { return static_cast<uint32_t>((hash >> shift) & Mask); }
} // namespace

static_assert(Iterator::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

// A CHAMP node. `Datamap` marks the slots holding an inline entry, `Nodemap` the slots holding a
// child, and no slot is in both. Children and entries trail the header in that order, each packed
// to the set bits of its map, so a slot's position is the popcount of the lower bits.
// `Refs` counts the maps and parent nodes naming this one. It is not atomic: sharing a map across
// threads is not supported, and rpds measures the atomic version at up to 2x on this traffic.
struct alignas(8) Node {
    mutable uint32_t Refs;
    // popcount(Nodemap), which is where the entries start. Cached because the entries are behind the
    // children, so every walk that ends in one would otherwise pay a popcount to find them, and
    // because it costs nothing: the alignment the bitmaps need leaves this word spare at both widths.
    uint32_t Children;
    Bitmap Datamap, Nodemap;
};

namespace {
uint32_t DataCount(const Node &n) { return std::popcount(n.Datamap); }
uint32_t ChildCount(const Node &n) { return n.Children; }
uint32_t DataOffset(const Node &n, Bitmap bit) { return std::popcount(n.Datamap & (bit - 1)); }
uint32_t ChildOffset(const Node &n, Bitmap bit) { return std::popcount(n.Nodemap & (bit - 1)); }

// Children come first so that descending a level costs one popcount and no arithmetic on the base.
// That is the step a lookup repeats, against a single entry access at the end of it.
const Node **Children(Node *n) { return reinterpret_cast<const Node **>(n + 1); }
const Node *const *Children(const Node *n) { return reinterpret_cast<const Node *const *>(n + 1); }
Entry *Entries(Node *n) { return reinterpret_cast<Entry *>(Children(n) + ChildCount(*n)); }
const Entry *Entries(const Node *n) { return reinterpret_cast<const Entry *>(Children(n) + ChildCount(*n)); }

// Nodes live in malloc'd storage, never in read-only memory, so writing through this is well formed.
// Only reachable when the node is uniquely owned, which is what makes the write unobservable.
Node *Mutable(const Node *n) { return const_cast<Node *>(n); }

void Retain(const Node *n) { ++n->Refs; }

// A node's size, in the 8-byte words it is always a whole number of. Recoverable from the bitmaps
// alone, which is what lets a node be returned to the right free list without storing its size.
constexpr uint32_t Words(Bitmap datamap, Bitmap nodemap) {
    return uint32_t(sizeof(Node) / 8) + std::popcount(datamap) * uint32_t(sizeof(Entry) / 8) + std::popcount(nodemap);
}

// Freed nodes go to a list per size, rather than back to the allocator. A trie churns nodes of a few
// dozen sizes at a rate malloc is not built for, and the node a path copy frees is nearly always the
// size the next one wants. immer does the same thing for the same reason.
// The cost is that the memory stays ours until exit. The lists hold every node the process has freed.
//
// It costs the sanitizers too, which is what the audit build is for. A freed node stays reachable and
// gets handed out again, so a leak is indistinguishable from a live node and a use after free reads as
// an ordinary access. A double release is not caught where it happens either: the link below overwrites
// `Refs`, so the decrement lands in half a pointer, and what surfaces is a node handed out at the wrong
// size somewhere else entirely. Under -DHAMT_AUDIT nodes go straight back to the allocator and the live
// ones are counted, which puts all three back within reach of a test and of asan, at the site.
#ifdef HAMT_AUDIT
uint64_t Live = 0;
#else
constexpr uint32_t SizeClasses = Words(~Bitmap{0}, 0) + 1; // All entries, the widest a node gets.
const Node *Freed[SizeClasses]{};
#endif

void Release(const Node *n) {
    if (--n->Refs) return;
    const auto *const *cs = Children(n);
    for (uint32_t i = 0, nc = ChildCount(*n); i < nc; ++i) Release(cs[i]);
#ifdef HAMT_AUDIT
    --Live;
    std::free(Mutable(n));
#else
    auto &list = Freed[Words(n->Datamap, n->Nodemap)];
    // The header is dead now, so the link to the next free node lives in it.
    *reinterpret_cast<const Node **>(Mutable(n)) = list;
    list = n;
#endif
}

void CopyEntries(Entry *dst, const Entry *src, uint32_t n) { std::memcpy(dst, src, n * sizeof(Entry)); }

// Children are copied by reference, so every clone claims one. A child left out of a clone keeps
// the reference its old parent holds, and a freshly built child is assigned rather than cloned.
void CloneChildren(const Node **dst, const Node *const *src, uint32_t n) {
    std::memcpy(static_cast<void *>(dst), static_cast<const void *>(src), n * sizeof(const Node *));
    for (uint32_t i = 0; i < n; ++i) Retain(dst[i]);
}

// The returned node carries the one reference its caller is expected to hand on.
Node *Alloc(Bitmap datamap, Bitmap nodemap) {
    const auto words = Words(datamap, nodemap);
    Node *n;
#ifdef HAMT_AUDIT
    ++Live;
    n = static_cast<Node *>(std::malloc(words * 8));
#else
    auto &list = Freed[words];
    if (list) {
        n = Mutable(list);
        list = *reinterpret_cast<const Node *const *>(n);
    } else {
        n = static_cast<Node *>(std::malloc(words * 8));
    }
#endif
    n->Refs = 1;
    n->Children = std::popcount(nodemap);
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

Node *WithEntryInserted(const Node *n, Bitmap bit, Entry e) {
    const auto dc = DataCount(*n), off = DataOffset(*n, bit);
    auto *c = Alloc(n->Datamap | bit, n->Nodemap);
    auto *es = Entries(c);
    CopyEntries(es, Entries(n), off);
    es[off] = e;
    CopyEntries(es + off + 1, Entries(n) + off, dc - off);
    CloneChildren(Children(c), Children(n), ChildCount(*n));
    return c;
}

Node *WithEntryRemoved(const Node *n, Bitmap bit) {
    const auto dc = DataCount(*n), off = DataOffset(*n, bit);
    auto *c = Alloc(n->Datamap ^ bit, n->Nodemap);
    auto *es = Entries(c);
    CopyEntries(es, Entries(n), off);
    CopyEntries(es + off, Entries(n) + off + 1, dc - off - 1);
    CloneChildren(Children(c), Children(n), ChildCount(*n));
    return c;
}

// The slot moves from the entry side to the child side, for when two keys collide on it.
Node *WithEntryPromoted(const Node *n, Bitmap bit, const Node *child) {
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
Node *WithChildInlined(const Node *n, Bitmap bit, Entry e) {
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
        auto *n = Alloc(0, Bitmap{1} << ia);
        Children(n)[0] = Merged(a, ha, b, hb, shift + Bits);
        return n;
    }
    auto *n = Alloc((Bitmap{1} << ia) | (Bitmap{1} << ib), 0);
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
    const Bitmap bit = Bitmap{1} << Idx(hash, shift);
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
    const Bitmap bit = Bitmap{1} << Idx(hash, shift);
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

bool SameNodes(const Node *a, const Node *b) {
    if (a == b) return true; // Structure the two maps share settles a whole subtrie at once.
    if (a->Datamap != b->Datamap || a->Nodemap != b->Nodemap) return false;
    // A slot's position is fixed by the bitmap, so equal nodes hold their entries in the same order.
    if (std::memcmp(Entries(a), Entries(b), DataCount(*a) * sizeof(Entry)) != 0) return false;
    const auto *const *ca = Children(a);
    const auto *const *cb = Children(b);
    for (uint32_t i = 0, nc = ChildCount(*a); i < nc; ++i)
        if (!SameNodes(ca[i], cb[i])) return false;
    return true;
}

// `prefix` is the low `shift` hash bits the path down to `n` has already fixed, and `count`
// accumulates the entries found. Confirms every entry and child sits in the slot its hash selects,
// and that no node is empty or collapsible -- the two ways shape could stop being a function of contents.
bool CheckNode(const Node *n, uint32_t shift, uint64_t prefix, uint64_t &count) {
    if (n->Datamap & n->Nodemap) return false; // A slot holds an entry or a child, never both.
    // Checked before anything reads an entry, because the cached count is what places the entries:
    // a stale one moves the whole entry array and everything below would be looking at the wrong bytes.
    if (n->Children != uint32_t(std::popcount(n->Nodemap))) return false;
    const auto dc = DataCount(*n), nc = ChildCount(*n);
    if (dc + nc == 0) return false; // Empty nodes are never built, not even at the root.
    if (shift > 0 && nc == 0 && dc == 1) return false; // A lone entry belongs inlined in the parent.
    count += dc;
    auto dm = n->Datamap;
    for (uint32_t i = 0; dm; dm &= dm - 1, ++i) {
        const auto hash = Hash(Entries(n)[i].Key);
        if ((hash & ((uint64_t{1} << shift) - 1)) != prefix) return false;
        if (Idx(hash, shift) != uint32_t(std::countr_zero(dm))) return false;
    }
    auto nm = n->Nodemap;
    for (uint32_t i = 0; nm; nm &= nm - 1, ++i) {
        const auto slot = uint64_t(std::countr_zero(nm));
        if (!CheckNode(Children(n)[i], shift + Bits, prefix | (slot << shift), count)) return false;
    }
    return true;
}

// Points `it` at `n`'s entries and queues `n`'s children behind them.
void Descend(Iterator &it, const Node *n) {
    it.Cur = Entries(n);
    it.End = it.Cur + DataCount(*n);
    const auto *const *cs = Children(n);
    it.Stack[it.Depth++] = {cs, cs + ChildCount(*n)};
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
        auto *n = Alloc(Bitmap{1} << Idx(hash, 0), 0);
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
        const Bitmap bit = Bitmap{1} << Idx(hash, shift);
        if (n->Datamap & bit) {
            const auto &e = Entries(n)[DataOffset(*n, bit)];
            return e.Key == key ? std::optional{e.Value} : std::nullopt;
        }
        if (!(n->Nodemap & bit)) break;
        n = Children(n)[ChildOffset(*n, bit)];
    }
    return {};
}

bool operator==(const Map &a, const Map &b) {
    if (a.Root == b.Root) return true; // Two empty maps, or two that share a root outright.
    if (!a.Root || !b.Root) return false; // Only an empty map has a null root, so one of them is empty.
    // The size test is an early out and nothing more. Equal contents in a canonical trie means equal
    // structure, so `SameNodes` would reject a size mismatch on its own, just after walking for it.
    return a.Size == b.Size && SameNodes(a.Root, b.Root);
}

bool Check(const Map &m) {
    if (!m.Root) return m.Size == 0;
    uint64_t count = 0;
    return CheckNode(m.Root, 0, 0, count) && count == m.Size;
}

std::optional<uint64_t> LiveNodes() {
#ifdef HAMT_AUDIT
    return Live;
#else
    return {};
#endif
}

void Iterator::Advance() {
    while (Depth) {
        auto &frame = Stack[Depth - 1];
        if (frame.Child == frame.ChildEnd) {
            --Depth;
            continue;
        }
        Descend(*this, *frame.Child++);
        if (Cur != End) return;
    }
    Cur = End = nullptr;
}

Iterator begin(const Map &m) {
    Iterator it{m}; // The copy is the reference that holds the structure below still.
    if (!m.Root) return it;
    Descend(it, m.Root);
    if (it.Cur == it.End) it.Advance(); // A root holding only children has nothing to yield yet.
    return it;
}
} // namespace hamt
