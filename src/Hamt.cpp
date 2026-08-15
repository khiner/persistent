#include "Hamt.h"

#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

// Branching factor exponent: 32 slots per node at 5, 64 at 6.
#ifndef HAMT_BITS
#define HAMT_BITS 5
#endif

namespace hamt {
namespace {
constexpr uint32_t Bits = HAMT_BITS;
static_assert(Bits == 5 || Bits == 6, "A slot map has to be exactly one machine word wide");

using Bitmap = std::conditional_t<Bits == 6, uint64_t, uint32_t>;
constexpr Bitmap Mask = (Bitmap{1} << Bits) - 1;

// The fold is a bijection, so distinct keys never share a hash and the trie always separates them
// before it bottoms out, which is why there are no collision nodes. It folds the top half of the key
// down and stops there. A second fold would reach into bits that a strided key set does not vary,
// spreading keys that were adjacent. Rationale in docs/Literature.md.
constexpr uint64_t Hash(uint64_t x) { return x ^ (x >> 32); }

constexpr uint32_t Idx(uint64_t hash, uint32_t shift) { return static_cast<uint32_t>((hash >> shift) & Mask); }
} // namespace

static_assert(Iterator::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

// A CHAMP node. `Datamap` marks the slots holding an inline entry, `Nodemap` those holding a child,
// never both. Children then entries trail the header, each packed to the set bits of its map, so a
// slot's position is the popcount of the lower bits. `Refs` is not atomic: no cross-thread sharing.
struct alignas(8) Node {
    mutable uint32_t Refs;
    // The popcounts of the two maps: where the entries start, since they sit behind the children, and
    // how many of them there are. They cost nothing to keep, because the bitmaps have to be 8-byte
    // aligned and so leave half a word spare beside the refcount at either branching factor. Iteration,
    // equality and every write want the entry count, and on ARM a popcount is a move to a vector
    // register, a count and a move back.
    uint16_t Children, Data;
    Bitmap Datamap, Nodemap;
};

namespace {
uint32_t DataCount(const Node &n) { return n.Data; }
uint32_t ChildCount(const Node &n) { return n.Children; }
uint32_t DataOffset(const Node &n, Bitmap bit) { return std::popcount(n.Datamap & (bit - 1)); }
uint32_t ChildOffset(const Node &n, Bitmap bit) { return std::popcount(n.Nodemap & (bit - 1)); }

const Node **Children(Node *n) { return reinterpret_cast<const Node **>(n + 1); }
const Node *const *Children(const Node *n) { return reinterpret_cast<const Node *const *>(n + 1); }
Entry *Entries(Node *n) { return reinterpret_cast<Entry *>(Children(n) + ChildCount(*n)); }
const Entry *Entries(const Node *n) { return reinterpret_cast<const Entry *>(Children(n) + ChildCount(*n)); }

// Nodes are malloc'd, never in fact const. Only reached when the node is uniquely owned.
Node *Mutable(const Node *n) { return const_cast<Node *>(n); }

void Retain(const Node *n) { ++n->Refs; }

// A node's size in 8-byte words. Recoverable from the bitmaps, so a node never stores its own size.
// Rounded up to a pair of words, so that every node is 16-byte aligned and its header cannot straddle
// a cache line. An 8-byte aligned header straddles one visit in eight, and the rounding costs four
// bytes per node on average.
constexpr uint32_t Words(Bitmap datamap, Bitmap nodemap) {
    const auto words = uint32_t(sizeof(Node) / 8) + std::popcount(datamap) * uint32_t(sizeof(Entry) / 8) + std::popcount(nodemap);
    return (words + 1) & ~uint32_t{1};
}

// Nodes are cut from slabs, one size to a slab, and a freed node goes back to the slab it came from
// rather than to the allocator, so the memory stays ours until exit. That blinds the sanitizers: a
// freed node stays reachable and gets handed out again, so a leak looks live. Under -DHAMT_AUDIT
// nodes go back to the allocator and the live ones are counted.
//
// Which freed node a build gets back decides how spread out a map is. A single free list per size
// returns whatever was released last, wherever it came from, so a map built after a large one has been
// dropped is made of that one's nodes and spans everything the process has touched. Per slab, reuse
// stays within memory the size already holds. Measured on the benchmark's last configuration, after
// every earlier one has been through the allocator: lookup was 66% slower than the same map built in a
// fresh process, and is now within noise of it. Where a slab sits in its list once it has room again
// matters too -- see `Release`.
#ifdef HAMT_AUDIT
uint64_t Live = 0;
#else
constexpr uint32_t SizeClasses = Words(~Bitmap{0}, 0) + 1; // All entries, the widest a node gets.
constexpr uint32_t SlabBytes = 64 * 1024;
static_assert(SlabBytes > SizeClasses * 8, "A slab has to hold at least one node of its size");

// A slab is aligned to its own size, so clearing the low bits of a node's address finds the slab it
// came from and a release needs no lookup. One slab serves one size, and allocation stays on a single
// slab until it is full.
struct Slab {
    Slab *Next; // The next slab of this size with room in it.
    const Node *Free; // Nodes released back into this slab, linked through their dead headers.
    uint32_t Bump; // How far allocation has cut into it.
    uint32_t Bytes; // The node size it serves.
    uint32_t Live; // Nodes it has handed out and not had back.
    bool Listed; // A full slab leaves the list until a release puts something back.
};

Slab *Partial[SizeClasses]{};

Slab *SlabOf(const Node *n) { return reinterpret_cast<Slab *>(reinterpret_cast<uintptr_t>(n) & ~uintptr_t(SlabBytes - 1)); }
#endif

void Release(const Node *n) {
    if (--n->Refs) return;
    const auto *const *cs = Children(n);
    for (uint32_t i = 0, nc = ChildCount(*n); i < nc; ++i) Release(cs[i]);
#ifdef HAMT_AUDIT
    --Live;
    std::free(Mutable(n));
#else
    auto *slab = SlabOf(n);
    // A slab holding nothing is reset to a fresh one. Dropping a map empties the slabs it filled, and
    // resetting the bump gives the next map contiguous memory rather than a list of holes.
    const bool emptied = --slab->Live == 0;
    if (emptied) {
        slab->Free = nullptr;
        slab->Bump = uint32_t(sizeof(Slab));
    } else {
        // The header is dead now, so the link to the next free node lives in it.
        *reinterpret_cast<const Node **>(Mutable(n)) = slab->Free;
        slab->Free = n;
    }
    if (!slab->Listed) {
        // An emptied slab goes in front, since it is the one worth allocating from. One that has only
        // lost a node goes behind the slab being filled, so allocation stays put until that slab is
        // used up. Every build frees as it goes, because growing a node releases the one below it, and
        // following each release would move allocation across the whole size class.
        auto &head = Partial[slab->Bytes / 8];
        if (emptied || !head) {
            slab->Next = head;
            head = slab;
        } else {
            slab->Next = head->Next;
            head->Next = slab;
        }
        slab->Listed = true;
    }
#endif
}

void Copy(Entry *dst, const Entry *src, uint32_t n) { std::memcpy(dst, src, n * sizeof(Entry)); }

// Children are copied by reference, so every copy claims one.
void Copy(const Node **dst, const Node *const *src, uint32_t n) {
    std::memcpy(static_cast<void *>(dst), static_cast<const void *>(src), n * sizeof(const Node *));
    for (uint32_t i = 0; i < n; ++i) Retain(dst[i]);
}

// One edit to a node's entry or child array as the node is copied: at `Off`, drop what was there
// when `Del` and store `Ins` when it is given. Both together is a replacement, neither a plain copy.
// `Ins` is stored as it stands, so a child goes in with the reference the new node then holds.
template<typename T> struct Edit {
    uint32_t Off{};
    bool Del{};
    const T *Ins{};
};

template<typename T> void Splice(T *dst, const T *src, uint32_t n, Edit<T> e) {
    Copy(dst, src, e.Off);
    if (e.Ins) dst[e.Off] = *e.Ins;
    Copy(dst + e.Off + (e.Ins != nullptr), src + e.Off + e.Del, n - e.Off - e.Del);
}

// Carries one reference, which passes to the caller.
Node *Alloc(Bitmap datamap, Bitmap nodemap) {
    const auto words = Words(datamap, nodemap);
    Node *n;
#ifdef HAMT_AUDIT
    ++Live;
    n = static_cast<Node *>(std::malloc(words * 8));
#else
    const auto bytes = words * 8;
    auto *slab = Partial[words];
    if (!slab) {
        slab = static_cast<Slab *>(std::aligned_alloc(SlabBytes, SlabBytes));
        *slab = {nullptr, nullptr, uint32_t(sizeof(Slab)), bytes, 0, true};
        Partial[words] = slab;
    }
    ++slab->Live;
    if (slab->Free) {
        n = Mutable(slab->Free);
        slab->Free = *reinterpret_cast<const Node *const *>(n);
    } else {
        n = reinterpret_cast<Node *>(reinterpret_cast<char *>(slab) + slab->Bump);
        slab->Bump += bytes;
    }
    if (!slab->Free && slab->Bump + bytes > SlabBytes) { // Nothing left in it, so off the list.
        Partial[words] = slab->Next;
        slab->Listed = false;
    }
#endif
    n->Refs = 1;
    n->Children = uint16_t(std::popcount(nodemap));
    n->Data = uint16_t(std::popcount(datamap));
    n->Datamap = datamap;
    n->Nodemap = nodemap;
    return n;
}

// A fresh node holding `n`'s contents under the given bitmaps, with one edit to each of its arrays.
// Inlined so that a known `Edit` folds the tests on it away, which is worth about 1% on the write
// rows -- out of line, one shared copy has to branch on all of them.
[[gnu::always_inline]] Node *Rebuilt(const Node *n, Bitmap datamap, Bitmap nodemap, Edit<Entry> de, Edit<const Node *> ce) {
    auto *c = Alloc(datamap, nodemap);
    Splice(Entries(c), Entries(n), DataCount(*n), de);
    Splice(Children(c), Children(n), ChildCount(*n), ce);
    return c;
}

Node *WithEntryInserted(const Node *n, Bitmap bit, Entry e) { return Rebuilt(n, n->Datamap | bit, n->Nodemap, {DataOffset(*n, bit), false, &e}, {}); }
Node *WithEntryRemoved(const Node *n, Bitmap bit) { return Rebuilt(n, n->Datamap ^ bit, n->Nodemap, {DataOffset(*n, bit), true}, {}); }

// The slot moves from the entry side to the child side, for when two keys collide on it.
Node *WithEntryPromoted(const Node *n, Bitmap bit, const Node *child) {
    return Rebuilt(n, n->Datamap ^ bit, n->Nodemap | bit, {DataOffset(*n, bit), true}, {ChildOffset(*n, bit), false, &child});
}

// The reverse, and what keeps the trie canonical: a child down to its last entry folds back inline.
Node *WithChildInlined(const Node *n, Bitmap bit, Entry e) {
    return Rebuilt(n, n->Datamap | bit, n->Nodemap ^ bit, {DataOffset(*n, bit), false, &e}, {ChildOffset(*n, bit), true});
}

// The two in-place rewrites, each paired with the copy it falls back to. Returning `n` itself says
// the write happened in place. Inlined like `Rebuilt`, `WithChild` the more urgently: it runs once
// per level of a path copy.
[[gnu::always_inline]] const Node *WithEntry(const Node *n, uint32_t off, Entry e, bool owned) {
    if (!owned) return Rebuilt(n, n->Datamap, n->Nodemap, {off, true, &e}, {});
    Entries(Mutable(n))[off] = e;
    return n;
}

[[gnu::always_inline]] const Node *WithChild(const Node *n, uint32_t off, const Node *old, const Node *updated, bool owned) {
    if (updated == old) return n;
    if (!owned) return Rebuilt(n, n->Datamap, n->Nodemap, {}, {off, true, &updated});
    Children(Mutable(n))[off] = updated;
    Release(old);
    return n;
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

// `owned` means every node from the root down to `n` is named exactly once, so no other map can see
// a write and rewriting in place is unobservable. Returning `n` leaves the path above it untouched.
SetResult DoSet(const Node *n, Entry e, uint64_t hash, uint32_t shift, bool owned) {
    const Bitmap bit = Bitmap{1} << Idx(hash, shift);
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        const auto old = Entries(n)[off];
        if (old.Key == e.Key) return {WithEntry(n, off, e, owned), false};
        return {WithEntryPromoted(n, bit, Merged(old, Hash(old.Key), e, hash, shift + Bits)), true};
    }
    if (n->Nodemap & bit) {
        const auto off = ChildOffset(*n, bit);
        const auto *child = Children(n)[off];
        const auto r = DoSet(child, e, hash, shift + Bits, owned && child->Refs == 1);
        return {WithChild(n, off, child, r.Root, owned), r.Added};
    }
    return {WithEntryInserted(n, bit, e), true};
}

enum class EraseStatus {
    NotFound,
    Replaced, // `Replacement` stands in for this node.
    Singleton, // This node is down to `Lone`, which belongs inlined a level up.
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
        // A node with a child, or entries to spare, stands on its own. Otherwise the lone survivor
        // belongs inlined in the parent -- except at the root, which has nowhere to hand it.
        if (n->Nodemap || dc > 2) return {EraseStatus::Replaced, WithEntryRemoved(n, bit)};
        if (shift > 0) {
            assert(dc == 2); // Below the root, one entry and no children would have collapsed.
            return {EraseStatus::Singleton, nullptr, Entries(n)[off ^ 1]};
        }
        if (dc == 1) return {EraseStatus::Empty};
        return {EraseStatus::Replaced, WithEntryRemoved(n, bit)};
    }
    if (!(n->Nodemap & bit)) return {EraseStatus::NotFound};
    const auto off = ChildOffset(*n, bit);
    const auto *child = Children(n)[off];
    const auto r = DoErase(child, key, hash, shift + Bits, owned && child->Refs == 1);
    if (r.Status == EraseStatus::Replaced) return {EraseStatus::Replaced, WithChild(n, off, child, r.Replacement, owned)};
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

// `prefix` is the low `shift` hash bits the path down to `n` has already fixed. Confirms every entry
// and child sits in the slot its hash selects, and that no node is empty or collapsible.
bool CheckNode(const Node *n, uint32_t shift, uint64_t prefix, uint64_t &count) {
    if (n->Datamap & n->Nodemap) return false; // A slot holds an entry or a child, never both.
    // Before anything reads an entry: the cached counts are what place and bound the entry array.
    if (n->Children != std::popcount(n->Nodemap) || n->Data != std::popcount(n->Datamap)) return false;
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

// A node with no children gets no frame. Most nodes in a trie have none, and pushing a frame only to
// find it empty and pop it is most of what visiting one costs.
void Descend(Iterator &it, const Node *n) {
    it.Cur = Entries(n);
    it.End = it.Cur + DataCount(*n);
    if (const auto nc = ChildCount(*n)) {
        const auto *const *cs = Children(n);
        it.Stack[it.Depth++] = {cs, cs + nc};
    }
}
} // namespace

Map::Map(const Map &o) : Root(o.Root), Size(o.Size) {
    if (Root) Retain(Root);
}
Map::Map(Map &&o) noexcept : Root(std::exchange(o.Root, nullptr)), Size(std::exchange(o.Size, 0)) {}
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
    // A root rewritten in place is still the one `m` holds, so hand that reference on.
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

const uint64_t *Get(const Map &m, uint64_t key) {
    const auto *n = m.Root;
    if (!n) return nullptr;
    // Shifting the hash costs an instruction less per level than indexing it at a running shift. The
    // loop has no condition because a slot holding a child is never null, so the walk ends by
    // returning. The child map is tested first because most levels descend, and the maps are disjoint,
    // so either order is correct.
    for (auto hash = Hash(key);; hash >>= Bits) {
        const Bitmap bit = Bitmap{1} << (hash & Mask);
        if (n->Nodemap & bit) {
            n = Children(n)[ChildOffset(*n, bit)];
            continue;
        }
        if (!(n->Datamap & bit)) return nullptr;
        const auto &e = Entries(n)[DataOffset(*n, bit)];
        return e.Key == key ? &e.Value : nullptr;
    }
}

bool operator==(const Map &a, const Map &b) {
    if (a.Root == b.Root) return true; // Two empty maps, or two that share a root outright.
    if (!a.Root || !b.Root) return false; // Only an empty map has a null root, so one of them is empty.
    // The size test is an early out and nothing more: `SameNodes` would reject a mismatch anyway.
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
    // Both ends of the frame are read into locals first. Read through the frame instead, clang loads
    // the pair into a vector register and moves it back out, which lands on the path between nodes.
    while (Depth) {
        auto &frame = Stack[Depth - 1];
        const auto *const *child = frame.Child;
        const auto *const *end = frame.ChildEnd;
        if (child == end) {
            --Depth;
            continue;
        }
        frame.Child = child + 1;
        Descend(*this, *child);
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
