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
constexpr uint32_t Slots = Mask + 1;

// Fold the key, do not scramble it. Counters, object ids and row numbers hand over a dense key set,
// the best case a trie has, which any good mixer would destroy. Each xor-shift is a bijection, so a
// fold keeps it -- and distinct keys never share a hash, so there are no collision nodes.
//
// One fold and no more: a heap address is dense on a *stride*, and folding bits 16 and up into the low
// four leaves such a set a permutation of itself yet no longer a lattice -- four times the nodes for
// the same entries, on the very shape a second fold was meant to protect. Keys whose low bits never
// vary -- the case it did protect against -- go to `Trie::Shift`.
constexpr uint64_t Hash(uint64_t x) { return x ^ (x >> 32); }

constexpr uint32_t Idx(uint64_t hash, uint32_t shift) { return static_cast<uint32_t>((hash >> shift) & Mask); }
// The lowest slot a map still has set, as a one-bit mask, for a loop clearing them one at a time.
constexpr Bitmap Lowest(Bitmap m) { return m & ~(m - 1); }

// Where a walk finds the key in an element, and all of one that anything below a write reads.
constexpr uint64_t KeyOf(const Entry &e) { return e.Key; }
constexpr uint64_t KeyOf(uint64_t key) { return key; }

// What a write does with a key the trie already holds. The map replaces the value bound to it. The set
// has nothing to replace, so it hands back the node it was given and copies no path above it.
enum struct OnPresent {
    Rebind,
    Keep,
};
} // namespace

