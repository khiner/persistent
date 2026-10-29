#include "Vector.h"

#include "Node.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

namespace vec {
static_assert(Iterator::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

namespace {
constexpr bool Relaxed(Shape s) { return s == Shape::Relaxed; }
template<typename W> constexpr bool HasWeight = !std::is_same_v<W, Unweighted>;

template<typename W> uint64_t LeafWeight(const Node *n, uint64_t count) {
    uint64_t weight = 0;
    for (uint64_t i = 0; i < count; ++i) weight += W::Of(Values(n)[i]);
    return weight;
}
template<typename W> uint64_t SubtreeWeight(const Node *n, uint32_t shift) {
    if (shift) return Values(n->Weights)[n->Count - 1];
    return LeafWeight<W>(n, n->Count);
}

template<typename W> void MeasureWeights(Node *c, uint32_t shift) {
    if constexpr (HasWeight<W>) {
        if (c->Weights) Release(c->Weights, 0);
        auto *table = Alloc(c->Count);
        uint64_t total = 0;
        for (uint32_t i = 0, nc = c->Count; i < nc; ++i) Values(table)[i] = (total += SubtreeWeight<W>(Children(c)[i], shift - Bits));
        c->Weights = table;
    }
}
template<typename W> Node *Measured(Node *c, uint32_t shift) {
    MeasureWeights<W>(c, shift);
    return c;
}

// The levels between `shift` and the leaves, as a chain holding `leaf` at the bottom. One child each,
// since the chunk it carries is the first thing to reach that far.
template<typename W> const Node *NewPath(uint32_t shift, const Node *leaf) {
    for (uint32_t level = Bits; level <= shift; level += Bits) {
        auto *n = AllocInner(1);
        Children(n)[0] = leaf;
        MeasureWeights<W>(n, level);
        leaf = n;
    }
    return leaf;
}

// A second reference to each node the trie stands on: what a copy and the widening both come to.
template<typename V> void Claim(const V &v) {
    if (v.Root) Retain(v.Root);
    if (v.Tail) Retain(v.Tail);
}

template<typename V> void Swap(V &a, V &b) noexcept {
    std::swap(a.Root, b.Root);
    std::swap(a.Tail, b.Tail);
    std::swap(a.Size, b.Size);
    std::swap(a.Shift, b.Shift);
}

// Where the trie ends and the tail begins. A strict trie holds a whole number of chunks, so the size
// with its low bits cleared says this without reaching for the tail.
template<typename V> uint64_t TailOffset(const V &v) {
    if constexpr (Relaxed(V::Kind)) return v.Size - v.Tail->Count;
    else return (v.Size - 1) & ~Mask;
}

// The slot of `index` under `n`, leaving `index` the offset within it. Where a table says otherwise, the
// radix slot is where the search starts and not merely a guess: a child holds no more than a full one,
// so no slot before it can reach that far.
template<Shape S> uint32_t Slot(const Node *n, uint64_t &index, uint32_t shift) {
    const auto radix = uint32_t(index >> shift);
    if constexpr (Relaxed(S)) {
        if (const auto *sizes = n->Sizes) {
            const auto *totals = Values(sizes);
            auto i = radix;
            while (totals[i] <= index) ++i;
            if (i) index -= totals[i - 1];
            return i;
        }
    }
    index -= uint64_t(radix) << shift;
    return radix;
}

// The leaf holding `index`, leaving `index` the offset within it. A searched descent only: the strict
// one is written out at `operator[]`, where nothing stands between the shift and the load it feeds.
const Node *LeafFor(const Node *n, uint64_t &index, uint32_t shift) {
    for (; shift; shift -= Bits) n = Children(n)[Slot<Shape::Relaxed>(n, index, shift)];
    return n;
}

// A node without a table is full down to its last child, so only that child has to be walked.
uint64_t SubtreeSize(const Node *n, uint32_t shift) {
    if (!shift) return n->Count;
    if (n->Sizes) return Values(n->Sizes)[n->Count - 1];
    return (uint64_t(n->Count - 1) << shift) + SubtreeSize(Children(n)[n->Count - 1], shift - Bits);
}

// Free from the table when there is one, and otherwise from every child but the last being full.
uint64_t ChildSize(const Node *n, uint32_t i, uint32_t shift) {
    if (const auto *sizes = n->Sizes) {
        const auto *totals = Values(sizes);
        return i ? totals[i] - totals[i - 1] : totals[0];
    }
    if (i + 1 < n->Count) return uint64_t{1} << shift;
    return SubtreeSize(Children(n)[i], shift - Bits);
}

// Whether the subtree covers every element it could. A node carrying a table is by that fact not full,
// so the walk stops at the first one it meets.
bool Full(const Node *n, uint32_t shift) {
    if (n->Count != Slots) return false;
    if (!shift) return true;
    return !n->Sizes && Full(Children(n)[Slots - 1], shift - Bits);
}

// Gives `c`, whose children hold `each[i]` elements apiece, a table -- or leaves it without one, which
// is the canonical form for a node whose children are full for their level.
void SetSizes(Node *c, const uint64_t *each, uint32_t count, uint32_t shift) {
    const uint64_t span = uint64_t{1} << shift; // What a child covers when it is full.
    for (uint32_t i = 0; i + 1 < count; ++i) {
        if (each[i] == span) continue;
        auto *sizes = Alloc(count);
        auto *totals = Values(sizes);
        uint64_t total = 0;
        for (uint32_t j = 0; j < count; ++j) totals[j] = (total += each[j]);
        c->Sizes = sizes;
        return;
    }
}

// The same where the caller cannot name the sizes, so every child is measured.
template<typename W> void Measure(Node *c, uint32_t shift) {
    uint64_t each[Slots];
    for (uint32_t i = 0, nc = c->Count; i < nc; ++i) each[i] = SubtreeSize(Children(c)[i], shift - Bits);
    SetSizes(c, each, c->Count, shift);
    MeasureWeights<W>(c, shift);
}

// A fresh inner node standing for `n` with `count` children, the one at `idx` being `child`, which comes
// in with the reference the new node keeps. Every other child is copied by reference, so each is
// claimed. A `count` past what `n` holds appends. A write moves no element, so the copy carries the size
// table it came with rather than working one out -- and an append never has one, being strict.
template<Shape S, typename W> Node *WithChild(const Node *n, uint32_t count, uint32_t idx, const Node *child, uint32_t shift) {
    auto *c = AllocInner(count);
    auto **cs = Children(c);
    const auto *const *src = Children(n);
    std::memcpy(static_cast<void *>(cs), static_cast<const void *>(src), idx * sizeof(const Node *));
    std::memcpy(static_cast<void *>(cs + idx + 1), static_cast<const void *>(src + idx + 1), (count - idx - 1) * sizeof(const Node *));
    for (uint32_t i = 0; i < idx; ++i) Retain(cs[i]);
    for (uint32_t i = idx + 1; i < count; ++i) Retain(cs[i]);
    cs[idx] = child;
    if constexpr (Relaxed(S)) {
        if (n->Sizes) {
            c->Sizes = n->Sizes;
            Retain(c->Sizes);
        }
    }
    return Measured<W>(c, shift);
}

// A fresh node at `shift` holding `n`'s first `keep` children, with `last` in place of the final one --
// or, when `last` is null, that child unchanged. `last` comes in with the reference the new node keeps,
// and every other child is claimed.
template<Shape S, typename W> Node *WithPrefix(const Node *n, uint32_t keep, const Node *last, uint32_t shift) {
    auto *c = AllocInner(keep);
    auto **cs = Children(c);
    const auto *const *src = Children(n);
    std::memcpy(static_cast<void *>(cs), static_cast<const void *>(src), (keep - 1) * sizeof(const Node *));
    for (uint32_t i = 0; i + 1 < keep; ++i) Retain(cs[i]);
    if (last) {
        cs[keep - 1] = last;
    } else {
        cs[keep - 1] = src[keep - 1];
        Retain(cs[keep - 1]);
    }
    if constexpr (Relaxed(S)) {
        if (!n->Sizes) return Measured<W>(c, shift); // Every child but the last was full, and cutting the last leaves that so.
        // Nothing before the cut moved, so the totals carry over and only the last entry is worked out.
        const auto *totals = Values(n->Sizes);
        const uint64_t span = uint64_t{1} << shift;
        uint32_t full = 0;
        while (full + 1 < keep && totals[full] == uint64_t(full + 1) * span) ++full;
        if (full + 1 == keep) return Measured<W>(c, shift); // Short only past the cut, so the copy needs no table at all.
        auto *sizes = Alloc(keep);
        std::memcpy(Values(sizes), totals, (keep - 1) * sizeof(uint64_t));
        Values(sizes)[keep - 1] = last ? (keep > 1 ? totals[keep - 2] : 0) + SubtreeSize(last, shift - Bits) : totals[keep - 1];
        c->Sizes = sizes;
    }
    return Measured<W>(c, shift);
}

// The mirror: `n`'s children from `from` on, with `first` in place of the one at `from`, or that child
// unchanged when `first` is null. Only the relaxed shape can lose its front, so only it comes here.
template<typename W> Node *WithSuffix(const Node *n, uint32_t from, const Node *first, uint32_t shift) {
    const uint32_t keep = n->Count - from;
    auto *c = AllocInner(keep);
    auto **cs = Children(c);
    const auto *const *src = Children(n);
    std::memcpy(static_cast<void *>(cs + 1), static_cast<const void *>(src + from + 1), (keep - 1) * sizeof(const Node *));
    for (uint32_t i = 1; i < keep; ++i) Retain(cs[i]);
    if (first) {
        cs[0] = first;
    } else {
        cs[0] = src[from];
        Retain(cs[0]);
    }
    const uint64_t head = first ? SubtreeSize(first, shift - Bits) : ChildSize(n, from, shift);
    const uint64_t span = uint64_t{1} << shift;
    if (const auto *n_sizes = n->Sizes) {
        // The totals carry over here too, shifted down by everything dropped in front of them.
        const auto *totals = Values(n_sizes);
        uint32_t full = 0;
        if (head == span) {
            ++full;
            while (full + 1 < keep && totals[from + full] - totals[from + full - 1] == span) ++full;
        }
        if (full + 1 >= keep) return Measured<W>(c, shift); // Nothing short but the last child, so the copy is strict.
        auto *sizes = Alloc(keep);
        auto *out = Values(sizes);
        const uint64_t dropped = totals[from] - head;
        out[0] = head;
        for (uint32_t i = 1; i < keep; ++i) out[i] = totals[from + i] - dropped;
        c->Sizes = sizes;
        return Measured<W>(c, shift);
    }
    // Nothing but the first child can be short, `n` having been full to its own last.
    uint64_t each[Slots];
    each[0] = head;
    for (uint32_t i = 1; i < keep; ++i) each[i] = ChildSize(n, from + i, shift);
    SetSizes(c, each, keep, shift);
    return Measured<W>(c, shift);
}

// Levels the trie no longer needs, dropped from the top: leaving a root over a single child would make
// the shape depend on how the vector was built. Takes the reference it is handed and gives one back.
void Collapse(const Node *&root, uint32_t &shift) {
    while (shift && root->Count == 1) {
        const auto *child = Children(root)[0];
        Retain(child);
        Release(root, shift);
        root = child;
        shift -= Bits;
    }
}

// Put `leaf` on the end of the subtrie under `n`, which holds `told` elements counted from the start of
// the whole trie. `owned` means every node from the root down to `n` is held exactly once, so writing
// through it is unobservable, and returning `n` leaves the path above untouched.
//
// An append only ever touches the rightmost path, and a strict trie can say from `told` alone where that
// path runs: the slot the chunk lands in either holds the one partly filled child or holds nothing yet,
// and a level that is starting fresh is not descended at all.
template<typename W> const Node *PushLeaf(const Node *n, const Node *leaf, uint64_t told, uint32_t shift, bool owned) {
    const auto idx = uint32_t((told >> shift) & Mask);
    // Nothing below this level is half filled exactly when the elements divide evenly by what a child
    // covers, and then the slot is one no chunk has reached.
    const bool fresh = (told & ((uint64_t{1} << shift) - 1)) == 0;
    const auto *old = fresh ? nullptr : Children(n)[idx];
    const auto *child = fresh ? NewPath<W>(shift - Bits, leaf) : PushLeaf<W>(old, leaf, told, shift - Bits, owned && old->Refs == 1);
    if (owned) {
        if (child == old) return Measured<W>(Mutable(n), shift);
        if (old) Release(old, shift - Bits);
        Children(Mutable(n))[idx] = child;
        Mutable(n)->Count = idx + 1;
        return Measured<W>(Mutable(n), shift);
    }
    return WithChild<Shape::Strict, W>(n, idx + 1, idx, child, shift);
}

// The same where nothing says where the rightmost path runs, so it is walked to the bottom and the chunk
// placed on the way back up. Null when there is no room under `n` and the caller has to grow a level.
template<typename W> const Node *PushLeaf(const Node *n, const Node *leaf, uint32_t shift, bool owned) {
    const uint32_t last = n->Count - 1;
    const Node *grown = nullptr;
    if (shift > Bits) {
        const auto *below = Children(n)[last];
        grown = PushLeaf<W>(below, leaf, shift - Bits, owned && below->Refs == 1);
    }
    if (!grown && n->Count == Slots) return nullptr; // Slot-full to the right all the way down.
    const uint32_t count = grown ? n->Count : n->Count + 1, idx = count - 1;
    const auto *child = grown ? grown : NewPath<W>(shift - Bits, leaf);

    Node *c;
    if (owned) {
        c = Mutable(n);
        if (grown && grown != Children(c)[last]) Release(Children(c)[last], shift - Bits);
        Children(c)[idx] = child;
        c->Count = count;
    } else {
        c = AllocInner(count);
        std::memcpy(static_cast<void *>(Children(c)), static_cast<const void *>(Children(n)), idx * sizeof(const Node *));
        for (uint32_t i = 0; i < idx; ++i) Retain(Children(c)[i]);
        Children(c)[idx] = child;
    }
    if (const auto *sizes = n->Sizes) {
        // Grown or appended, the last entry ends up the same: everything that was here, and the chunk.
        const uint64_t total = Values(sizes)[last] + leaf->Count;
        // The table grows with the node, so a shared one has to be copied before it can be written.
        if (owned && sizes->Refs == 1) {
            Values(Mutable(sizes))[idx] = total;
            Mutable(sizes)->Count = count;
        } else {
            auto *fresh = Alloc(count);
            std::memcpy(Values(fresh), Values(sizes), idx * sizeof(uint64_t));
            Values(fresh)[idx] = total;
            if (owned) Release(sizes, 0);
            c->Sizes = fresh;
        }
    } else if (!grown && !Full(Children(n)[last], shift - Bits)) {
        // Appending beside a last child that was not full turns a strict node relaxed.
        uint64_t each[Slots];
        for (uint32_t i = 0; i < last; ++i) each[i] = uint64_t{1} << shift;
        each[last] = SubtreeSize(Children(n)[last], shift - Bits);
        each[idx] = leaf->Count;
        SetSizes(c, each, count, shift);
    }
    return Measured<W>(c, shift);
}

// The trie with `leaf` on the end of it, where a filled tail goes. `told` is what the trie held before
// it. `root` may be null, and both it and `leaf` come in with the reference the answer keeps.
template<Shape S, typename W> void PushChunk(const Node *&root, uint32_t &shift, const Node *leaf, uint64_t told) {
    if (!root) { // The first full tail is the whole trie.
        root = leaf;
        shift = 0;
        return;
    }
    // A strict trie is full exactly when its elements fill the root, and a relaxed one has to be asked.
    // Either way a root that is one lone chunk falls straight through to the level that goes on top.
    const Node *grown = nullptr;
    if constexpr (Relaxed(S)) {
        if (shift) grown = PushLeaf<W>(root, leaf, shift, root->Refs == 1);
    } else {
        if (told != Covers(shift)) grown = PushLeaf<W>(root, leaf, told, shift, root->Refs == 1);
    }
    if (grown) {
        if (grown != root) {
            Release(root, shift);
            root = grown;
        }
        return;
    }
    // No room under the root, so a level goes on top of it.
    auto *top = AllocInner(2);
    Children(top)[0] = root;
    Children(top)[1] = NewPath<W>(shift, leaf);
    if constexpr (Relaxed(S)) {
        const uint64_t each[]{SubtreeSize(root, shift), leaf->Count};
        SetSizes(top, each, 2, shift + Bits);
    }
    MeasureWeights<W>(top, shift + Bits);
    root = top;
    shift += Bits;
}

// `value` says what to store given what the index holds now: the one thing `Set` and `Update` differ
// in. A template and not a pointer, so `Set` compiles as it would have on its own.
template<Shape S, typename W, typename F> const Node *SetAt(const Node *n, uint64_t index, F value, uint32_t shift, bool owned) {
    if (!shift) {
        const auto slot = uint32_t(index);
        if (!owned) {
            auto *c = CopiedLeaf(n, n->Count);
            Values(c)[slot] = value(Values(n)[slot]);
            return c;
        }
        auto &held = Values(Mutable(n))[slot];
        held = value(held);
        return n;
    }
    const auto idx = Slot<S>(n, index, shift);
    const auto *old = Children(n)[idx];
    const auto *child = SetAt<S, W>(old, index, value, shift - Bits, owned && old->Refs == 1);
    if (child == old) return Measured<W>(Mutable(n), shift); // Written in place. The weight may have changed.
    if (!owned) return WithChild<S, W>(n, n->Count, idx, child, shift);
    Children(Mutable(n))[idx] = child;
    Release(old, shift - Bits);
    return Measured<W>(Mutable(n), shift);
}

// The subtrie under `n` cut to its first `count` elements, null when nothing is left of it, with the
// leaf that ends the cut handed back in `tail` rather than kept. Only the rightmost kept path is copied:
// every subtrie left of it is kept whole and shared.
template<Shape S, typename W> const Node *Cut(const Node *n, uint64_t count, uint32_t shift, const Node *&tail) {
    if (!shift) {
        if (count == n->Count) {
            Retain(n);
            tail = n;
        } else {
            tail = CopiedLeaf(n, uint32_t(count));
        }
        return nullptr;
    }
    uint64_t within = count - 1;
    const auto idx = Slot<S>(n, within, shift);
    const auto *child = Cut<S, W>(Children(n)[idx], within + 1, shift - Bits, tail);
    if (!child && !idx) return nullptr;
    return WithPrefix<S, W>(n, child ? idx + 1 : idx, child, shift);
}

// The subtrie under `n` with its first `count` elements dropped, `count` being neither none of it nor
// all of it. Only the leftmost kept path is copied.
template<typename W> const Node *Dropped(const Node *n, uint64_t count, uint32_t shift) {
    if (!shift) {
        auto *c = Alloc(n->Count - uint32_t(count));
        std::memcpy(Values(c), Values(n) + count, (n->Count - count) * sizeof(uint64_t));
        return c;
    }
    uint64_t within = count;
    const auto idx = Slot<Shape::Relaxed>(n, within, shift);
    const auto *child = within ? Dropped<W>(Children(n)[idx], within, shift - Bits) : nullptr;
    return WithSuffix<W>(n, idx, child, shift);
}

// How many nodes a joined run may take beyond the fewest that would do. Bagwell and Rompf's `e`, and
// the same 2 immer uses.
constexpr uint32_t Extra = 2;

// `center` is the join of what sat under the last child of `left` and the first child of `right`, all
// three standing at `shift`. Their children -- `left`'s bar its last, `center`'s, `right`'s bar its
// first -- are one run at `shift - Bits`, and this hands back a node at `shift + Bits` holding that run
// repacked into one or two children. `left` and `right` may be null, and all three are borrowed.
template<typename W> const Node *Rebalance(const Node *left, const Node *center, const Node *right, uint32_t shift) {
    const uint32_t below = shift - Bits;
    const Node *run[3 * Slots];
    uint32_t sizes[3 * Slots];
    uint32_t n = 0, total = 0;
    const auto gather = [&](const Node *from, uint32_t first, uint32_t last) {
        for (uint32_t i = first; i < last; ++i) {
            run[n] = Children(from)[i];
            total += sizes[n] = run[n]->Count;
            ++n;
        }
    };
    if (left) gather(left, 0, left->Count - 1);
    gather(center, 0, center->Count);
    if (right) gather(right, 1, right->Count);
    // The induction the whole algorithm rests on: a run this long repacks into two nodes, so the join
    // one level up gets a `center` of two children, so its run is this long again.
    assert(n <= 2 * Slots);

    // The plan: how many slots each node of the repacked run takes. Left alone, a run keeps every hole
    // the join opened and those holes compound over levels, so all but `Extra` of them are squeezed
    // out -- one short node at a time, filled from the ones after it.
    const uint32_t fewest = (total + Slots - 1) / Slots;
    uint32_t count = n;
    for (uint32_t i = 0; fewest + Extra < count;) {
        while (sizes[i] == Slots) ++i;
        for (uint32_t carry = sizes[i]; carry;) {
            const uint32_t fill = carry + sizes[i + 1] < Slots ? carry + sizes[i + 1] : Slots;
            carry += sizes[i + 1] - fill;
            sizes[i++] = fill;
        }
        for (uint32_t j = i; j + 1 < count; ++j) sizes[j] = sizes[j + 1];
        --count;
        --i;
    }

    // A node the plan leaves alone is shared rather than rebuilt.
    const Node *packed[3 * Slots];
    for (uint32_t i = 0, src = 0, taken = 0; i < count; ++i) {
        const uint32_t want = sizes[i];
        if (!taken && run[src]->Count == want) {
            Retain(run[src]);
            packed[i] = run[src++];
            continue;
        }
        auto *c = AllocInner(want);
        for (uint32_t filled = 0; filled < want;) {
            const uint32_t left_here = run[src]->Count - taken;
            const uint32_t take = want - filled < left_here ? want - filled : left_here;
            if (below) {
                std::memcpy(static_cast<void *>(Children(c) + filled), static_cast<const void *>(Children(run[src]) + taken), take * sizeof(const Node *));
                for (uint32_t k = 0; k < take; ++k) Retain(Children(c)[filled + k]);
            } else {
                std::memcpy(Values(c) + filled, Values(run[src]) + taken, take * sizeof(uint64_t));
            }
            filled += take;
            taken += take;
            if (taken == run[src]->Count) {
                ++src;
                taken = 0;
            }
        }
        if (below) Measure<W>(c, below);
        packed[i] = c;
    }

    // Roofed over twice: one node per `Slots` of the run, and one node over those.
    const uint32_t roofs = (count + Slots - 1) / Slots;
    auto *top = AllocInner(roofs);
    for (uint32_t p = 0; p < roofs; ++p) {
        const uint32_t from = p * Slots, to = from + Slots < count ? from + Slots : count;
        auto *node = AllocInner(to - from);
        std::memcpy(static_cast<void *>(Children(node)), static_cast<const void *>(packed + from), (to - from) * sizeof(const Node *));
        Measure<W>(node, shift);
        Children(top)[p] = node;
    }
    Measure<W>(top, shift + Bits);
    return top;
}

// The join of two subtries, as a node standing one level above the taller of them and holding one or
// two children at that level. Neither may be empty.
template<typename W> const Node *Joined(const Node *left, uint32_t lshift, const Node *right, uint32_t rshift) {
    if (lshift > rshift) {
        const auto *center = Joined<W>(Children(left)[left->Count - 1], lshift - Bits, right, rshift);
        const auto *out = Rebalance<W>(left, center, nullptr, lshift);
        Release(center, lshift);
        return out;
    }
    if (lshift < rshift) {
        const auto *center = Joined<W>(left, lshift, Children(right)[0], rshift - Bits);
        const auto *out = Rebalance<W>(nullptr, center, right, rshift);
        Release(center, rshift);
        return out;
    }
    if (!lshift) { // Two chunks, held side by side for the level above to repack.
        auto *top = AllocInner(2);
        Retain(left);
        Retain(right);
        Children(top)[0] = left;
        Children(top)[1] = right;
        const uint64_t each[]{left->Count, right->Count};
        SetSizes(top, each, 2, Bits);
        return Measured<W>(top, Bits);
    }
    const auto *center = Joined<W>(Children(left)[left->Count - 1], lshift - Bits, Children(right)[0], rshift - Bits);
    const auto *out = Rebalance<W>(left, center, right, lshift);
    Release(center, lshift);
    return out;
}

// Everything `v` holds as one trie, its tail folded in. Carries a reference.
template<typename W> const Node *Flattened(const Flex<W> &v, uint32_t &shift) {
    if (!v.Root) {
        Retain(v.Tail);
        shift = 0;
        return v.Tail;
    }
    const auto *root = v.Root;
    Retain(root);
    Retain(v.Tail);
    shift = v.Shift;
    PushChunk<Shape::Relaxed, W>(root, shift, v.Tail, TailOffset(v));
    return root;
}

// Structure the two vectors share settles a whole subtrie at once. Only the strict shape can walk this
// way: equal sizes mean equal shapes there, so the two tries step in lockstep.
bool SameNodes(const Node *a, const Node *b, uint32_t shift) {
    if (a == b) return true;
    if (a->Count != b->Count) return false;
    if (!shift) return std::memcmp(Values(a), Values(b), a->Count * sizeof(uint64_t)) == 0;
    const auto *const *ca = Children(a);
    const auto *const *cb = Children(b);
    for (uint32_t i = 0, nc = a->Count; i < nc; ++i)
        if (!SameNodes(ca[i], cb[i], shift - Bits)) return false;
    return true;
}

// A position in a vector: the chunk holding the element and the path down to it.
struct Cursor {
    const Node *Nodes[Iterator::MaxDepth + 1]; // `Nodes[Depth]` is the chunk, and above it lies the path.
    uint32_t Slots[Iterator::MaxDepth]; // `Slots[d]` is the child of `Nodes[d]` the path takes.
    const uint64_t *Values; // The chunk's elements, and the index of the first of them.
    uint64_t Base;
    uint32_t Count, Depth;
};

// The cursor at `index`, which must be in range.
template<typename V> Cursor SeekTo(const V &v, uint64_t index) {
    Cursor c;
    const auto told = TailOffset(v);
    if (index >= told) {
        c.Depth = 0;
        c.Nodes[0] = v.Tail;
        c.Base = told;
    } else {
        uint64_t within = index;
        const auto *n = v.Root;
        c.Depth = 0;
        for (auto shift = v.Shift; shift; shift -= Bits) {
            const auto slot = Slot<V::Kind>(n, within, shift);
            c.Nodes[c.Depth] = n;
            c.Slots[c.Depth] = slot;
            n = Children(n)[slot];
            ++c.Depth;
        }
        c.Nodes[c.Depth] = n;
        c.Base = index - within;
    }
    c.Values = Values(c.Nodes[c.Depth]);
    c.Count = c.Nodes[c.Depth]->Count;
    return c;
}

// Moves the cursor to the next chunk, or returns false at the end.
template<typename V> bool Advance(const V &v, Cursor &c) {
    const auto at = c.Base + c.Count;
    if (at >= v.Size) return false;
    if (at >= TailOffset(v)) { // Into the tail, which is on no path.
        c = SeekTo(v, at);
        return true;
    }
    uint32_t d = c.Depth;
    while (d && c.Slots[d - 1] + 1 >= c.Nodes[d - 1]->Count) --d;
    ++c.Slots[d - 1];
    const auto *n = Children(c.Nodes[d - 1])[c.Slots[d - 1]];
    for (; d < c.Depth; ++d) {
        c.Nodes[d] = n;
        c.Slots[d] = 0;
        n = Children(n)[0];
    }
    c.Nodes[c.Depth] = n;
    c.Base = at;
    c.Values = Values(n);
    c.Count = n->Count;
    return true;
}

// How many elements sit under `n`'s children before the one at `i`.
uint64_t SlotOffset(const Node *n, uint32_t i, uint32_t shift) {
    if (const auto *sizes = n->Sizes) return i ? Values(sizes)[i - 1] : 0;
    return uint64_t(i) << shift;
}

// How far a match reaches around one position: `Before` elements before it and `After` from it on.
struct Run {
    uint64_t Before, After;
};

// How far the two cursors are on the same nodes, before and from their positions.
// Zero on different chunks, which says nothing about the elements.
// `Ahead` picks which side the siblings are read on.
template<bool Ahead> Run Shared(const Cursor &a, uint64_t ia, const Cursor &b, uint64_t ib) {
    const auto within = ia - a.Base;
    if (a.Nodes[a.Depth] != b.Nodes[b.Depth] || within != ib - b.Base) return {0, 0};
    Run run{within, a.Count - within};
    uint32_t da = a.Depth, db = b.Depth, shift = 0;
    while (da && db) {
        const auto *pa = a.Nodes[da - 1], *pb = b.Nodes[db - 1];
        if (pa != pb || a.Slots[da - 1] != b.Slots[db - 1]) break;
        --da;
        --db;
        shift += Bits;
        run.Before += SlotOffset(pa, a.Slots[da], shift);
        run.After = SubtreeSize(pa, shift) - run.Before;
    }
    // A write copies only its path and leaves every sibling shared, so the run continues through the siblings that are the same node.
    // One pointer compare per sibling, instead of a fresh descent per subtrie.
    while (da && db) {
        const auto *pa = a.Nodes[da - 1], *pb = b.Nodes[db - 1];
        const auto pshift = shift + Bits;
        const auto *const *ca = Children(pa);
        const auto *const *cb = Children(pb);
        uint32_t sa = a.Slots[da - 1], sb = b.Slots[db - 1];
        if constexpr (Ahead) {
            while (sa + 1 < pa->Count && sb + 1 < pb->Count && ca[sa + 1] == cb[sb + 1]) {
                ++sa;
                ++sb;
                run.After += ChildSize(pa, sa, pshift);
            }
            if (sa + 1 < pa->Count || sb + 1 < pb->Count) break;
        } else {
            while (sa && sb && ca[sa - 1] == cb[sb - 1]) {
                --sa;
                --sb;
                run.Before += ChildSize(pa, sa, pshift);
            }
            if (sa || sb) break;
        }
        --da;
        --db;
        shift = pshift;
    }
    return run;
}

// How many elements `a` from `ia` and `b` from `ib` agree on, up to `limit`.
template<typename V> uint64_t Extend(const V &a, uint64_t ia, const V &b, uint64_t ib, uint64_t limit) {
    if (!limit) return 0;
    auto ca = SeekTo(a, ia), cb = SeekTo(b, ib);
    uint64_t n = 0;
    while (true) {
        if (const auto shared = Shared<true>(ca, ia + n, cb, ib + n).After) {
            n += std::min(shared, limit - n); // A whole subtrie was skipped, so both cursors are sought again.
            if (n >= limit) break;
            ca = SeekTo(a, ia + n);
            cb = SeekTo(b, ib + n);
            continue;
        }
        const auto oa = uint32_t(ia + n - ca.Base), ob = uint32_t(ib + n - cb.Base);
        const auto run = std::min({uint64_t{ca.Count - oa}, uint64_t{cb.Count - ob}, limit - n});
        uint64_t k = 0;
        while (k < run && ca.Values[oa + k] == cb.Values[ob + k]) ++k;
        n += k;
        if (k < run || n >= limit) break;
        if (ia + n == ca.Base + ca.Count) Advance(a, ca);
        if (ib + n == cb.Base + cb.Count) Advance(b, cb);
    }
    return n;
}

// The same backwards: how many elements `a` before `ia` and `b` before `ib` agree on, up to `limit`.
template<typename V> uint64_t ExtendBack(const V &a, uint64_t ia, const V &b, uint64_t ib, uint64_t limit) {
    uint64_t n = 0;
    while (n < limit) {
        const auto ca = SeekTo(a, ia - n - 1), cb = SeekTo(b, ib - n - 1);
        // A shared element and everything before it in the run agree.
        if (const auto shared = Shared<false>(ca, ia - n - 1, cb, ib - n - 1); shared.After) {
            n += std::min(shared.Before + 1, limit - n);
            continue;
        }
        const auto oa = uint32_t(ia - n - 1 - ca.Base), ob = uint32_t(ib - n - 1 - cb.Base);
        const auto run = std::min(uint64_t{std::min(oa, ob)} + 1, limit - n);
        uint64_t k = 0;
        while (k < run && ca.Values[oa - k] == cb.Values[ob - k]) ++k;
        n += k;
        if (k < run) break;
    }
    return n;
}

// The budget of the full search, which finds a script of up to twice this many edits.
// The search costs the square of the budget, all of it when it fails.
// Four chunks' worth finds every gap left by rewriting a twentieth of a million positions.
// Doubling it found nothing more at any density measured, and halving it leaves some of those gaps reported as one run.
constexpr int64_t MaxEdits = 4 * Slots;
// The budget of the first search.
// Nearly every stretch is resolved here, and one that is not differs by more than twice this, as a moved block does.
constexpr int64_t Probe = 32;
// The two frontiers of the search, one entry per diagonal, indexed from `-MaxEdits` to `MaxEdits`.
// File scope rather than the stack, at 4KB apiece.
int64_t Frontiers[2][2 * MaxEdits + 3];

// The value of a diagonal no path has reached yet, low enough that a step from it is never on the grid.
constexpr int64_t Unreached = INT64_MIN / 2;

// A point a shortest script through `n` elements against `m` passes, or false when its cost exceeds `budget`.
// `extend(ahead, x, y, limit)` reads the elements: how many agree from `x` and `y` on, or before the `x`th and `y`th from the end.
template<typename Extender> bool Middle(int64_t n, int64_t m, int64_t budget, Extender &&extend, int64_t &sx, int64_t &sy) {
    const int64_t delta = n - m;
    auto *fwd = Frontiers[0] + MaxEdits + 1;
    auto *bwd = Frontiers[1] + MaxEdits + 1;
    // A split at either corner leaves one half equal to the whole, so only a split inside the grid is taken, which keeps the recursion finite.
    const auto split = [&](int64_t x, int64_t y) {
        if ((!x && !y) || (x == n && y == m)) return false;
        sx = x;
        sy = y;
        return true;
    };
    // One step from the previous cost's frontier to the furthest point still on the grid.
    // `left` is the diagonal below as it was before this cost overwrote it, and `right` the diagonal above, which this cost has not reached yet.
    const auto step = [n, m](int64_t left, int64_t here, int64_t right, int64_t k) {
        const auto on = [k, n, m](int64_t x) { return x >= 0 && x <= n && x - k >= 0 && x - k <= m; };
        auto x = here;
        for (const auto to : {left + 1, here + 1, right})
            if (to > x && on(to)) x = to;
        return x;
    };
    // Advances one frontier by one cost and checks it against `other` for a meeting, which is where the two together span `n`.
    // `ahead` is which end `self` grows from, and `slack` is how many costs `other` is behind.
    // The forward sweep meets the backward frontier of the previous cost, and the backward sweep meets the forward frontier of the same cost.
    const auto sweep = [&](bool ahead, int64_t d, int64_t *self, const int64_t *other, int64_t slack) -> std::optional<bool> {
        // The diagonals this cost reaches for the first time, cleared of an earlier search's values.
        self[-d] = self[d] = self[d + 1] = Unreached;
        for (int64_t k = -d, left = Unreached; k <= d; ++k) {
            const auto here = self[k];
            auto x = step(left, here, self[k + 1], k);
            left = here;
            if (x == Unreached) continue;
            const auto y = x - k;
            if (x < n && y < m) {
                const auto limit = std::min(uint64_t(n - x), uint64_t(m - y));
                x += int64_t(extend(ahead, uint64_t(x), uint64_t(y), limit));
            }
            self[k] = x;
            if (const int64_t c = delta - k; c >= slack - d && c <= d - slack && x + other[c] >= n) return ahead ? split(x, x - k) : split(n - x, m - (x - k));
        }
        return {};
    };
    // Both frontiers start at the corners, where the stretches differ, so neither extends.
    fwd[0] = bwd[0] = 0;
    // A script costs at most the longer stretch, and the frontiers meet at half the cost.
    const int64_t furthest = int64_t(std::min(uint64_t(std::max(n, m)) + 1, uint64_t(budget) * 2) / 2);
    // Ukkonen's diagonal transitions from both ends, checked for a meeting after both steps at each cost.
    // Myers checks after one step, since under his metric a script's cost has the parity of `n - m`, and an overwrite has no parity.
    for (int64_t d = 1; d <= furthest; ++d) {
        if (const auto crossed = sweep(true, d, fwd, bwd, 1)) return *crossed;
        if (const auto crossed = sweep(false, d, bwd, fwd, 0)) return *crossed;
    }
    return false;
}

// No position: a key the table lacks, the end of a chain, or a key struck out for repeating.
constexpr uint64_t None = ~uint64_t{0}, Repeated = None;

// The position of each key in a stretch, open addressed.
// `At` holds the position plus one, so zero is a free slot, or `Repeated`.
// A key at two positions is struck out, since it says nothing about where a block went.
struct Table {
    struct Slot {
        uint64_t Key, At;
    };

    uint64_t Mask, Held = 0;
    std::vector<Slot> Slots;

    // Room for `want` keys at half occupancy at most, since a lookup walks to a free slot.
    explicit Table(uint64_t want) : Mask(std::bit_ceil(2 * want) - 1), Slots(Mask + 1) {}

    Slot *Find(uint64_t key) {
        auto *slot = Slots.data() + ((key ^ (key >> 29)) & Mask);
        for (; slot->At && slot->Key != key; slot = Slots.data() + ((uint64_t(slot - Slots.data()) + 1) & Mask));
        return slot;
    }
    // Keys past the room are dropped, which can only lose a run.
    void Keep(uint64_t key, uint64_t at) {
        auto *slot = Find(key);
        if (slot->At) slot->At = Repeated;
        else if (Held < (Mask + 1) / 2) {
            *slot = {key, at + 1};
            ++Held;
        }
    }
    uint64_t At(uint64_t key) {
        const auto *slot = Find(key);
        return slot->At && slot->At != Repeated ? slot->At - 1 : None;
    }
};

// Nodes are aligned to sixteen bytes, so the low four bits are dropped.
uint64_t ChunkKey(const Node *n) { return reinterpret_cast<uintptr_t>(n) >> 4; }

// One run common to both stretches: `Len` elements from `X` in `a` and from `Y` in `b`.
struct Anchor {
    uint64_t X, Y, Len;
};

// Per run in `Chain`: the weight of the best chain ending at it, and the run before it.
struct Link {
    uint64_t Best, Prev;
};

// Extends a match at `ia` and `ib` both ways into a run, and keeps the run if it is at least a chunk long.
// Returns how far the run reaches from `ib` on, kept or not, so the caller can step past it.
// `taken` moves to the end of a kept run, which keeps the runs disjoint in `b`.
template<typename V> uint64_t Grow(const V &a, uint64_t x0, uint64_t x1, const V &b, uint64_t y1, uint64_t ia, uint64_t ib, uint64_t &taken, std::vector<Anchor> &found) {
    const auto before = ExtendBack(a, ia, b, ib, std::min(ia - x0, ib - taken));
    const auto after = Extend(a, ia, b, ib, std::min(x1 - ia, y1 - ib));
    if (before + after >= Slots) {
        found.push_back({ia - before, ib - before, before + after});
        taken = ib + after;
    }
    return after;
}

// The runs common to both stretches found by identity, in the order they sit in `b`, disjoint there.
template<typename V> void Runs(const V &a, uint64_t x0, uint64_t x1, const V &b, uint64_t y0, uint64_t y1, std::vector<Anchor> &found) {
    // Sized from the length rather than a count of chunks, so the table size does not depend on how the stretch is cut.
    Table table((x1 - x0) / Slots + 2);

    // A chunk straddling either end is left out, so every run stays inside the stretch.
    for (auto c = SeekTo(a, x0);;) {
        if (c.Base >= x0) table.Keep(ChunkKey(c.Nodes[c.Depth]), c.Base);
        if (c.Base + c.Count >= x1 || !Advance(a, c)) break;
    }

    uint64_t taken = y0; // How far into `b` the runs reach, where the next one is measured from.
    for (auto c = SeekTo(b, y0);;) {
        // Skips a found run, so a block's chunks are measured once.
        auto next = c.Base + c.Count;
        if (const auto ia = c.Base >= y0 ? table.At(ChunkKey(c.Nodes[c.Depth])) : None; ia != None)
            next = std::max(next, c.Base + Grow(a, x0, x1, b, y1, ia, c.Base, taken, found));
        if (next >= y1) break;
        if (next == c.Base + c.Count) {
            if (!Advance(b, c)) break;
        } else {
            c = SeekTo(b, next);
        }
    }
}

template<typename V> void Copy(const V &v, uint64_t from, uint64_t to, uint64_t *out) {
    for (auto c = SeekTo(v, from); from < to; Advance(v, c)) {
        const auto offset = from - c.Base, count = std::min(uint64_t{c.Count} - offset, to - from);
        std::memcpy(out, c.Values + offset, count * sizeof(uint64_t));
        out += count;
        from += count;
    }
}

template<typename V> struct Reader {
    const V &Vec;
    Cursor Cur;
    uint64_t At;
    Reader(const V &v, uint64_t at) : Vec(v), Cur(SeekTo(v, at)), At(at) {}
    uint64_t Next() {
        if (At == Cur.Base + Cur.Count) Advance(Vec, Cur);
        return Cur.Values[At++ - Cur.Base];
    }
};

// A window is hashed as a polynomial in an odd base over the mixed values, so the hash rolls.
// The mixing keeps structured values from cancelling through the base.
constexpr uint64_t Window = Slots;
constexpr uint64_t HashBase = 0x9E3779B97F4A7C15;
constexpr uint64_t Highest = [] {
    uint64_t p = 1;
    for (uint64_t i = 1; i < Window; ++i) p *= HashBase;
    return p;
}();
uint64_t Mixed(uint64_t v) { return (v ^ (v >> 31)) * 0xBF58476D1CE4E5B9; }

// Adds the runs found by content to those found by identity, still disjoint in `b`.
// Two runs may overlap in `a`, which `Chain` allows.
template<typename V> void Blocks(const V &a, uint64_t x0, uint64_t x1, const V &b, uint64_t y0, uint64_t y1, std::vector<Anchor> &found) {
    if (x1 - x0 < Window || y1 - y0 < Window) return;
    const auto count = found.size(); // The runs identity found, indexed rather than pointed to, since `push_back` may move them.

    // Visits the stretches of `a` no run covers, in order.
    // The runs may overlap in `a`, so they are sorted by start first.
    std::vector<uint64_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&found](uint64_t i, uint64_t j) { return found[i].X < found[j].X; });
    const auto uncovered = [&](auto visit) {
        uint64_t covered = x0;
        for (uint64_t r = 0; r < count; ++r) {
            const auto &run = found[order[r]];
            if (run.X > covered) visit(covered, run.X);
            covered = std::max(covered, run.X + run.Len);
        }
        if (x1 > covered) visit(covered, x1);
    };
    uint64_t windows = 0;
    uncovered([&windows](uint64_t s, uint64_t e) { windows += (e - s) / Window; });
    if (!windows) return;

    // rsync's method: the windows of `a` at a fixed stride are hashed into a table, and a window is rolled over `b` one element at a time.
    Table table(windows);
    // A bit set in front of the table, so a position of `b` matching no window of `a` costs one load rather than a table probe.
    // The bit set is a sixteenth of the table's size, at least one word, and stays in cache.
    const uint64_t mark_mask = std::max<uint64_t>((table.Mask + 1) * 16, 64) - 1;
    std::vector<uint64_t> seen((mark_mask + 1) / 64);
    const auto mark = [mark_mask](uint64_t h) { return (h >> 20) & mark_mask; };
    uncovered([&](uint64_t s, uint64_t e) {
        Reader ra(a, s);
        for (uint64_t p = s; p + Window <= e; p += Window) {
            uint64_t h = 0;
            for (uint64_t i = 0; i < Window; ++i) h = h * HashBase + Mixed(ra.Next());
            table.Keep(h, p);
            seen[mark(h) / 64] |= uint64_t{1} << (mark(h) % 64);
        }
    });

    // The mixed values of the window at `q`, each at the slot of its position, so the value leaving shares the slot of the value arriving.
    uint64_t ring[Window];
    uint64_t r = 0; // The first run found by identity that the walk has not passed.
    uint64_t taken = y0; // How far into `b` the runs reach, and where the window restarts after one.
    for (uint64_t q = y0;;) {
        while (r < count && found[r].Y + found[r].Len <= q) ++r;
        // Where the uncovered stretch ends: at the next run found by identity, or at the end of the stretch.
        // `q` itself when the walk is inside a run, which the branch below skips.
        const auto end = r == count ? y1 : std::max(found[r].Y, q);
        if (q + Window > end) { // Inside a run, or in a stretch too short for a window.
            if (r == count) break;
            taken = q = found[r].Y + found[r].Len;
            ++r;
            continue;
        }
        Reader rb(b, q);
        uint64_t h = 0;
        for (uint64_t i = 0; i < Window; ++i) h = h * HashBase + (ring[(q + i) & (Window - 1)] = Mixed(rb.Next()));
        while (true) {
            // A window that is a real match extends at least its own length, and a hash collision does not.
            if (const auto ia = (seen[mark(h) / 64] >> (mark(h) % 64)) & 1 ? table.At(h) : None; ia != None) {
                if (Grow(a, x0, x1, b, end, ia, q, taken, found) >= Window) {
                    q = taken;
                    break;
                }
            }
            if (q + Window >= end) {
                q = end;
                break;
            }
            auto &leaving = ring[q & (Window - 1)];
            const auto arriving = Mixed(rb.Next());
            h = (h - leaving * Highest) * HashBase + arriving;
            leaving = arriving;
            ++q;
        }
        if (q == y1) break;
    }
}

// Of `runs` in the order they sit in `b`, the chain covering the most elements: runs increasing in `a` as well, none overlapping the next.
// Returns the indices of the chosen runs, first to last.
std::vector<uint64_t> Chain(const std::vector<Anchor> &runs) {
    // A longest increasing subsequence weighted by length, over a Fenwick tree of the runs' ends in `a`.
    const auto count = runs.size();
    std::vector<uint64_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&runs](uint64_t i, uint64_t j) { return runs[i].X + runs[i].Len < runs[j].X + runs[j].Len; });
    std::vector<Link> links(count);
    std::vector<uint64_t> rank(count);
    // Per prefix of the order, the best chain and the run it ends at.
    std::vector<Link> tree(count + 1);
    for (uint64_t r = 0; r < count; ++r) rank[order[r]] = r + 1;
    uint64_t best = 0, last = None;
    for (uint64_t j = 0; j < count; ++j) {
        // The runs ending in `a` at or before this one starts form a prefix of the order.
        uint64_t lo = 0, hi = count;
        while (lo < hi) {
            const auto mid = (lo + hi) / 2;
            if (runs[order[mid]].X + runs[order[mid]].Len <= runs[j].X) lo = mid + 1;
            else hi = mid;
        }
        Link at{0, None};
        for (auto r = lo; r; r -= r & (~r + 1))
            if (tree[r].Best > at.Best) at = tree[r];
        links[j] = {runs[j].Len + at.Best, at.Prev};
        for (auto r = rank[j]; r <= count; r += r & (~r + 1))
            if (links[j].Best > tree[r].Best) tree[r] = {links[j].Best, j};
        if (links[j].Best > best) {
            best = links[j].Best;
            last = j;
        }
    }
    uint64_t chosen = 0;
    for (auto j = last; j != None; j = links[j].Prev) ++chosen;
    for (auto j = last, at = chosen; j != None; j = links[j].Prev) order[--at] = j;
    order.resize(chosen);
    return order;
}

