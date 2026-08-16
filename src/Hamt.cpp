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

// Fold the key, do not scramble it. A dense key set is the best case a trie has -- every node full,
// every key at the same depth -- and counters, object ids and row numbers hand that over for free,
// which any good mixer would destroy. A fold keeps it, since each xor-shift is a bijection on
// `[0, 2^k)`, while still filling low bits that never vary from bits that do.
//
// The fold being a bijection also means distinct keys never share a hash, so the trie always
// separates them before it bottoms out and there are no collision nodes.
//
// One fold and no more, and the reason is subtler than the invariant above. A counter is dense on an
// interval, but a heap address is dense on a *stride*, and a stride is not an interval: folding bits
// 16 and up into the low four leaves such a set a permutation of itself yet no longer a lattice, and
// a trie that was going to be full is scattered instead -- four times the nodes for the same entries,
// on the very key shape the second fold was meant to protect. Keys whose low bits never vary, which
// is what it did protect against, cost one level at the root and are handled by `Map::Shift` instead.
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
// slab until it is full. The header is 16-byte aligned and a whole number of pairs of words wide, so
// the nodes cut behind it keep the alignment the slab itself has.
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
// `value` says what to store, given the value the key is bound to now or null when it is bound to
// nothing, and is where `Set` and `Update` part company: one ignores its argument and the other is a
// function of it. It folds away into each instantiation, so `Set` compiles to the instructions it
// did before `Update` existed.
template<typename F> SetResult DoSet(const Node *n, uint64_t key, F value, uint64_t hash, uint32_t shift, bool owned) {
    const Bitmap bit = Bitmap{1} << Idx(hash, shift);
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        const auto old = Entries(n)[off];
        if (old.Key == key) return {WithEntry(n, off, {key, value(&old.Value)}, owned), false};
        return {WithEntryPromoted(n, bit, Merged(old, Hash(old.Key), {key, value(nullptr)}, hash, shift + Bits)), true};
    }
    if (n->Nodemap & bit) {
        const auto off = ChildOffset(*n, bit);
        const auto *child = Children(n)[off];
        const auto r = DoSet(child, key, value, hash, shift + Bits, owned && child->Refs == 1);
        return {WithChild(n, off, child, r.Root, owned), r.Added};
    }
    return {WithEntryInserted(n, bit, {key, value(nullptr)}), true};
}