static_assert(Iterator<Entry>::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

// A CHAMP node. `Datamap` marks the slots holding an inline element, `Nodemap` those holding a child,
// never both. Children then elements trail the header, packed to the set bits of each map, so a slot's
// position is the popcount of the lower bits. `Refs` is not atomic: no cross-thread sharing. The header
// says nothing about what an element is, so one node type and one allocator serve every element type.
struct alignas(8) Node {
    mutable uint32_t Refs;
    // The popcounts of the two maps: where the elements start, since they sit behind the children, and how
    // many there are, which iteration, equality and every write want. Free to keep, the bitmaps having to
    // be 8-byte aligned, and on ARM a popcount is a round trip to a vector register.
    uint16_t Children, Data;
    Bitmap Datamap, Nodemap;
};

namespace {
uint32_t DataCount(const Node &n) { return n.Data; }
uint32_t ChildCount(const Node &n) { return n.Children; }
uint32_t DataOffset(const Node &n, Bitmap bit) { return std::popcount(n.Datamap & (bit - 1)); }
uint32_t ChildOffset(const Node &n, Bitmap bit) { return std::popcount(n.Nodemap & (bit - 1)); }
// The same where the walk already knows the slot. A node whose every slot holds a child has an all-ones
// nodemap, so the offset is the slot itself -- which skips a popcount, done on arm64 through the vector
// unit and sitting between the bitmap and the load it indexes. The top levels of a large trie are full.
uint32_t ChildOffset(const Node &n, Bitmap bit, uint32_t slot) {
    return n.Children == Slots ? slot : std::popcount(n.Nodemap & (bit - 1));
}

const Node **Children(Node *n) { return reinterpret_cast<const Node **>(n + 1); }
const Node *const *Children(const Node *n) { return reinterpret_cast<const Node *const *>(n + 1); }
template<typename E> E *Entries(Node *n) { return reinterpret_cast<E *>(Children(n) + ChildCount(*n)); }
template<typename E> const E *Entries(const Node *n) { return reinterpret_cast<const E *>(Children(n) + ChildCount(*n)); }

// Nodes are malloc'd, never in fact const. Only reached when the node is uniquely owned.
Node *Mutable(const Node *n) { return const_cast<Node *>(n); }

void Retain(const Node *n) { ++n->Refs; }

// A node's size in 8-byte words, recoverable from the bitmaps so a node never stores it. Rounded up to
// a pair of words, so every node is 16-byte aligned and its header cannot straddle a cache line: an
// 8-byte aligned header straddles one visit in eight, and the rounding costs four bytes per node.
template<typename E> constexpr uint32_t Words(Bitmap datamap, Bitmap nodemap) {
    const auto words = uint32_t(sizeof(Node) / 8) + std::popcount(datamap) * uint32_t(sizeof(E) / 8) + std::popcount(nodemap);
    return (words + 1) & ~uint32_t{1};
}

// Nodes are cut from slabs, one size to a slab, and a freed node goes back to its own slab rather than
// to the allocator, so the memory stays ours until exit. That blinds the sanitizers -- a freed node
// stays reachable -- so -DHAMT_AUDIT frees to the allocator instead and counts the live ones.
//
// Which freed node a build gets back decides how spread out a map is. One free list per size hands back
// whatever was released last, so a map built after a large one was dropped is made of that one's nodes
// and spans everything the process has touched. Per slab, reuse stays within memory the size already
// holds, worth 66% on lookup. Where a slab sits in its list matters too, see `Release`.
#ifdef HAMT_AUDIT
uint64_t Live = 0;
#else
// One array of free lists serves every element type, so this is the widest of them.
constexpr uint32_t SizeClasses = Words<Entry>(~Bitmap{0}, 0) + 1;
// Width is linear in the two slot counts, so the widest node is all of one kind or all of the other.
static_assert(Words<Entry>(0, ~Bitmap{0}) <= Words<Entry>(~Bitmap{0}, 0), "A node of all children has to fit the size class array too");
static_assert(Words<uint64_t>(~Bitmap{0}, 0) < SizeClasses && Words<uint64_t>(0, ~Bitmap{0}) < SizeClasses, "Every element type has to fit the array the widest one sizes");
constexpr uint32_t SlabBytes = 64 * 1024;
static_assert(SlabBytes > SizeClasses * 8, "A slab has to hold at least one node of its size");

// A slab is aligned to its own size, so clearing the low bits of a node's address finds it and a
// release needs no lookup. One slab serves one size, and allocation stays on one until it is full. The
// header is 16-byte aligned and a whole number of word pairs wide, so nodes behind it keep that.
struct alignas(16) Slab {
    Slab *Next; // The next slab of this size with room in it.
    Slab *All; // Every slab ever taken, so the footprint can be totted up.
    const Node *Free; // Nodes released back into this slab, linked through their dead headers.
    uint32_t Bump; // How far allocation has cut into it.
    uint32_t Bytes; // The node size it serves.
    uint32_t Live; // Nodes it has handed out and not had back.
    bool Listed; // A full slab leaves the list until a release puts something back.
};
static_assert(sizeof(Slab) % 16 == 0, "The first node behind the header has to stay 16-byte aligned");

Slab *Partial[SizeClasses]{}, *AllSlabs = nullptr;

Slab *SlabOf(const Node *n) { return reinterpret_cast<Slab *>(reinterpret_cast<uintptr_t>(n) & ~uintptr_t(SlabBytes - 1)); }
#endif

// No element type: a node says how many children it has, and its slab how wide it is.
void Release(const Node *n) {
    if (--n->Refs) return;
    const auto *const *cs = Children(n);
    for (uint32_t i = 0, nc = ChildCount(*n); i < nc; ++i) Release(cs[i]);
#ifdef HAMT_AUDIT
    --Live;
    std::free(Mutable(n));
#else
    auto *slab = SlabOf(n);
    // A slab holding nothing is reset. Dropping a map empties the slabs it filled, and resetting the
    // bump gives the next map contiguous memory rather than a list of holes.
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
        // An emptied slab goes in front, being the one worth allocating from. One that has only lost a
        // node goes behind the slab being filled, so allocation stays put until that slab is used up:
        // every build frees as it goes, and following each release would cross the whole size class.
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

// A loop and not `std::memcpy`: a runtime length cannot fold into a few stores, so it became a call
// across the library boundary, and a node's arrays are only tens of bytes.
template<typename E> void Copy(E *dst, const E *src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst[i] = src[i];
}

// Children are copied by reference, so every copy claims one -- in the same pass, not a second one over
// the array it just wrote. An overload and not a specialization, so it wins inside `Splice`.
void Copy(const Node **dst, const Node *const *src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const auto *c = src[i];
        dst[i] = c;
        Retain(c);
    }
}

// One edit to a node's element or child array as the node is copied: at `Off`, drop what was there when
// `Del` and store `Ins` when given. Both is a replacement, neither a plain copy. `Ins` is stored as it
// stands, so a child goes in with the reference the new node then holds.
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
template<typename E> Node *Alloc(Bitmap datamap, Bitmap nodemap) {
    const auto words = Words<E>(datamap, nodemap);
    Node *n;
#ifdef HAMT_AUDIT
    ++Live;
    n = static_cast<Node *>(std::malloc(words * 8));
#else
    const auto bytes = words * 8;
    auto *slab = Partial[words];
    if (!slab) {
        slab = static_cast<Slab *>(std::aligned_alloc(SlabBytes, SlabBytes));
        *slab = {nullptr, AllSlabs, nullptr, uint32_t(sizeof(Slab)), bytes, 0, true};
        Partial[words] = AllSlabs = slab;
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

// A fresh node holding `n`'s contents under the given bitmaps, with one edit to each array. Inlined so
// a known `Edit` folds its tests away, worth about 1% on the write rows.
template<typename E> [[gnu::always_inline]] Node *Rebuilt(const Node *n, Bitmap datamap, Bitmap nodemap, Edit<E> de, Edit<const Node *> ce) {
    auto *c = Alloc<E>(datamap, nodemap);
    Splice(Entries<E>(c), Entries<E>(n), DataCount(*n), de);
    Splice(Children(c), Children(n), ChildCount(*n), ce);
    return c;
}

template<typename E> Node *WithEntryInserted(const Node *n, Bitmap bit, E e) { return Rebuilt<E>(n, n->Datamap | bit, n->Nodemap, {DataOffset(*n, bit), false, &e}, {}); }
template<typename E> Node *WithEntryRemoved(const Node *n, Bitmap bit) { return Rebuilt<E>(n, n->Datamap ^ bit, n->Nodemap, {DataOffset(*n, bit), true}, {}); }

// The slot moves from the element side to the child side, for when two keys collide on it.
template<typename E> Node *WithEntryPromoted(const Node *n, Bitmap bit, const Node *child) {
    return Rebuilt<E>(n, n->Datamap ^ bit, n->Nodemap | bit, {DataOffset(*n, bit), true}, {ChildOffset(*n, bit), false, &child});
}

// The reverse, and what keeps the trie canonical: a child down to its last element folds back inline.
template<typename E> Node *WithChildInlined(const Node *n, Bitmap bit, E e) {
    return Rebuilt<E>(n, n->Datamap | bit, n->Nodemap ^ bit, {DataOffset(*n, bit), false, &e}, {ChildOffset(*n, bit), true});
}

// The two in-place rewrites, each with the copy it falls back to. Returning `n` says the write happened
// in place. Inlined like `Rebuilt`, `WithChild` the more urgently: it runs once per level of a copy.
template<typename E> [[gnu::always_inline]] const Node *WithEntry(const Node *n, uint32_t off, E e, bool owned) {
    if (!owned) return Rebuilt<E>(n, n->Datamap, n->Nodemap, {off, true, &e}, {});
    Entries<E>(Mutable(n))[off] = e;
    return n;
}

template<typename E> [[gnu::always_inline]] const Node *WithChild(const Node *n, uint32_t off, const Node *old, const Node *updated, bool owned) {
    if (updated == old) return n;
    if (!owned) return Rebuilt<E>(n, n->Datamap, n->Nodemap, {}, {off, true, &updated});
    Children(Mutable(n))[off] = updated;
    Release(old);
    return n;
}

// Two elements whose hashes agree up to `shift`, as a subtrie: one node per level they still share.
template<typename E> const Node *Merged(E a, uint64_t ha, E b, uint64_t hb, uint32_t shift) {
    assert(shift < 64); // Distinct hashes always diverge before this.
    const auto ia = Idx(ha, shift), ib = Idx(hb, shift);
    if (ia == ib) {
        auto *n = Alloc<E>(0, Bitmap{1} << ia);
        Children(n)[0] = Merged<E>(a, ha, b, hb, shift + Bits);
        return n;
    }
    auto *n = Alloc<E>((Bitmap{1} << ia) | (Bitmap{1} << ib), 0);
    auto *es = Entries<E>(n);
    es[ia < ib ? 0 : 1] = a;
    es[ia < ib ? 1 : 0] = b;
    return n;
}

struct BindResult {
    const Node *Root;
    bool Added;
};

// `owned` means every node from the root down to `n` is named exactly once, so rewriting in place is
// unobservable, and returning `n` leaves the path above untouched. `make` builds the element to store,
// given the key and the element holding it now, or null when nothing does -- where `InsertOrAssign` and
// `Update` part company. It folds away per instantiation, so `InsertOrAssign` compiles as it did before
// `Update`. The key is passed and not captured: capturing it costs 4% on a move update.
template<OnPresent P, typename E, typename F> BindResult DoBind(const Node *n, uint64_t key, F make, uint64_t hash, uint32_t shift, bool owned) {
    const auto slot = Idx(hash, shift);
    const Bitmap bit = Bitmap{1} << slot;
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        const auto old = Entries<E>(n)[off];
        if (KeyOf(old) == key) {
            // Handing `n` back is how the levels above are told nothing moved, so `Keep` allocates nothing.
            if constexpr (P == OnPresent::Keep) return {n, false};
            else return {WithEntry<E>(n, off, make(key, &old), owned), false};
        }
        return {WithEntryPromoted<E>(n, bit, Merged<E>(old, Hash(KeyOf(old)), make(key, nullptr), hash, shift + Bits)), true};
    }
    if (n->Nodemap & bit) {
        const auto off = ChildOffset(*n, bit, slot);
        const auto *child = Children(n)[off];
        const auto r = DoBind<P, E>(child, key, make, hash, shift + Bits, owned && child->Refs == 1);
        return {WithChild<E>(n, off, child, r.Root, owned), r.Added};
    }
    return {WithEntryInserted<E>(n, bit, make(key, nullptr)), true};
}

// A rebind that gives up when there is no such key, which null reports. A path is copied as the
// recursion unwinds, so a miss has allocated nothing by the time it gets back to the top.
template<typename E, typename F> const Node *DoRebind(const Node *n, uint64_t key, F make, uint64_t hash, uint32_t shift, bool owned) {
    const auto slot = Idx(hash, shift);
    const Bitmap bit = Bitmap{1} << slot;
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        const auto old = Entries<E>(n)[off];
        if (KeyOf(old) != key) return nullptr;
        return WithEntry<E>(n, off, make(key, old), owned);
    }
    if (!(n->Nodemap & bit)) return nullptr;
    const auto off = ChildOffset(*n, bit, slot);
    const auto *child = Children(n)[off];
    const auto *updated = DoRebind<E>(child, key, make, hash, shift + Bits, owned && child->Refs == 1);
    return updated ? WithChild<E>(n, off, child, updated, owned) : nullptr;
}

enum class EraseStatus {
    NotFound,
    Replaced, // `Replacement` stands in for this node.
    Singleton, // This node is down to `Lone`, which belongs inlined a level up.
    Empty, // The whole trie is gone. Only the root can report this.
};

template<typename E> struct EraseResult {
    EraseStatus Status;
    const Node *Replacement{};
    E Lone{};
};

template<typename E> EraseResult<E> DoErase(const Node *n, uint64_t key, uint64_t hash, uint32_t shift, bool owned) {
    const auto slot = Idx(hash, shift);
    const Bitmap bit = Bitmap{1} << slot;
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        if (KeyOf(Entries<E>(n)[off]) != key) return {EraseStatus::NotFound};
        const auto dc = DataCount(*n);
        // A node with a child, or elements to spare, stands on its own. Otherwise the lone survivor
        // belongs inlined in the parent, or is the whole trie when there is no parent.
        if (n->Nodemap || dc > 2) return {EraseStatus::Replaced, WithEntryRemoved<E>(n, bit)};
        if (dc == 1) return {EraseStatus::Empty}; // Only a root, since one element alone collapses.
        assert(dc == 2);
        return {EraseStatus::Singleton, nullptr, Entries<E>(n)[off ^ 1]};
    }
    if (!(n->Nodemap & bit)) return {EraseStatus::NotFound};
    const auto off = ChildOffset(*n, bit, slot);
    const auto *child = Children(n)[off];
    const auto r = DoErase<E>(child, key, hash, shift + Bits, owned && child->Refs == 1);
    if (r.Status == EraseStatus::Replaced) return {EraseStatus::Replaced, WithChild<E>(n, off, child, r.Replacement, owned)};
    if (r.Status == EraseStatus::Singleton) {
        // Keep passing it up while this node has nothing of its own to hold it. The root always has
        // two slots taken, so it is never the one passing.
        if (!n->Datamap && ChildCount(*n) == 1) return r;
        return {EraseStatus::Replaced, WithChildInlined<E>(n, bit, r.Lone)};
    }
    assert(r.Status == EraseStatus::NotFound); // A child collapses to a singleton, never to empty.
    return {EraseStatus::NotFound};
}

template<typename E> bool SameNodes(const Node *a, const Node *b) {
    if (a == b) return true; // Structure the two tries share settles a whole subtrie at once.
    if (a->Datamap != b->Datamap || a->Nodemap != b->Nodemap) return false;
    // A slot's position is fixed by the bitmap, so equal nodes hold their elements in the same order.
    if (std::memcmp(Entries<E>(a), Entries<E>(b), DataCount(*a) * sizeof(E)) != 0) return false;
    const auto *const *ca = Children(a);
    const auto *const *cb = Children(b);
    for (uint32_t i = 0, nc = ChildCount(*a); i < nc; ++i)
        if (!SameNodes<E>(ca[i], cb[i])) return false;
    return true;
}

// Where a diff reports to. Carried by reference, so nothing is allocated and the caller's callable is
// reached through one indirect call rather than a `std::function`. `ReportsChanges` says whether an
// element carries anything beside its key, so whether two sharing a key can differ at all.
struct MapSink {
    static constexpr bool ReportsChanges = true;

    void (*Report)(void *, const Change &);
    void *Context;

    // A binding only one of the two maps holds, `b`'s when `added`.
    void One(const Entry &e, bool added) const { Report(Context, {e.Key, added ? nullptr : &e.Value, added ? &e.Value : nullptr}); }
    // A key both hold, bound differently. `flip` says `e` is `b`'s and `other` is `a`'s.
    void Changed(const Entry &e, const Entry &other, bool flip) const { Report(Context, {e.Key, flip ? &other.Value : &e.Value, flip ? &e.Value : &other.Value}); }
};

// No `Changed`, since two keys that compare equal are the same element and there is nothing to report.
struct SetSink {
    static constexpr bool ReportsChanges = false;

    void (*Report)(void *, uint64_t, bool);
    void *Context;

    void One(uint64_t key, bool added) const { Report(Context, key, added); }
};

template<typename E, typename F> void ForEach(const Node *n, const F &f) {
    const auto *es = Entries<E>(n);
    for (uint32_t i = 0, dc = DataCount(*n); i < dc; ++i) f(es[i]);
    const auto *const *cs = Children(n);
    for (uint32_t i = 0, nc = ChildCount(*n); i < nc; ++i) ForEach<E>(cs[i], f);
}

// Everything under `n`, where the other trie has nothing at all: every element is a change.
template<typename E, typename S> void ReportAll(const Node *n, bool added, const S &sink) {
    ForEach<E>(n, [&](const E &e) { sink.One(e, added); });
}

// The same, minus the one slot the walk is about to follow further down.
template<typename E, typename S> void ReportAllBut(const Node *n, Bitmap keep, bool added, const S &sink) {
    const auto *es = Entries<E>(n);
    uint32_t i = 0;
    for (auto m = n->Datamap; m; m &= m - 1, ++i)
        if (Lowest(m) != keep) sink.One(es[i], added);
    const auto *const *cs = Children(n);
    uint32_t j = 0;
    for (auto m = n->Nodemap; m; m &= m - 1, ++j)
        if (Lowest(m) != keep) ReportAll<E>(cs[j], added, sink);
}

// One trie holds a single element where the other holds a whole subtrie. Everything in the subtrie is a
// change except the one element that might carry the same key. `lone_from_a` says which way round.
template<typename E, typename S> void DiffLoneAgainst(const E &lone, const Node *n, bool lone_from_a, const S &sink) {
    bool matched = false;
    ForEach<E>(n, [&](const E &e) {
        if (KeyOf(e) != KeyOf(lone)) {
            sink.One(e, lone_from_a); // The subtrie is `b`'s exactly when `lone` is `a`'s.
        } else {
            matched = true;
            if constexpr (S::ReportsChanges) {
                if (e.Value != lone.Value) sink.Changed(e, lone, lone_from_a);
            }
        }
    });
    if (!matched) sink.One(lone, !lone_from_a);
}

// Two nodes standing at the same level. Structure the tries share settles a whole subtrie at once, and
// a slot the two disagree about is one of nine cases: each side holds an element, a child, or nothing.
template<typename E, typename S> void DiffNodes(const Node *a, const Node *b, const S &sink) {
    if (a == b) return;
    for (auto rest = Bitmap(a->Datamap | a->Nodemap | b->Datamap | b->Nodemap); rest; rest &= rest - 1) {
        const auto bit = Lowest(rest);
        if (a->Datamap & bit) {
            const auto &ea = Entries<E>(a)[DataOffset(*a, bit)];
            if (b->Datamap & bit) {
                const auto &eb = Entries<E>(b)[DataOffset(*b, bit)];
                // A slot is placed by hash, so two elements sharing one need not share a key.
                if (KeyOf(ea) != KeyOf(eb)) {
                    sink.One(ea, false);
                    sink.One(eb, true);
                } else if constexpr (S::ReportsChanges) {
                    if (ea.Value != eb.Value) sink.Changed(ea, eb, false);
                }
            } else if (b->Nodemap & bit) {
                DiffLoneAgainst<E>(ea, Children(b)[ChildOffset(*b, bit)], true, sink);
            } else {
                sink.One(ea, false);
            }
        } else if (a->Nodemap & bit) {
            const auto *ca = Children(a)[ChildOffset(*a, bit)];
            if (b->Nodemap & bit) DiffNodes<E>(ca, Children(b)[ChildOffset(*b, bit)], sink);
            else if (b->Datamap & bit) DiffLoneAgainst<E>(Entries<E>(b)[DataOffset(*b, bit)], ca, false, sink);
            else ReportAll<E>(ca, false, sink);
        } else if (b->Datamap & bit) {
            sink.One(Entries<E>(b)[DataOffset(*b, bit)], true);
        } else {
            ReportAll<E>(Children(b)[ChildOffset(*b, bit)], true, sink);
        }
    }
}

// Where the shallower root ends up once it has come down to the deeper one's level: on a node there,
// or on a lone element, or on nothing at all.
template<typename E> struct Aligned {
    const Node *At{};
    const E *Lone{};
};

// Brings a root down from `from` to `to` along `prefix`. Every key in the other trie agrees on those
// bits, so nothing beside that path can be in it and all of it is reported on the way past.
template<typename E, typename S> Aligned<E> Align(const Node *n, uint32_t from, uint32_t to, uint64_t prefix, bool added, const S &sink) {
    for (auto shift = from; shift < to; shift += Bits) {
        const auto slot = Idx(prefix, shift);
        const Bitmap bit = Bitmap{1} << slot;
        ReportAllBut<E>(n, bit, added, sink);
        if (n->Nodemap & bit) {
            n = Children(n)[ChildOffset(*n, bit, slot)];
            continue;
        }
        if (n->Datamap & bit) return {nullptr, &Entries<E>(n)[DataOffset(*n, bit)]};
        return {};
    }
    return {n, nullptr};
}

// `prefix` is the low `shift` hash bits the path down to `n` has already fixed. Confirms every element
// and child sits in the slot its hash selects, and that no node is empty or collapsible.
template<typename E> bool CheckNode(const Node *n, uint32_t shift, uint64_t prefix, uint64_t &count) {
    if (n->Datamap & n->Nodemap) return false; // A slot holds an element or a child, never both.
    // Before anything reads an element: the cached counts are what place and bound the element array.
    if (n->Children != std::popcount(n->Nodemap) || n->Data != std::popcount(n->Datamap)) return false;
    const auto dc = DataCount(*n), nc = ChildCount(*n);
    if (dc + nc == 0) return false; // Empty nodes are never built, not even at the root.
    if (shift > 0 && nc == 0 && dc == 1) return false; // A lone element belongs inlined in the parent.
    count += dc;
    auto dm = n->Datamap;
    for (uint32_t i = 0; dm; dm &= dm - 1, ++i) {
        const auto hash = Hash(KeyOf(Entries<E>(n)[i]));
        if ((hash & ((uint64_t{1} << shift) - 1)) != prefix) return false;
        if (Idx(hash, shift) != uint32_t(std::countr_zero(dm))) return false;
    }
    auto nm = n->Nodemap;
    for (uint32_t i = 0; nm; nm &= nm - 1, ++i) {
        const auto slot = uint64_t(std::countr_zero(nm));
        if (!CheckNode<E>(Children(n)[i], shift + Bits, prefix | (slot << shift), count)) return false;
    }
    return true;
}

// A node with no children gets no frame. Most nodes have none, and pushing a frame only to find it
// empty and pop it is most of what visiting one costs.
template<typename E> void Descend(Iterator<E> &it, const Node *n) {
    it.Cur = Entries<E>(n);
    it.End = it.Cur + DataCount(*n);
    if (const auto nc = ChildCount(*n)) {
        const auto *const *cs = Children(n);
        it.Stack[it.Depth++] = {cs, cs + nc};
    }
}

template<typename E> void Swap(Trie<E> &a, Trie<E> &b) noexcept {
    std::swap(a.Root, b.Root);
    std::swap(a.Shift, b.Shift);
    std::swap(a.Prefix, b.Prefix);
    std::swap(a.Size, b.Size);
}
} // namespace