// A split can leave two adjacent runs, so each run is held until the next arrives and merged with it if they touch.
struct Sink {
    void (*Report)(void *, const Change &);
    void *Context;
    Change Held{};
    bool Holding{};

    void operator()(uint64_t index, uint64_t removed, uint64_t to, uint64_t added) {
        if (Holding && Held.Index + Held.Removed == index && Held.To + Held.Added == to) {
            Held.Removed += removed;
            Held.Added += added;
            return;
        }
        Flush();
        Held = {index, removed, to, added};
        Holding = true;
    }
    void Flush() {
        if (Holding) Report(Context, Held);
        Holding = false;
    }
};

// Which stage of `DiffRange` resolved a stretch.
enum : uint32_t { ByProbe,
                  ByRun,
                  ByBlock,
                  BySearch,
                  ByNothing };
#ifdef VECTOR_AUDIT
uint64_t Settled[5];
void Tally(uint32_t which) { ++Settled[which]; }
#else
void Tally(uint32_t) {}
#endif

template<typename V> void DiffRange(const V &a, uint64_t x0, uint64_t x1, const V &b, uint64_t y0, uint64_t y1, Sink &sink);

uint64_t Agree(const uint64_t *a, const uint64_t *b, uint64_t limit) {
    uint64_t k = 0;
    while (k < limit && a[k] == b[k]) ++k;
    return k;
}
uint64_t AgreeBack(const uint64_t *a, const uint64_t *b, uint64_t limit) {
    uint64_t k = 0;
    while (k < limit && a[-1 - int64_t(k)] == b[-1 - int64_t(k)]) ++k;
    return k;
}

