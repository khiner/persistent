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

// Each fold is a bijection, so distinct keys never share a hash and the trie always separates them
// before it bottoms out -- which is why there are no collision nodes. Rationale in docs/Literature.md.
constexpr uint64_t Hash(uint64_t x) {
    x ^= x >> 32;
    x ^= x >> 16;
    return x;
}

constexpr uint32_t Idx(uint64_t hash, uint32_t shift) { return static_cast<uint32_t>((hash >> shift) & Mask); }
} // namespace

static_assert(Iterator::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

// A CHAMP node. `Datamap` marks the slots holding an inline entry, `Nodemap` those holding a child,
// never both. Children then entries trail the header, each packed to the set bits of its map, so a
// slot's position is the popcount of the lower bits. `Refs` is not atomic: no cross-thread sharing.
struct alignas(8) Node {
    mutable uint32_t Refs;
    // popcount(Nodemap), where the entries start. Cached because they sit behind the children.
    uint32_t Children;
    Bitmap Datamap, Nodemap;
};

namespace {
uint32_t DataCount(const Node &n) { return std::popcount(n.Datamap); }
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
constexpr uint32_t Words(Bitmap datamap, Bitmap nodemap) {
    return uint32_t(sizeof(Node) / 8) + std::popcount(datamap) * uint32_t(sizeof(Entry) / 8) + std::popcount(nodemap);
}

// Freed nodes go to a list per size rather than back to the allocator, so the memory stays ours until
// exit. That also blinds the sanitizers: a freed node stays reachable and gets handed out again, so a
// leak looks live. Under -DHAMT_AUDIT nodes go back to the allocator and the live ones are counted.
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
    // Before anything reads an entry: the cached count is what places the entry array.
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