template<typename E> Trie<E>::Trie(const Trie &o) : Root(o.Root), Shift(o.Shift), Prefix(o.Prefix), Size(o.Size) {
    if (Root) Retain(Root);
}
// Default-initialized to empty by the member initializers, then handed `o`'s contents, leaving `o`
// empty rather than a trie whose size no longer matches the root it gave up.
template<typename E> Trie<E>::Trie(Trie &&o) noexcept { Swap(*this, o); }
template<typename E> Trie<E>::~Trie() {
    if (Root) Release(Root);
}
template<typename E> Trie<E> &Trie<E>::operator=(Trie o) {
    Swap(*this, o);
    return *this;
}

namespace {
// The one element a trie is down to, as a trie. Its root sits at shift zero, since a single key agrees
// with itself everywhere and the skipped bits would have nothing to say.
template<typename E> Trie<E> Lone(E e) {
    auto *n = Alloc<E>(Bitmap{1} << Idx(Hash(KeyOf(e)), 0), 0);
    Entries<E>(n)[0] = e;
    return {n, 1, 0, 0};
}

// `t` standing on `root` instead of its own. The two are the same node when the write happened in
// place, and then `t` is handed straight back: the reference it holds is the one the result needs.
template<typename E> Trie<E> WithRoot(Trie<E> t, const Node *root) {
    if (root == t.Root) return t;
    return {root, t.Size, t.Prefix, t.Shift};
}

// Drops every root that has one child and no elements, recording the bits it stood for. What makes it
// canonical is that the level a trie's keys first diverge at is a function of the keys.
template<typename E> void Compress(Trie<E> &t) {
    while (!t.Root->Datamap && ChildCount(*t.Root) == 1) {
        const auto *child = Children(t.Root)[0];
        Retain(child); // Claimed before the root goes, since the root holds the only reference.
        t.Prefix |= uint64_t(std::countr_zero(t.Root->Nodemap)) << t.Shift;
        t.Shift += Bits;
        Release(t.Root);
        t.Root = child;
    }
}

// The trie with `e` beside it, for a key that diverges from the prefix the root skips. Everything below
// `shift` agrees, so the levels between there and the old root become nodes at last.
template<typename E> Trie<E> Grown(Trie<E> t, E e, uint64_t hash, uint32_t shift) {
    const auto *chain = std::exchange(t.Root, nullptr); // Its reference passes to the node above it.
    for (auto s = t.Shift - Bits; s > shift; s -= Bits) {
        auto *n = Alloc<E>(0, Bitmap{1} << Idx(t.Prefix, s));
        Children(n)[0] = chain;
        chain = n;
    }
    auto *root = Alloc<E>(Bitmap{1} << Idx(hash, shift), Bitmap{1} << Idx(t.Prefix, shift));
    Entries<E>(root)[0] = e;
    Children(root)[0] = chain;
    return {root, t.Size + 1, t.Prefix & ((uint64_t{1} << shift) - 1), shift};
}

// The whole of a write that can add a key. `InsertOrAssign` reaches it with a constant and `Update` with
// a function of the binding it displaces, and nothing from here down knows the difference.
template<OnPresent P, typename E, typename F> Trie<E> BindWith(Trie<E> t, uint64_t key, F make) {
    const auto hash = Hash(key);
    if (!t.Root) return Lone(make(key, nullptr));
    // A key that disagrees over the bits the root skips belongs above the root, not under it, so it is
    // one the trie cannot already hold and `Keep` has nothing to say about it.
    if (const auto diff = (hash ^ t.Prefix) & ((uint64_t{1} << t.Shift) - 1)) return Grown(std::move(t), make(key, nullptr), hash, uint32_t(std::countr_zero(diff)) / Bits * Bits);
    const auto r = DoBind<P, E>(t.Root, key, make, hash, t.Shift, t.Root->Refs == 1);
    auto out = WithRoot(std::move(t), r.Root);
    out.Size += r.Added;
    // Promoting the element of a trie that held only one leaves a root with a single child, and the
    // subtrie under it is a chain until the two keys diverge. Nothing else here can build one.
    if (out.Size == 2) Compress(out);
    return out;
}

// What one slot of a bulk build came to: a subtrie, or the single element it collapsed to. A slot with
// one element is inlined by the parent and never gets a node, which keeps a repeated key canonical.
template<typename E> struct Piece {
    const Node *Child; // Null when the slot is a lone element.
    E Lone;
};

// The subtrie for `n` elements agreeing on the low `shift` hash bits. They are partitioned by slot into
// `to`, and `from` becomes the scratch for the level below, so the two buffers alternate all the way
// down and nothing but the nodes themselves is allocated.
template<typename E> Piece<E> Assemble(E *from, E *to, uint64_t n, uint32_t shift, uint64_t &placed) {
    if (n == 1) {
        ++placed;
        return {nullptr, from[0]};
    }
    // Agreeing on all 64 bits is agreeing on the key, since the fold is a bijection. So these are
    // repeats of one key, and the last of them wins as it would through a repeated `InsertOrAssign`.
    if (shift >= 64) {
        ++placed;
        return {nullptr, from[n - 1]};
    }
    uint64_t counts[Slots]{}, start[Slots], cursor[Slots];
    for (uint64_t i = 0; i < n; ++i) ++counts[Idx(Hash(KeyOf(from[i])), shift)];
    for (uint32_t s = 0, running = 0; s < Slots; ++s) {
        start[s] = cursor[s] = running;
        running += counts[s];
    }
    // Stable, so that the last repeat of a key is still the last one when its slot is read back.
    for (uint64_t i = 0; i < n; ++i) to[cursor[Idx(Hash(KeyOf(from[i])), shift)]++] = from[i];

    Bitmap datamap = 0, nodemap = 0;
    E entries[Slots];
    const Node *children[Slots];
    uint32_t ne = 0, nc = 0;
    for (uint32_t s = 0; s < Slots; ++s) {
        if (!counts[s]) continue;
        const auto piece = Assemble<E>(to + start[s], from + start[s], counts[s], shift + Bits, placed);
        if (piece.Child) {
            nodemap |= Bitmap{1} << s;
            children[nc++] = piece.Child;
        } else {
            datamap |= Bitmap{1} << s;
            entries[ne++] = piece.Lone;
        }
    }
    // Everything here was one key repeated, so there is no node to build after all.
    if (nc == 0 && ne == 1) return {nullptr, entries[0]};
    auto *node = Alloc<E>(datamap, nodemap);
    std::memcpy(Entries<E>(node), entries, ne * sizeof(E));
    std::memcpy(static_cast<void *>(Children(node)), static_cast<const void *>(children), nc * sizeof(const Node *));
    return {node, {}};
}
} // namespace