// The longest stretch, both sides together, copied to flat arrays before the first search.
// A search step over the trie is two seeks and over a flat copy it is one compare.
// The copy costs 3us at this length on a sparse stretch the trie resolves in a few steps.
// At twice this, sixteen scattered writes in a hundred thousand elements cost seven times as much, since the stretch between two of them is copied.
constexpr uint64_t FlatLimit = 8192;
// The longest stretch copied before the full search.
// A stretch reaching the full search differs at least every other chunk, so the search costs more than the copy up to this length.
constexpr uint64_t SearchFlatLimit = 64 * FlatLimit;

// Diffs `A[x0, x1)` against `B[y0, y1)`, flat copies of the vectors from `ox` and `oy` on.
// Returns false without reporting anything when a shortest script costs more than `budget`.
bool FlatRange(const uint64_t *A, int64_t x0, int64_t x1, const uint64_t *B, int64_t y0, int64_t y1, int64_t budget, uint64_t ox, uint64_t oy, Sink &sink) {
    const auto pre = int64_t(Agree(A + x0, B + y0, uint64_t(std::min(x1 - x0, y1 - y0))));
    x0 += pre;
    y0 += pre;
    const auto post = int64_t(AgreeBack(A + x1, B + y1, uint64_t(std::min(x1 - x0, y1 - y0))));
    x1 -= post;
    y1 -= post;
    if (x0 == x1 && y0 == y1) return true;
    const auto extend = [&](bool ahead, uint64_t x, uint64_t y, uint64_t limit) { return ahead ? Agree(A + x0 + x, B + y0 + y, limit) : AgreeBack(A + x1 - x, B + y1 - y, limit); };
    int64_t sx, sy;
    if (x0 != x1 && y0 != y1 && !(x1 - x0 == 1 && y1 - y0 == 1)) {
        if (!Middle(x1 - x0, y1 - y0, budget, extend, sx, sy)) return false;
        // Each half costs at most the whole, and at most its longer side, so a budget of the longer side never fails.
        const auto whole = std::max(x1 - x0, y1 - y0);
        FlatRange(A, x0, x0 + sx, B, y0, y0 + sy, whole, ox, oy, sink);
        FlatRange(A, x0 + sx, x1, B, y0 + sy, y1, whole, ox, oy, sink);
        return true;
    }
    sink(ox + uint64_t(x0), uint64_t(x1 - x0), oy + uint64_t(y0), uint64_t(y1 - y0));
    return true;
}