// A rebind that gives up when there is no such key, which null reports. The copies a path costs are
// made as the recursion unwinds, so a miss has allocated nothing by the time it gets back to the top.
template<typename F> const Node *DoRebind(const Node *n, uint64_t key, F value, uint64_t hash, uint32_t shift, bool owned) {
    const Bitmap bit = Bitmap{1} << Idx(hash, shift);
    if (n->Datamap & bit) {
        const auto off = DataOffset(*n, bit);
        const auto old = Entries(n)[off];
        if (old.Key != key) return nullptr;
        return WithEntry(n, off, {key, value(old.Value)}, owned);
    }
    if (!(n->Nodemap & bit)) return nullptr;
    const auto off = ChildOffset(*n, bit);
    const auto *child = Children(n)[off];
    const auto *updated = DoRebind(child, key, value, hash, shift + Bits, owned && child->Refs == 1);
    return updated ? WithChild(n, off, child, updated, owned) : nullptr;
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
        // belongs inlined in the parent, or is the whole map when there is no parent.
        if (n->Nodemap || dc > 2) return {EraseStatus::Replaced, WithEntryRemoved(n, bit)};
        if (dc == 1) return {EraseStatus::Empty}; // Only a root, since one entry alone collapses.
        assert(dc == 2);
        return {EraseStatus::Singleton, nullptr, Entries(n)[off ^ 1]};
    }
    if (!(n->Nodemap & bit)) return {EraseStatus::NotFound};
    const auto off = ChildOffset(*n, bit);
    const auto *child = Children(n)[off];
    const auto r = DoErase(child, key, hash, shift + Bits, owned && child->Refs == 1);
    if (r.Status == EraseStatus::Replaced) return {EraseStatus::Replaced, WithChild(n, off, child, r.Replacement, owned)};
    if (r.Status == EraseStatus::Singleton) {
        // Keep passing it up while this node has nothing of its own to hold it. The root always has
        // two slots taken, so it is never the one passing.
        if (!n->Datamap && ChildCount(*n) == 1) return r;
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

// Where a diff reports to. Carried by reference through the walk, so nothing is allocated and the
// caller's callable is reached through one indirect call rather than a `std::function`.
struct Sink {
    void (*Report)(void *, const Change &);
    void *Context;

    void operator()(uint64_t key, const uint64_t *before, const uint64_t *after) const { Report(Context, {key, before, after}); }
};

template<typename F> void ForEach(const Node *n, const F &f) {
    const auto *es = Entries(n);
    for (uint32_t i = 0, dc = DataCount(*n); i < dc; ++i) f(es[i]);
    const auto *const *cs = Children(n);
    for (uint32_t i = 0, nc = ChildCount(*n); i < nc; ++i) ForEach(cs[i], f);
}

// Everything under `n`, where the other map has nothing at all: every binding is a change.
void ReportAll(const Node *n, bool added, const Sink &sink) {
    ForEach(n, [&](const Entry &e) { sink(e.Key, added ? nullptr : &e.Value, added ? &e.Value : nullptr); });
}

// The same, minus the one slot the walk is about to follow further down.
void ReportAllBut(const Node *n, Bitmap keep, bool added, const Sink &sink) {
    const auto *es = Entries(n);
    uint32_t i = 0;
    for (auto m = n->Datamap; m; m &= m - 1, ++i)
        if ((Bitmap{1} << std::countr_zero(m)) != keep) sink(es[i].Key, added ? nullptr : &es[i].Value, added ? &es[i].Value : nullptr);
    const auto *const *cs = Children(n);
    uint32_t j = 0;
    for (auto m = n->Nodemap; m; m &= m - 1, ++j)
        if ((Bitmap{1} << std::countr_zero(m)) != keep) ReportAll(cs[j], added, sink);
}

// One map holds a single entry where the other holds a whole subtrie. Everything in the subtrie is a
// change except the one binding that might carry the same key. `lone_from_a` says which way round.
void DiffLoneAgainst(const Entry &lone, const Node *n, bool lone_from_a, const Sink &sink) {
    bool matched = false;
    ForEach(n, [&](const Entry &e) {
        if (e.Key != lone.Key) {
            sink(e.Key, lone_from_a ? nullptr : &e.Value, lone_from_a ? &e.Value : nullptr);
        } else {
            matched = true;
            if (e.Value != lone.Value) sink(e.Key, lone_from_a ? &lone.Value : &e.Value, lone_from_a ? &e.Value : &lone.Value);
        }
    });
    if (!matched) sink(lone.Key, lone_from_a ? &lone.Value : nullptr, lone_from_a ? nullptr : &lone.Value);
}

// Two nodes standing at the same level. Structure the maps share settles a whole subtrie at once, and
// a slot the two disagree about is one of nine cases: each side holds an entry, a child, or nothing.
void DiffNodes(const Node *a, const Node *b, const Sink &sink) {
    if (a == b) return;
    for (auto rest = Bitmap(a->Datamap | a->Nodemap | b->Datamap | b->Nodemap); rest; rest &= rest - 1) {
        const Bitmap bit = Bitmap{1} << std::countr_zero(rest);
        if (a->Datamap & bit) {
            const auto &ea = Entries(a)[DataOffset(*a, bit)];
            if (b->Datamap & bit) {
                const auto &eb = Entries(b)[DataOffset(*b, bit)];
                // A slot is placed by hash, so two entries sharing one need not share a key.
                if (ea.Key != eb.Key) {
                    sink(ea.Key, &ea.Value, nullptr);
                    sink(eb.Key, nullptr, &eb.Value);
                } else if (ea.Value != eb.Value) {
                    sink(ea.Key, &ea.Value, &eb.Value);
                }
            } else if (b->Nodemap & bit) {
                DiffLoneAgainst(ea, Children(b)[ChildOffset(*b, bit)], true, sink);
            } else {
                sink(ea.Key, &ea.Value, nullptr);
            }
        } else if (a->Nodemap & bit) {
            const auto *ca = Children(a)[ChildOffset(*a, bit)];
            if (b->Nodemap & bit) DiffNodes(ca, Children(b)[ChildOffset(*b, bit)], sink);
            else if (b->Datamap & bit) DiffLoneAgainst(Entries(b)[DataOffset(*b, bit)], ca, false, sink);
            else ReportAll(ca, false, sink);
        } else if (b->Datamap & bit) {
            const auto &eb = Entries(b)[DataOffset(*b, bit)];
            sink(eb.Key, nullptr, &eb.Value);
        } else {
            ReportAll(Children(b)[ChildOffset(*b, bit)], true, sink);
        }
    }
}

// Where the shallower root ends up once it has come down to the deeper one's level: on a node there,
// or on a lone entry, or on nothing at all.
struct Aligned {
    const Node *At{};
    const Entry *Lone{};
};

// Brings a root down from `from` to `to` along `prefix`. Every key in the other map agrees on those
// bits, so nothing beside that path can be in it and all of it is reported on the way past.
Aligned Align(const Node *n, uint32_t from, uint32_t to, uint64_t prefix, bool added, const Sink &sink) {
    for (auto shift = from; shift < to; shift += Bits) {
        const Bitmap bit = Bitmap{1} << Idx(prefix, shift);
        ReportAllBut(n, bit, added, sink);
        if (n->Nodemap & bit) {
            n = Children(n)[ChildOffset(*n, bit)];
            continue;
        }
        if (n->Datamap & bit) return {nullptr, &Entries(n)[DataOffset(*n, bit)]};
        return {};
    }
    return {n, nullptr};
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

Map::Map(const Map &o) : Root(o.Root), Shift(o.Shift), Prefix(o.Prefix), Size(o.Size) {
    if (Root) Retain(Root);
}
Map::Map(Map &&o) noexcept : Root(std::exchange(o.Root, nullptr)), Shift(std::exchange(o.Shift, 0)), Prefix(std::exchange(o.Prefix, 0)), Size(std::exchange(o.Size, 0)) {}
Map::~Map() {
    if (Root) Release(Root);
}
Map &Map::operator=(Map o) {
    std::swap(Root, o.Root);
    std::swap(Shift, o.Shift);
    std::swap(Prefix, o.Prefix);
    std::swap(Size, o.Size);
    return *this;
}

namespace {
// The one entry a map is down to, as a map. Its root sits at shift zero, since a single key agrees
// with itself everywhere and the skipped bits would have nothing to say.
Map Lone(Entry e) {
    auto *n = Alloc(Bitmap{1} << Idx(Hash(e.Key), 0), 0);
    Entries(n)[0] = e;
    return {n, 1, 0, 0};
}

// Drops every root that has one child and no entries, recording the bits it stood for. What makes it
// canonical is that the level a map's keys first diverge at is a function of the keys.
void Compress(Map &m) {
    while (!m.Root->Datamap && ChildCount(*m.Root) == 1) {
        const auto *child = Children(m.Root)[0];
        Retain(child); // Claimed before the root goes, since the root is what holds it.
        m.Prefix |= uint64_t(std::countr_zero(m.Root->Nodemap)) << m.Shift;
        m.Shift += Bits;
        Release(m.Root);
        m.Root = child;
    }
}

// The map with `e` beside it, for a key that diverges from the prefix the root skips. Everything
// below `shift` agrees, so the levels between there and the old root become nodes at last: a walk
// coming down through the new branch has to be told the way.
Map Grown(Map m, Entry e, uint64_t hash, uint32_t shift) {
    const auto *chain = std::exchange(m.Root, nullptr); // Its reference passes to the node above it.
    for (auto s = m.Shift - Bits; s > shift; s -= Bits) {
        auto *n = Alloc(0, Bitmap{1} << Idx(m.Prefix, s));
        Children(n)[0] = chain;
        chain = n;
    }
    auto *root = Alloc(Bitmap{1} << Idx(hash, shift), Bitmap{1} << Idx(m.Prefix, shift));
    Entries(root)[0] = e;
    Children(root)[0] = chain;
    return {root, m.Size + 1, m.Prefix & ((uint64_t{1} << shift) - 1), shift};
}

// The whole of a write that can add a key. `Set` reaches it with a constant and `Update` with a
// function of the binding it displaces, and nothing from here down knows the difference.
template<typename F> Map SetWith(Map m, uint64_t key, F value) {
    const auto hash = Hash(key);
    if (!m.Root) return Lone({key, value(nullptr)});
    // A key that disagrees over the bits the root skips belongs above the root, not under it.
    if (const auto diff = (hash ^ m.Prefix) & ((uint64_t{1} << m.Shift) - 1)) return Grown(std::move(m), {key, value(nullptr)}, hash, uint32_t(std::countr_zero(diff)) / Bits * Bits);
    const auto r = DoSet(m.Root, key, value, hash, m.Shift, m.Root->Refs == 1);
    // A root rewritten in place is still the one `m` holds, so hand that reference on.
    Map out = r.Root == m.Root ? std::move(m) : Map{r.Root, m.Size, m.Prefix, m.Shift};
    out.Size += r.Added;
    // Promoting the entry of a map that held only one leaves a root with a single child, and the
    // subtrie under it is a chain until the two keys diverge. Nothing else here can build one.
    if (out.Size == 2) Compress(out);
    return out;
}

// What one slot of a bulk build came to: a subtrie, or the single binding it collapsed to. A slot
// holding one binding is inlined by the parent and never gets a node, which is what keeps the result
// canonical when the same key turns up more than once.
struct Piece {
    const Node *Child; // Null when the slot is a lone entry.
    Entry Lone;
};

// The subtrie for `n` entries that agree on the low `shift` hash bits. They are partitioned by slot
// into `to`, and `from` becomes the scratch the level below partitions into, so the two buffers
// alternate all the way down and nothing else is allocated but the nodes themselves.
Piece Assemble(Entry *from, Entry *to, uint64_t n, uint32_t shift, uint64_t &placed) {
    if (n == 1) {
        ++placed;
        return {nullptr, from[0]};
    }
    // Agreeing on all 64 bits is agreeing on the key, since the fold is a bijection. So these are
    // repeats of one key, and the last of them wins as it would through a repeated `Set`.
    if (shift >= 64) {
        ++placed;
        return {nullptr, from[n - 1]};
    }
    constexpr uint32_t Slots = Mask + 1;
    uint64_t counts[Slots]{}, start[Slots], cursor[Slots];
    for (uint64_t i = 0; i < n; ++i) ++counts[Idx(Hash(from[i].Key), shift)];
    for (uint32_t s = 0, running = 0; s < Slots; ++s) {
        start[s] = cursor[s] = running;
        running += counts[s];
    }
    // Stable, so that the last repeat of a key is still the last one when its slot is read back.
    for (uint64_t i = 0; i < n; ++i) to[cursor[Idx(Hash(from[i].Key), shift)]++] = from[i];

    Bitmap datamap = 0, nodemap = 0;
    Entry entries[Slots];
    const Node *children[Slots];
    uint32_t ne = 0, nc = 0;
    for (uint32_t s = 0; s < Slots; ++s) {
        if (!counts[s]) continue;
        const auto piece = Assemble(to + start[s], from + start[s], counts[s], shift + Bits, placed);
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
    auto *node = Alloc(datamap, nodemap);
    std::memcpy(Entries(node), entries, ne * sizeof(Entry));
    std::memcpy(static_cast<void *>(Children(node)), static_cast<const void *>(children), nc * sizeof(const Node *));
    return {node, {}};
}
} // namespace

Map Build(const Entry *first, const Entry *last) {
    const auto n = uint64_t(last - first);
    if (!n) return {};
    // One allocation for both halves of the ping-pong, and the entries copied into the first, since
    // the partition reorders what it is given and the caller's array is not ours to disturb.
    auto *scratch = static_cast<Entry *>(std::malloc(2 * n * sizeof(Entry)));
    std::memcpy(scratch, first, n * sizeof(Entry));
    uint64_t placed = 0;
    const auto piece = Assemble(scratch, scratch + n, n, 0, placed);
    std::free(scratch);
    if (!piece.Child) return Lone(piece.Lone); // Every entry carried the same key.
    Map m{piece.Child, placed, 0, 0};
    Compress(m); // Keys that agree on their low bits leave a chain above where they diverge.
    return m;
}

Map Set(Map m, uint64_t key, uint64_t value) {
    return SetWith(std::move(m), key, [value](const uint64_t *) { return value; });
}

Map Update(Map m, uint64_t key, uint64_t (*fn)(void *, uint64_t), void *context) {
    return SetWith(std::move(m), key, [fn, context](const uint64_t *current) { return fn(context, current ? *current : 0); });
}

Map UpdateIfExists(Map m, uint64_t key, uint64_t (*fn)(void *, uint64_t), void *context) {
    if (!m.Root) return m;
    // Nothing tests the bits the root skips, for the reason `Get` gives: a key that disagrees on them
    // lands on some other key and fails the comparison it was going to make anyway.
    const auto *root = DoRebind(m.Root, key, [fn, context](uint64_t current) { return fn(context, current); }, Hash(key), m.Shift, m.Root->Refs == 1);
    if (!root) return m; // No such key, and no node copied on the way to finding that out.
    // A rebind leaves the shape alone, so the size stands and there is nothing to compress.
    return root == m.Root ? std::move(m) : Map{root, m.Size, m.Prefix, m.Shift};
}

Map Erase(Map m, uint64_t key) {
    if (!m.Root) return m;
    const auto r = DoErase(m.Root, key, Hash(key), m.Shift, m.Root->Refs == 1);
    if (r.Status == EraseStatus::Replaced) {
        Map out = r.Replacement == m.Root ? std::move(m) : Map{r.Replacement, m.Size, m.Prefix, m.Shift};
        --out.Size;
        Compress(out); // A root down to its last child skips the level it stood on.
        return out;
    }
    if (r.Status == EraseStatus::Empty) return {};
    if (r.Status == EraseStatus::Singleton) return Lone(r.Lone); // Only the root reports one, and only from two entries.
    assert(r.Status == EraseStatus::NotFound);
    return m;
}

const uint64_t *Get(const Map &m, uint64_t key) {
    const auto *n = m.Root;
    if (!n) return nullptr;
    // Shifting the hash costs an instruction less per level than indexing it at a running shift. The
    // loop has no condition because a slot holding a child is never null, so the walk ends by
    // returning. The child map is tested first because most levels descend, and the maps are disjoint,
    // so either order is correct.
    // A key that disagrees over the bits the root skips needs no test of its own: every key in the
    // map agrees on them, the fold is a bijection, so whatever the walk lands on is a different key.
    for (auto hash = Hash(key) >> m.Shift;; hash >>= Bits) {
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

void ForEachChunk(const Map &m, void (*visit)(void *, const Entry *, const Entry *), void *context) {
    if (!m.Root) return; // An empty map has no root, and so calls nothing.
    // A node's entries, then each child's, which is the order the iterator takes as well. The stack
    // is explicit rather than the call stack's, so that nothing here recurses and the whole walk can
    // be inlined into the caller -- which is what turns `visit` from a call through a pointer into
    // the caller's own loop body. Worth 20% at a thousand keys, where nothing waits on memory and
    // the call was a real share of the work.
    Iterator::Frame stack[Iterator::MaxDepth];
    uint32_t depth = 0;
    for (const auto *n = m.Root;;) {
        if (const auto dc = DataCount(*n)) visit(context, Entries(n), Entries(n) + dc);
        if (const auto nc = ChildCount(*n)) stack[depth++] = {Children(n), Children(n) + nc};
        while (depth && stack[depth - 1].Child == stack[depth - 1].ChildEnd) --depth;
        if (!depth) return;
        n = *stack[depth - 1].Child++;
    }
}

bool operator==(const Map &a, const Map &b) {
    if (a.Root == b.Root) return true; // Two empty maps, or two that share a root outright.
    if (!a.Root || !b.Root) return false; // Only an empty map has a null root, so one of them is empty.
    // The size test is an early out and nothing more: `SameNodes` would reject a mismatch anyway.
    return a.Size == b.Size && SameNodes(a.Root, b.Root);
}

void Diff(const Map &a, const Map &b, void (*report)(void *, const Change &), void *context) {
    const Sink sink{report, context};
    if (a.Root == b.Root) return; // Two empty maps, or two that share a root outright.
    if (!a.Root) return ReportAll(b.Root, true, sink);
    if (!b.Root) return ReportAll(a.Root, false, sink);
    // Each root stands above the bits its own keys agree on, so the two can sit at different levels.
    const auto shared = a.Shift < b.Shift ? a.Shift : b.Shift;
    if ((a.Prefix ^ b.Prefix) & ((uint64_t{1} << shared) - 1)) {
        // They disagree inside bits that both maps hold fixed, so they have no key in common.
        ReportAll(a.Root, false, sink);
        return ReportAll(b.Root, true, sink);
    }
    if (a.Shift == b.Shift) return DiffNodes(a.Root, b.Root, sink);
    // The shallower root has to come down to the deeper one before the two can be walked in step.
    const bool deeper = b.Shift > a.Shift;
    const auto *deep = deeper ? b.Root : a.Root;
    const auto aligned = Align(deeper ? a.Root : b.Root, shared, deeper ? b.Shift : a.Shift, deeper ? b.Prefix : a.Prefix, !deeper, sink);
    if (aligned.At) return deeper ? DiffNodes(aligned.At, deep, sink) : DiffNodes(deep, aligned.At, sink);
    if (aligned.Lone) return DiffLoneAgainst(*aligned.Lone, deep, deeper, sink);
    ReportAll(deep, deeper, sink);
}

bool Check(const Map &m) {
    if (!m.Root) return m.Size == 0 && m.Shift == 0 && m.Prefix == 0;
    if (m.Shift >= 64 || m.Shift % Bits) return false; // The root stands on a level boundary.
    if (m.Prefix >> m.Shift) return false; // It holds the skipped bits and nothing above them.
    // A root with one child and no entries is a level the map should have skipped instead.
    if (!m.Root->Datamap && ChildCount(*m.Root) == 1) return false;
    uint64_t count = 0;
    return CheckNode(m.Root, m.Shift, m.Prefix, count) && count == m.Size;
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