template<typename E> Trie<E> Build(const E *first, const E *last) {
    const auto n = uint64_t(last - first);
    if (!n) return {};
    // One allocation for both halves of the ping-pong, and the elements copied into the first, since
    // the partition reorders what it is given and the caller's array is not ours to disturb.
    auto *scratch = static_cast<E *>(std::malloc(2 * n * sizeof(E)));
    std::memcpy(scratch, first, n * sizeof(E));
    uint64_t placed = 0;
    const auto piece = Assemble<E>(scratch, scratch + n, n, 0, placed);
    std::free(scratch);
    if (!piece.Child) return Lone(piece.Lone); // Every element carried the same key.
    Trie<E> t{piece.Child, placed, 0, 0};
    Compress(t); // Keys that agree on their low bits leave a chain above where they diverge.
    return t;
}

Map InsertOrAssign(Map m, uint64_t key, uint64_t value) {
    return BindWith<OnPresent::Rebind>(std::move(m), key, [value](uint64_t k, const Entry *) { return Entry{k, value}; });
}

Set Insert(Set s, uint64_t key) {
    return BindWith<OnPresent::Keep>(std::move(s), key, [](uint64_t k, const uint64_t *) { return k; });
}

Map Update(Map m, uint64_t key, uint64_t (*fn)(void *, uint64_t), void *context) {
    return BindWith<OnPresent::Rebind>(std::move(m), key, [fn, context](uint64_t k, const Entry *current) { return Entry{k, fn(context, current ? current->Value : 0)}; });
}