// Aligns the stretch on the runs common to both and diffs each gap between them.
// Returns false without reporting anything when there are none.
template<typename V> bool Anchored(const V &a, uint64_t x0, uint64_t x1, const V &b, uint64_t y0, uint64_t y1, Sink &sink) {
    std::vector<Anchor> found;
    Runs(a, x0, x1, b, y0, y1, found);
    auto by = ByRun;
    const auto known = found.size();
    Blocks(a, x0, x1, b, y0, y1, found);
    if (found.size() > known) {
        // Both lists are ordered and disjoint in `b`, so the union is one merge.
        std::inplace_merge(found.begin(), found.begin() + int64_t(known), found.end(), [](const Anchor &p, const Anchor &q) { return p.Y < q.Y; });
        by = ByBlock;
    }
    if (found.empty()) return false;
    // Chosen over every run at once, those found by identity and by content together.
    // Splitting at one run, or chaining identity before content, can pick the larger side of a block moved past one nearly its own size.
    const auto chosen = Chain(found);
    Tally(by);
    // Every gap is shorter than the stretch, since a run is at least a chunk long.
    uint64_t x = x0, y = y0;
    for (const auto i : chosen) {
        const auto &run = found[i];
        DiffRange(a, x, run.X, b, y, run.Y, sink);
        x = run.X + run.Len;
        y = run.Y + run.Len;
    }
    DiffRange(a, x, x1, b, y, y1, sink);
    return true;
}