Map UpdateIfExists(Map m, uint64_t key, uint64_t (*fn)(void *, uint64_t), void *context) {
    if (!m.Root) return m;
    // Nothing tests the bits the root skips, for the reason `Get` gives: a key that disagrees on them
    // lands on some other key and fails the comparison it was going to make anyway.
    const auto *root = DoRebind<Entry>(m.Root, key, [fn, context](uint64_t k, const Entry &current) { return Entry{k, fn(context, current.Value)}; }, Hash(key), m.Shift, m.Root->Refs == 1);
    if (!root) return m; // No such key, and no node copied on the way to finding that out.
    // A rebind leaves the shape alone, so the size stands and there is nothing to compress.
    return WithRoot(std::move(m), root);
}

template<typename E> Trie<E> Erase(Trie<E> t, uint64_t key) {
    if (!t.Root) return t;
    const auto r = DoErase<E>(t.Root, key, Hash(key), t.Shift, t.Root->Refs == 1);
    if (r.Status == EraseStatus::Replaced) {
        auto out = WithRoot(std::move(t), r.Replacement);
        --out.Size;
        Compress(out); // A root down to its last child skips the level it stood on.
        return out;
    }
    if (r.Status == EraseStatus::Empty) return {};
    if (r.Status == EraseStatus::Singleton) return Lone(r.Lone); // Only the root reports one, and only from two elements.
    assert(r.Status == EraseStatus::NotFound);
    return t;
}

namespace {
// The element holding `key`, or null.
template<typename E> const E *Find(const Trie<E> &t, uint64_t key) {
    const auto *n = t.Root;
    if (!n) return nullptr;
    // Shifting the hash costs an instruction less per level than indexing it at a running shift. The loop
    // has no condition because a slot holding a child is never null, so the walk ends by returning, and
    // the child map is tested first because most levels descend. A key disagreeing over the bits the root
    // skips needs no test: the fold is a bijection, so whatever the walk lands on is a different key.
    for (auto hash = Hash(key) >> t.Shift;; hash >>= Bits) {
        const auto slot = uint32_t(hash & Mask);
        const Bitmap bit = Bitmap{1} << slot;
        if (n->Nodemap & bit) {
            n = Children(n)[ChildOffset(*n, bit, slot)];
            continue;
        }
        if (!(n->Datamap & bit)) return nullptr;
        const auto *e = &Entries<E>(n)[DataOffset(*n, bit)];
        return KeyOf(*e) == key ? e : nullptr;
    }
}
} // namespace

const uint64_t *Get(const Map &m, uint64_t key) {
    const auto *e = Find(m, key);
    return e ? &e->Value : nullptr;
}

bool Contains(const Set &s, uint64_t key) { return Find(s, key) != nullptr; }