template<typename V> void DiffRange(const V &a, uint64_t x0, uint64_t x1, const V &b, uint64_t y0, uint64_t y1, Sink &sink) {
    // The common prefix and suffix, which are the whole answer for two vectors related by one write, append, cut, or splice.
    const auto pre = Extend(a, x0, b, y0, std::min(x1 - x0, y1 - y0));
    x0 += pre;
    y0 += pre;
    const auto post = ExtendBack(a, x1, b, y1, std::min(x1 - x0, y1 - y0));
    x1 -= post;
    y1 -= post;
    if (x0 == x1 && y0 == y1) return;
    // One element against one is an overwrite, and a script of one edit has no middle.
    if (x1 - x0 == 1 && y1 - y0 == 1) {
        sink(x0, 1, y0, 1);
        return;
    }
    // One side empty is one run added or removed.
    if (x0 != x1 && y0 != y1) {
        // A stretch of up to `limit` elements is copied to flat arrays and searched there, one compare per step instead of two seeks.
        // One buffer holds both sides end to end, uninitialized since the copy fills it, and is made once for both searches.
        const auto n = x1 - x0, m = y1 - y0;
        std::unique_ptr<uint64_t[]> copies;
        const auto extend = [&](bool ahead, uint64_t x, uint64_t y, uint64_t limit) { return ahead ? Extend(a, x0 + x, b, y0 + y, limit) : ExtendBack(a, x1 - x, b, y1 - y, limit); };
        const auto searched = [&](int64_t budget, uint64_t limit, uint32_t which) {
            if (n + m <= limit) {
                if (!copies) {
                    copies.reset(new uint64_t[n + m]);
                    Copy(a, x0, x1, copies.get());
                    Copy(b, y0, y1, copies.get() + n);
                }
                if (!FlatRange(copies.get(), 0, int64_t(n), copies.get() + n, 0, int64_t(m), budget, x0, y0, sink)) return false;
            } else {
                int64_t sx, sy;
                if (!Middle(int64_t(n), int64_t(m), budget, extend, sx, sy)) return false;
                DiffRange(a, x0, x0 + uint64_t(sx), b, y0, y0 + uint64_t(sy), sink);
                DiffRange(a, x0 + uint64_t(sx), x1, b, y0 + uint64_t(sy), y1, sink);
            }
            Tally(which);
            return true;
        };
        // The search costs the square of the edit count, so three stages, cheapest first.
        // A search with a small budget, then alignment on the runs common to both, which finds a moved block, then a search with the full budget.
        // A stretch past all three is reported as one run, which bounds the work on two unrelated vectors.
        if (searched(Probe, FlatLimit, ByProbe)) return;
        if (Anchored(a, x0, x1, b, y0, y1, sink)) return;
        if (searched(MaxEdits, SearchFlatLimit, BySearch)) return;
        Tally(ByNothing);
    }
    sink(x0, x1 - x0, y0, y1 - y0);
}

// How many elements sit under `n`, or zero for a node that breaks the invariant -- which no well formed
// node returns, every node standing over at least one element.
template<Shape S, typename W> uint64_t CheckNode(const Node *n, uint32_t shift) {
    if (!n->Count || n->Count > Slots) return 0;
    if (!shift) return n->Count;
    const auto *sizes = n->Sizes;
    if (sizes && (!Relaxed(S) || sizes->Count != n->Count)) return 0; // A strict node carries no table.
    const auto *weights = n->Weights;
    if (HasWeight<W> && (!weights || weights->Count != n->Count)) return 0;
    uint64_t total = 0, weight = 0;
    bool strict = true;
    for (uint32_t i = 0, nc = n->Count; i < nc; ++i) {
        const auto under = CheckNode<S, W>(Children(n)[i], shift - Bits);
        if (!under) return 0;
        total += under;
        if (sizes && Values(sizes)[i] != total) return 0;
        if (i + 1 < nc && under != uint64_t{1} << shift) strict = false;
        if constexpr (HasWeight<W>) {
            weight += SubtreeWeight<W>(Children(n)[i], shift - Bits);
            if (Values(weights)[i] != weight) return 0;
        }
    }
    // A table exactly where one is needed, and nowhere else.
    return strict == !sizes ? total : 0;
}

// Down to the leftmost chunk under `n`, stacking what is left of every node passed on the way.
void Descend(Iterator &it, const Node *n) {
    for (auto s = it.Source.Shift - it.Depth * Bits; s; s -= Bits) {
        const auto *const *cs = Children(n);
        it.Stack[it.Depth++] = {cs + 1, cs + n->Count};
        n = cs[0];
    }
    it.Cur = Values(n);
    it.End = it.Cur + n->Count;
}

// Everything let go of, field by field rather than by assignment: what the vector was standing on has
// been released already, and assigning would hand it to a temporary that releases it again.
template<typename V> void Clear(V &v) {
    if (v.Root) Release(v.Root, v.Shift);
    if (v.Tail) Release(v.Tail, 0);
    v.Root = v.Tail = nullptr;
    v.Size = 0;
    v.Shift = 0;
}

// Everything an append does beyond storing into the tail: a fresh tail when there is none or the one
// there is has to be copied, and the trie when the tail has filled. Out of line, and so out of the
// caller, since one push in a chunk's worth reaches it and the caller has to stay small.
template<Shape S, typename W> [[gnu::noinline]] void PushBackSlow(Trie<S, W> &v, uint64_t value) {
    const auto tc = v.Tail ? v.Tail->Count : 0;
    if (tc < Slots) { // A tail that is shared, or not there at all.
        auto *tail = tc ? CopiedLeaf(v.Tail, tc + 1) : Alloc(1);
        Values(tail)[tc] = value;
        if (v.Tail) Release(v.Tail, 0);
        v.Tail = tail;
        ++v.Size;
        return;
    }
    // The tail is full, so it becomes a chunk of the trie and `value` starts the next one.
    const auto *leaf = v.Tail; // Its reference passes to the trie.
    auto *tail = Alloc(1);
    Values(tail)[0] = value;
    v.Tail = tail;
    const auto told = v.Size - Slots; // What the trie holds before the chunk goes in.
    ++v.Size;
    PushChunk<S, W>(v.Root, v.Shift, leaf, told);
}

template<Shape S, typename W> void PushBackMut(Trie<S, W> &v, uint64_t value) {
    // A tail this vector alone holds grows in place, every node having room for a full chunk. That is
    // the whole of a push onto a vector being given up: one store, one count, one size.
    const auto tc = v.Tail ? v.Tail->Count : 0;
    if (!tc || tc == Slots || v.Tail->Refs != 1) return PushBackSlow(v, value);
    Values(Mutable(v.Tail))[tc] = value;
    Mutable(v.Tail)->Count = tc + 1;
    ++v.Size;
}

// The whole of a write to an index the vector already holds. Nothing from here down knows whether the
// caller brought a value or a function of the one being displaced.
template<Shape S, typename W, typename F> void SetWith(Trie<S, W> &v, uint64_t index, F value) {
    const auto told = TailOffset(v);
    if (index >= told) {
        const auto slot = uint32_t(index - told);
        if (v.Tail->Refs == 1) {
            auto &held = Values(Mutable(v.Tail))[slot];
            held = value(held);
            return;
        }
        auto *tail = CopiedLeaf(v.Tail, v.Tail->Count);
        Values(tail)[slot] = value(Values(v.Tail)[slot]);
        Release(v.Tail, 0);
        v.Tail = tail;
        return;
    }
    const auto *root = SetAt<S, W>(v.Root, index, value, v.Shift, v.Root->Refs == 1);
    if (root == v.Root) return; // Written in place, so the vector already stands on the answer.
    Release(v.Root, v.Shift);
    v.Root = root;
}

template<Shape S, typename W> void SetMut(Trie<S, W> &v, uint64_t index, uint64_t value) {
    SetWith(v, index, [value](uint64_t) { return value; });
}
template<Shape S, typename W> void UpdateMut(Trie<S, W> &v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    SetWith(v, index, [fn, context](uint64_t current) { return fn(context, current); });
}