template<typename E> void ForEachChunk(const Trie<E> &t, void (*visit)(void *, const E *, const E *), void *context) {
    if (!t.Root) return; // An empty trie has no root, and so calls nothing.
    // A node's elements, then each child's, the order the iterator takes too. The stack is explicit rather
    // than the call stack's, so nothing recurses and the whole walk inlines into the caller -- which turns
    // `visit` from a call through a pointer into the caller's own loop body. Worth 20% at a thousand keys.
    typename Iterator<E>::Frame stack[Iterator<E>::MaxDepth];
    uint32_t depth = 0;
    for (const auto *n = t.Root;;) {
        if (const auto dc = DataCount(*n)) visit(context, Entries<E>(n), Entries<E>(n) + dc);
        if (const auto nc = ChildCount(*n)) stack[depth++] = {Children(n), Children(n) + nc};
        while (depth && stack[depth - 1].Child == stack[depth - 1].ChildEnd) --depth;
        if (!depth) return;
        n = *stack[depth - 1].Child++;
    }
}

template<typename E> bool operator==(const Trie<E> &a, const Trie<E> &b) {
    if (a.Root == b.Root) return true; // Two empty tries, or two that share a root outright.
    if (!a.Root || !b.Root) return false; // Only an empty trie has a null root, so one of them is empty.
    // The size test is an early out and nothing more: `SameNodes` would reject a mismatch anyway.
    return a.Size == b.Size && SameNodes<E>(a.Root, b.Root);
}