template<Shape S, typename W> void TakeMut(Trie<S, W> &v, uint64_t count) {
    if (count >= v.Size) return;
    if (!count) return Clear(v);
    const auto told = TailOffset(v);
    if (count > told) { // The cut lands in the tail, so the trie is untouched and only the count moves.
        const auto tc = uint32_t(count - told);
        if (v.Tail->Refs == 1) {
            Mutable(v.Tail)->Count = tc;
        } else {
            const auto *tail = CopiedLeaf(v.Tail, tc);
            Release(v.Tail, 0);
            v.Tail = tail;
        }
        v.Size = count;
        return;
    }
    const Node *tail = nullptr;
    const auto *root = Cut<S, W>(v.Root, count, v.Shift, tail);
    auto shift = v.Shift;
    if (root) Collapse(root, shift);
    Release(v.Root, v.Shift);
    Release(v.Tail, 0);
    v.Root = root;
    v.Tail = tail;
    v.Size = count;
    v.Shift = root ? shift : 0;
}

template<typename W> void DropMut(Flex<W> &v, uint64_t count) {
    if (!count) return;
    if (count >= v.Size) return Clear(v);
    const auto told = TailOffset(v);
    if (count >= told) { // Everything the trie holds goes, and the cut lands in the tail.
        const auto keep = uint32_t(v.Size - count);
        auto *tail = Alloc(keep);
        std::memcpy(Values(tail), Values(v.Tail) + (v.Tail->Count - keep), keep * sizeof(uint64_t));
        if (v.Root) Release(v.Root, v.Shift);
        Release(v.Tail, 0);
        v.Root = nullptr;
        v.Tail = tail;
        v.Size = keep;
        v.Shift = 0;
        return;
    }
    const auto *root = Dropped<W>(v.Root, count, v.Shift);
    auto shift = v.Shift;
    Collapse(root, shift);
    Release(v.Root, v.Shift);
    v.Root = root;
    v.Size -= count;
    v.Shift = shift;
}
} // namespace

template<Shape S, typename W> Trie<S, W>::Trie(const Trie &o) : Trie(o.Root, o.Tail, o.Size, o.Shift) { Claim(*this); }
template<Shape S, typename W>
template<Shape O, typename N>
Trie<S, W>::Trie(const Trie<O, N> &o)
    requires Widens<S, W, O, N>
    : Trie(o.Root, o.Tail, o.Size, o.Shift) {
    Claim(*this);
}
// Default-initialized to empty by the member initializers, then handed `o`'s contents, leaving `o` empty
// rather than a vector whose size no longer matches the nodes it gave up.
template<Shape S, typename W> Trie<S, W>::Trie(Trie &&o) noexcept { Swap(*this, o); }
template<Shape S, typename W> Trie<S, W>::~Trie() {
    if (Root) Release(Root, Shift);
    if (Tail) Release(Tail, 0);
}
template<Shape S, typename W> Trie<S, W> &Trie<S, W>::operator=(const Trie &o) { return *this = Trie{o}; }
template<Shape S, typename W> Trie<S, W> &Trie<S, W>::operator=(Trie &&o) noexcept {
    // A write handed an rvalue gives back the same vector, so `v = PushBack(std::move(v), x)` assigns
    // from itself, and the check against self keeps that free.
    if (this != &o) Swap(*this, o);
    return *this;
}

// The one place the two shapes part company at speed, so the strict descent is written out here rather
// than reached through `LeafFor`: every path is the same length and no node is searched, so the index
// stays where it is and a slot never waits on the slot above it. Behind a call that leaves the index
// somewhere a node load could have written, the descent reloads it a level at a time and a random index
// costs a third again as much.
template<Shape S, typename W> const uint64_t &Trie<S, W>::operator[](uint64_t index) const {
    const auto told = TailOffset(*this);
    if (index >= told) return Values(Tail)[index - told];
    if constexpr (Relaxed(S)) {
        return Values(LeafFor(Root, index, Shift))[index];
    } else {
        const auto *n = Root;
        for (auto shift = Shift; shift; shift -= Bits) n = Children(n)[(index >> shift) & Mask];
        return Values(n)[index & Mask];
    }
}

// The two forms of every write, over one implementation. The form that has to leave its argument alone
// retains it first: every node under it is then held twice, so the write copies. Same rule as everywhere
// else here, reached by holding the vector rather than by a flag.
template<Shape S, typename W> Trie<S, W> PushBack(const Trie<S, W> &v, uint64_t value) {
    Trie<S, W> out{v};
    PushBackMut(out, value);
    return out;
}
template<Shape S, typename W> Trie<S, W> &&PushBack(Trie<S, W> &&v, uint64_t value) {
    PushBackMut(v, value);
    return std::move(v);
}
template<Shape S, typename W> Trie<S, W> Set(const Trie<S, W> &v, uint64_t index, uint64_t value) {
    Trie<S, W> out{v};
    SetMut(out, index, value);
    return out;
}
template<Shape S, typename W> Trie<S, W> &&Set(Trie<S, W> &&v, uint64_t index, uint64_t value) {
    SetMut(v, index, value);
    return std::move(v);
}
template<Shape S, typename W> Trie<S, W> Update(const Trie<S, W> &v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    Trie<S, W> out{v};
    UpdateMut(out, index, fn, context);
    return out;
}
template<Shape S, typename W> Trie<S, W> &&Update(Trie<S, W> &&v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    UpdateMut(v, index, fn, context);
    return std::move(v);
}
template<Shape S, typename W> Trie<S, W> Take(const Trie<S, W> &v, uint64_t count) {
    Trie<S, W> out{v};
    TakeMut(out, count);
    return out;
}
template<Shape S, typename W> Trie<S, W> &&Take(Trie<S, W> &&v, uint64_t count) {
    TakeMut(v, count);
    return std::move(v);
}
template<typename W> Flex<W> Drop(const Flex<W> &v, uint64_t count) {
    Flex<W> out{v};
    DropMut(out, count);
    return out;
}
template<typename W> Flex<W> &&Drop(Flex<W> &&v, uint64_t count) {
    DropMut(v, count);
    return std::move(v);
}

template<typename W> Flex<W> Concat(const Flex<W> &a, const Flex<W> &b) {
    if (!a.Size) return b;
    if (!b.Size) return a;
    // Nothing but a tail on the right, and room for it, so the two tails become one and neither trie
    // is touched.
    if (!b.Root && a.Tail->Count + b.Tail->Count <= Slots) {
        auto *tail = CopiedLeaf(a.Tail, a.Tail->Count + b.Tail->Count);
        std::memcpy(Values(tail) + a.Tail->Count, Values(b.Tail), b.Tail->Count * sizeof(uint64_t));
        if (a.Root) Retain(a.Root);
        return {a.Root, tail, a.Size + b.Size, a.Shift};
    }
    // The join is trie to trie, so the left tail goes into the left trie first. The right tail stays
    // where it is and becomes the answer's.
    uint32_t shift;
    const auto *root = Flattened<W>(a, shift);
    Retain(b.Tail);
    if (!b.Root) return {root, b.Tail, a.Size + b.Size, shift};

    const auto *top = Joined<W>(root, shift, b.Root, b.Shift);
    const auto taller = shift > b.Shift ? shift : b.Shift;
    Release(root, shift);
    root = top;
    shift = taller + Bits;
    Collapse(root, shift);
    return {root, b.Tail, a.Size + b.Size, shift};
}

template<typename W> Flex<W> PushFront(const Flex<W> &v, uint64_t value) {
    auto *leaf = Alloc(1);
    Values(leaf)[0] = value;
    return Concat(Flex<W>{nullptr, leaf, 1, 0}, v);
}

template<typename W> Flex<W> Insert(const Flex<W> &v, uint64_t pos, uint64_t value) { return Concat(PushBack(Take(v, pos), value), Drop(v, pos)); }
template<typename W> Flex<W> Insert(const Flex<W> &v, uint64_t pos, const Flex<W> &values) { return Concat(Concat(Take(v, pos), values), Drop(v, pos)); }
template<typename W> Flex<W> Erase(const Flex<W> &v, uint64_t pos) { return Erase(v, pos, pos + 1); }
template<typename W> Flex<W> Erase(const Flex<W> &v, uint64_t first, uint64_t last) {
    if (first >= last) return v;
    return Concat(Take(v, first), Drop(v, last));
}

template<Shape S, typename W> Trie<S, W> Build(const uint64_t *first, const uint64_t *last) {
    const auto n = uint64_t(last - first);
    if (!n) return {};
    const auto told = (n - 1) & ~Mask;
    auto *tail = Alloc(uint32_t(n - told));
    std::memcpy(Values(tail), first + told, (n - told) * sizeof(uint64_t));
    if (!told) return {nullptr, tail, n, 0};

    // The chunks are filled straight from the range, then roofed over a level at a time until one node is
    // left. Each level is written over the one below it, which it is always shorter than.
    auto count = told >> Bits;
    auto *nodes = static_cast<const Node **>(std::malloc(count * sizeof(const Node *)));
    for (uint64_t i = 0; i < count; ++i) {
        auto *leaf = Alloc(Slots);
        std::memcpy(Values(leaf), first + (i << Bits), Slots * sizeof(uint64_t));
        nodes[i] = leaf;
    }
    uint32_t shift = 0;
    for (; count > 1; shift += Bits) {
        const auto parents = (count + Slots - 1) >> Bits;
        for (uint64_t p = 0; p < parents; ++p) {
            const auto from = p << Bits, to = from + Slots < count ? from + Slots : count;
            auto *inner = AllocInner(uint32_t(to - from));
            std::memcpy(static_cast<void *>(Children(inner)), static_cast<const void *>(nodes + from), (to - from) * sizeof(const Node *));
            MeasureWeights<W>(inner, shift + Bits);
            nodes[p] = inner;
        }
        count = parents;
    }
    const auto *root = nodes[0];
    std::free(static_cast<void *>(nodes));
    return {root, tail, n, shift};
}

template<Shape S, typename W> void ForEachChunk(const Trie<S, W> &v, void (*visit)(void *, const uint64_t *, const uint64_t *), void *context) {
    if (!v.Size) return; // An empty vector has no tail, and so calls nothing.
    if (v.Root && !v.Shift) {
        visit(context, Values(v.Root), Values(v.Root) + v.Root->Count);
    } else if (v.Root) {
        // The stack is explicit rather than the call stack's, so nothing recurses and the whole walk
        // inlines into the caller, which turns `visit` into the caller's own loop body.
        //
        // It stops a level above the chunks and loops over them from there, so the stack runs once per
        // node of chunks rather than once per chunk. Worth 4x, most of it because the caller is then
        // left a loop nest the compiler can vectorize.
        Iterator::Frame stack[Iterator::MaxDepth];
        uint32_t depth = 0;
        for (const auto *n = v.Root;;) {
            while ((depth + 1) * Bits < v.Shift) { // Still above the chunks' parents, so take a child.
                const auto *const *cs = Children(n);
                stack[depth++] = {cs + 1, cs + n->Count};
                n = cs[0];
            }
            const auto *const *cs = Children(n);
            for (uint32_t i = 0, nc = n->Count; i < nc; ++i) {
                // Every chunk of a strict trie is full, so none of them is asked how long it is.
                const uint32_t held = Relaxed(S) ? cs[i]->Count : Slots;
                visit(context, Values(cs[i]), Values(cs[i]) + held);
            }
            while (depth && stack[depth - 1].Child == stack[depth - 1].ChildEnd) --depth;
            if (!depth) break;
            n = *stack[depth - 1].Child++;
        }
    }
    visit(context, Values(v.Tail), Values(v.Tail) + v.Tail->Count);
}

template<Shape S, typename W> bool operator==(const Trie<S, W> &a, const Trie<S, W> &b) {
    if (a.Size != b.Size) return false;
    if (!a.Size) return true;
    if (a.Root == b.Root && a.Tail == b.Tail) return true;
    if constexpr (Relaxed(S)) {
        // Equal sizes say nothing about the shapes, so the two are walked side by side and compared over
        // whatever run both have in hand at once.
        auto ia = begin(a), ib = begin(b);
        while (ia.Cur) {
            const auto run = ia.End - ia.Cur < ib.End - ib.Cur ? ia.End - ia.Cur : ib.End - ib.Cur;
            if (std::memcmp(ia.Cur, ib.Cur, size_t(run) * sizeof(uint64_t)) != 0) return false;
            if ((ia.Cur += run) == ia.End) ia.Advance();
            if ((ib.Cur += run) == ib.End) ib.Advance();
        }
        return true;
    } else {
        // Equal sizes mean equal shapes, the trie being a function of the size, so the two walk in step.
        if (std::memcmp(Values(a.Tail), Values(b.Tail), a.Tail->Count * sizeof(uint64_t)) != 0) return false;
        return !a.Root || SameNodes(a.Root, b.Root, a.Shift);
    }
}

template<Shape S, typename W> void Diff(const Trie<S, W> &a, const Trie<S, W> &b, void (*report)(void *, const Change &), void *context) {
    Sink sink{report, context};
    // An empty stretch is never read, so an empty vector's null tail is never sought.
    DiffRange(a, 0, a.Size, b, 0, b.Size, sink);
    sink.Flush();
}

template<typename W> Flex<W> Apply(const Flex<W> &a, const Change *first, const Change *last, const Flex<W> &source) {
    Flex<W> out;
    uint64_t at = 0; // How far into `a` the runs so far reach.
    for (const auto *c = first; c != last; ++c) {
        // Each piece is two cuts and a join, logarithmic, and shares every subtrie it does not straddle.
        out = Concat(out, Take(Drop(a, at), c->Index - at));
        out = Concat(out, Take(Drop(source, c->To), c->Added));
        at = c->Index + c->Removed;
    }
    return Concat(out, Drop(a, at));
}

template<Shape S, typename W> uint64_t Seek(const Trie<S, W> &v, uint64_t k) {
    uint64_t base = 0;
    const auto *n = v.Tail;
    const auto under = v.Root ? SubtreeWeight<W>(v.Root, v.Shift) : 0;
    if (k < under) {
        n = v.Root;
        for (auto shift = v.Shift; shift; shift -= Bits) {
            const auto *totals = Values(n->Weights);
            uint32_t i = 0;
            while (totals[i] <= k) ++i;
            if (i) k -= totals[i - 1];
            base += SlotOffset(n, i, shift);
            n = Children(n)[i];
        }
    } else {
        k -= under;
        base = TailOffset(v);
    }
    const auto *vals = Values(n);
    for (uint32_t i = 0;; ++i) {
        const auto w = W::Of(vals[i]);
        if (w > k) return base + i;
        k -= w;
    }
}

template<Shape S, typename W> uint64_t WeightBefore(const Trie<S, W> &v, uint64_t index) {
    if (!v.Size) return 0;
    const auto told = TailOffset(v);
    if (index >= told) return (v.Root ? SubtreeWeight<W>(v.Root, v.Shift) : 0) + LeafWeight<W>(v.Tail, index - told);
    uint64_t weight = 0, within = index;
    const auto *n = v.Root;
    for (auto shift = v.Shift; shift; shift -= Bits) {
        const auto i = Slot<S>(n, within, shift);
        if (i) weight += Values(n->Weights)[i - 1];
        n = Children(n)[i];
    }
    return weight + LeafWeight<W>(n, within);
}

template<Shape S, typename W> uint64_t Weight(const Trie<S, W> &v) { return WeightBefore(v, v.Size); }

template<Shape S, typename W> bool Check(const Trie<S, W> &v) {
    if (!v.Size) return !v.Root && !v.Tail && v.Shift == 0;
    if (!v.Tail || !v.Tail->Count || v.Tail->Count > Slots) return false;
    const auto told = v.Size - v.Tail->Count;
    // A strict trie holds a whole number of chunks: nothing under it is ever short.
    if (!Relaxed(S) && told % Slots) return false;
    if (!told) return !v.Root && v.Shift == 0;
    if (!v.Root || v.Shift % Bits || v.Shift >= 64) return false; // The root stands on a level boundary.
    if (v.Shift && v.Root->Count < 2) return false; // And no higher than it has to.
    return CheckNode<S, W>(v.Root, v.Shift) == told;
}

std::optional<Phases> DiffPhases() {
#ifdef VECTOR_AUDIT
    return Phases{Settled[ByProbe], Settled[ByRun], Settled[ByBlock], Settled[BySearch], Settled[ByNothing]};
#else
    return {};
#endif
}

std::optional<uint64_t> LiveNodes() {
#ifdef VECTOR_AUDIT
    return Live;
#else
    return {};
#endif
}

std::optional<Footprint> Held() {
#ifdef VECTOR_AUDIT
    return {};
#else
    Footprint f{};
    for (const auto *slab = AllSlabs; slab; slab = slab->All) {
        f.ReservedBytes += SlabBytes;
        if (!slab->Live) continue;
        f.LiveBytes += uint64_t(slab->Live) * NodeBytes;
        f.SpannedBytes += SlabBytes;
    }
    return f;
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
        return;
    }
    if (Rest) { // The trie is walked out, so only the tail is left.
        Cur = Values(Rest);
        End = Cur + Rest->Count;
        Rest = nullptr;
        return;
    }
    Cur = End = nullptr;
}

template<Shape S, typename W> Iterator begin(const Trie<S, W> &v) {
    Iterator it{v}; // The copy is the reference that holds the structure below still.
    if (!v.Size) return it;
    it.Rest = v.Tail;
    if (v.Root) Descend(it, v.Root);
    else it.Advance(); // Nothing but a tail, and `Advance` picks that up once the trie is done.
    return it;
}

// Every shape and weight is compiled here and nowhere else: a header that offered them would have to
// offer the node, and the node is this file's own.
#define PERSISTENT_VECTOR_INSTANTIATE(V, W)                                                                       \
    template struct Trie<V, W>;                                                                                   \
    template Trie<V, W> Build(const uint64_t *, const uint64_t *);                                                \
    template Trie<V, W> PushBack(const Trie<V, W> &, uint64_t);                                                   \
    template Trie<V, W> &&PushBack(Trie<V, W> &&, uint64_t);                                                      \
    template Trie<V, W> Set(const Trie<V, W> &, uint64_t, uint64_t);                                              \
    template Trie<V, W> &&Set(Trie<V, W> &&, uint64_t, uint64_t);                                                 \
    template Trie<V, W> Update(const Trie<V, W> &, uint64_t, uint64_t (*)(void *, uint64_t), void *);             \
    template Trie<V, W> &&Update(Trie<V, W> &&, uint64_t, uint64_t (*)(void *, uint64_t), void *);                \
    template Trie<V, W> Take(const Trie<V, W> &, uint64_t);                                                       \
    template Trie<V, W> &&Take(Trie<V, W> &&, uint64_t);                                                          \
    template void ForEachChunk(const Trie<V, W> &, void (*)(void *, const uint64_t *, const uint64_t *), void *); \
    template bool operator==(const Trie<V, W> &, const Trie<V, W> &);                                             \
    template void Diff(const Trie<V, W> &, const Trie<V, W> &, void (*)(void *, const Change &), void *);         \
    template bool Check(const Trie<V, W> &);                                                                      \
    template Iterator begin(const Trie<V, W> &);
#define PERSISTENT_FLEX_INSTANTIATE(W)                                   \
    template Flex<W> Drop(const Flex<W> &, uint64_t);                    \
    template Flex<W> &&Drop(Flex<W> &&, uint64_t);                       \
    template Flex<W> Concat(const Flex<W> &, const Flex<W> &);           \
    template Flex<W> PushFront(const Flex<W> &, uint64_t);               \
    template Flex<W> Insert(const Flex<W> &, uint64_t, uint64_t);        \
    template Flex<W> Insert(const Flex<W> &, uint64_t, const Flex<W> &); \
    template Flex<W> Erase(const Flex<W> &, uint64_t);                   \
    template Flex<W> Erase(const Flex<W> &, uint64_t, uint64_t);         \
    template Flex<W> Apply(const Flex<W> &, const Change *, const Change *, const Flex<W> &);
PERSISTENT_VECTOR_INSTANTIATE(Shape::Strict, Unweighted)
PERSISTENT_VECTOR_INSTANTIATE(Shape::Relaxed, Unweighted)
PERSISTENT_VECTOR_INSTANTIATE(Shape::Relaxed, Mark<VECTOR_WEIGHT_KEY>)
PERSISTENT_FLEX_INSTANTIATE(Unweighted)
PERSISTENT_FLEX_INSTANTIATE(Mark<VECTOR_WEIGHT_KEY>)
#undef PERSISTENT_VECTOR_INSTANTIATE
#undef PERSISTENT_FLEX_INSTANTIATE
template FlexVector::Trie(const Vector &);
template FlexVector::Trie(const Weighted &);
template uint64_t Weight(const Weighted &);
template uint64_t WeightBefore(const Weighted &, uint64_t);
template uint64_t Seek(const Weighted &, uint64_t);
} // namespace vec