namespace {
template<typename E, typename S> void DiffTries(const Trie<E> &a, const Trie<E> &b, const S &sink) {
    if (a.Root == b.Root) return; // Two empty tries, or two that share a root outright.
    if (!a.Root) return ReportAll<E>(b.Root, true, sink);
    if (!b.Root) return ReportAll<E>(a.Root, false, sink);
    // Each root stands above the bits its own keys agree on, so the two can sit at different levels.
    const auto shared = a.Shift < b.Shift ? a.Shift : b.Shift;
    if ((a.Prefix ^ b.Prefix) & ((uint64_t{1} << shared) - 1)) {
        // They disagree inside bits that both tries hold fixed, so they have no key in common.
        ReportAll<E>(a.Root, false, sink);
        return ReportAll<E>(b.Root, true, sink);
    }
    if (a.Shift == b.Shift) return DiffNodes<E>(a.Root, b.Root, sink);
    // The shallower root has to come down to the deeper one before the two can be walked in step.
    // `deeper` says which trie that is, and so which side of every report the shallow one is on.
    const bool deeper = b.Shift > a.Shift;
    const Trie<E> &deep = deeper ? b : a, &shallow = deeper ? a : b;
    const auto aligned = Align<E>(shallow.Root, shared, deep.Shift, deep.Prefix, !deeper, sink);
    if (aligned.At) return deeper ? DiffNodes<E>(aligned.At, deep.Root, sink) : DiffNodes<E>(deep.Root, aligned.At, sink);
    if (aligned.Lone) return DiffLoneAgainst<E>(*aligned.Lone, deep.Root, deeper, sink);
    ReportAll<E>(deep.Root, deeper, sink);
}
} // namespace

void Diff(const Map &a, const Map &b, void (*report)(void *, const Change &), void *context) { DiffTries(a, b, MapSink{report, context}); }

void Diff(const Set &a, const Set &b, void (*report)(void *, uint64_t, bool), void *context) { DiffTries(a, b, SetSink{report, context}); }

template<typename E> bool Check(const Trie<E> &t) {
    if (!t.Root) return t.Size == 0 && t.Shift == 0 && t.Prefix == 0;
    if (t.Shift >= 64 || t.Shift % Bits) return false; // The root stands on a level boundary.
    if (t.Prefix >> t.Shift) return false; // It holds the skipped bits and nothing above them.
    // A root with one child and no elements is a level the trie should have skipped instead.
    if (!t.Root->Datamap && ChildCount(*t.Root) == 1) return false;
    uint64_t count = 0;
    return CheckNode<E>(t.Root, t.Shift, t.Prefix, count) && count == t.Size;
}

std::optional<uint64_t> LiveNodes() {
#ifdef HAMT_AUDIT
    return Live;
#else
    return {};
#endif
}

std::optional<Footprint> Held() {
#ifdef HAMT_AUDIT
    return {};
#else
    Footprint f{};
    for (const auto *slab = AllSlabs; slab; slab = slab->All) {
        f.ReservedBytes += SlabBytes;
        if (!slab->Live) continue;
        f.LiveBytes += uint64_t(slab->Live) * slab->Bytes;
        f.SpannedBytes += SlabBytes;
    }
    return f;
#endif
}

template<typename E> void Iterator<E>::Advance() {
    // Both ends of the frame are read into locals first: read through the frame instead and clang moves
    // the pair through a vector register, which lands on the path between nodes.
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

template<typename E> Iterator<E> begin(const Trie<E> &t) {
    Iterator<E> it{t}; // The copy is the reference that holds the structure below still.
    if (!t.Root) return it;
    Descend(it, t.Root);
    if (it.Cur == it.End) it.Advance(); // A root holding only children has nothing to yield yet.
    return it;
}

// Both tries are compiled here and nowhere else: a header that offered them would have to offer the
// node, and the node is this file's own.
template struct Trie<Entry>;
template struct Trie<uint64_t>;
template struct Iterator<Entry>;
template struct Iterator<uint64_t>;
template Map Erase(Map, uint64_t);
template Set Erase(Set, uint64_t);
template Map Build(const Entry *, const Entry *);
template Set Build(const uint64_t *, const uint64_t *);
template bool operator==(const Map &, const Map &);
template bool operator==(const Set &, const Set &);
template bool Check(const Map &);
template bool Check(const Set &);
template void ForEachChunk(const Map &, void (*)(void *, const Entry *, const Entry *), void *);
template void ForEachChunk(const Set &, void (*)(void *, const uint64_t *, const uint64_t *), void *);
template Iterator<Entry> begin(const Map &);
template Iterator<uint64_t> begin(const Set &);
} // namespace hamt
