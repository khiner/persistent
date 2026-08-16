#include "Vector.h"

#include "Node.h"

#include <cassert>
#include <cstring>
#include <utility>

namespace vec {
static_assert(Iterator::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

namespace {
constexpr bool Relaxed(Shape s) { return s == Shape::Relaxed; }

// A second reference to each node the trie stands on: what a copy and the widening both come to.
template<Shape S> void Claim(const Trie<S> &v) {
    if (v.Root) Retain(v.Root);
    if (v.Tail) Retain(v.Tail);
}

template<Shape S> void Swap(Trie<S> &a, Trie<S> &b) noexcept {
    std::swap(a.Root, b.Root);
    std::swap(a.Tail, b.Tail);
    std::swap(a.Size, b.Size);
    std::swap(a.Shift, b.Shift);
}

// Where the trie ends and the tail begins. A strict trie holds a whole number of chunks, so the size
// with its low bits cleared says this without reaching for the tail.
template<Shape S> uint64_t TailOffset(const Trie<S> &v) {
    if constexpr (Relaxed(S)) return v.Size - v.Tail->Count;
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
void Measure(Node *c, uint32_t shift) {
    uint64_t each[Slots];
    for (uint32_t i = 0, nc = c->Count; i < nc; ++i) each[i] = SubtreeSize(Children(c)[i], shift - Bits);
    SetSizes(c, each, c->Count, shift);
}

// A fresh inner node standing for `n` with `count` children, the one at `idx` being `child`, which comes
// in with the reference the new node keeps. Every other child is copied by reference, so each is
// claimed. A `count` past what `n` holds appends. A write moves no element, so the copy carries the size
// table it came with rather than working one out -- and an append never has one, being strict.
template<Shape S> Node *WithChild(const Node *n, uint32_t count, uint32_t idx, const Node *child) {
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
    return c;
}

// A fresh node at `shift` holding `n`'s first `keep` children, with `last` in place of the final one --
// or, when `last` is null, that child unchanged. `last` comes in with the reference the new node keeps,
// and every other child is claimed.
template<Shape S> Node *WithPrefix(const Node *n, uint32_t keep, const Node *last, uint32_t shift) {
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
        if (!n->Sizes) return c; // Every child but the last was full, and cutting the last leaves that so.
        // Nothing before the cut moved, so the totals carry over and only the last entry is worked out.
        const auto *totals = Values(n->Sizes);
        const uint64_t span = uint64_t{1} << shift;
        uint32_t full = 0;
        while (full + 1 < keep && totals[full] == uint64_t(full + 1) * span) ++full;
        if (full + 1 == keep) return c; // Short only past the cut, so the copy needs no table at all.
        auto *sizes = Alloc(keep);
        std::memcpy(Values(sizes), totals, (keep - 1) * sizeof(uint64_t));
        Values(sizes)[keep - 1] = last ? (keep > 1 ? totals[keep - 2] : 0) + SubtreeSize(last, shift - Bits) : totals[keep - 1];
        c->Sizes = sizes;
    }
    return c;
}

// The mirror: `n`'s children from `from` on, with `first` in place of the one at `from`, or that child
// unchanged when `first` is null. Only the relaxed shape can lose its front, so only it comes here.
Node *WithSuffix(const Node *n, uint32_t from, const Node *first, uint32_t shift) {
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
        if (full + 1 >= keep) return c; // Nothing short but the last child, so the copy is strict.
        auto *sizes = Alloc(keep);
        auto *out = Values(sizes);
        const uint64_t dropped = totals[from] - head;
        out[0] = head;
        for (uint32_t i = 1; i < keep; ++i) out[i] = totals[from + i] - dropped;
        c->Sizes = sizes;
        return c;
    }
    // Nothing but the first child can be short, `n` having been full to its own last.
    uint64_t each[Slots];
    each[0] = head;
    for (uint32_t i = 1; i < keep; ++i) each[i] = ChildSize(n, from + i, shift);
    SetSizes(c, each, keep, shift);
    return c;
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
const Node *PushLeaf(const Node *n, const Node *leaf, uint64_t told, uint32_t shift, bool owned) {
    const auto idx = uint32_t((told >> shift) & Mask);
    // Nothing below this level is half filled exactly when the elements divide evenly by what a child
    // covers, and then the slot is one no chunk has reached.
    const bool fresh = (told & ((uint64_t{1} << shift) - 1)) == 0;
    const auto *old = fresh ? nullptr : Children(n)[idx];
    const auto *child = fresh ? NewPath(shift - Bits, leaf) : PushLeaf(old, leaf, told, shift - Bits, owned && old->Refs == 1);
    if (owned) {
        if (child == old) return n;
        if (old) Release(old, shift - Bits);
        Children(Mutable(n))[idx] = child;
        Mutable(n)->Count = idx + 1;
        return n;
    }
    return WithChild<Shape::Strict>(n, idx + 1, idx, child);
}

// The same where nothing says where the rightmost path runs, so it is walked to the bottom and the chunk
// placed on the way back up. Null when there is no room under `n` and the caller has to grow a level.
const Node *PushLeaf(const Node *n, const Node *leaf, uint32_t shift, bool owned) {
    const uint32_t last = n->Count - 1;
    const Node *grown = nullptr;
    if (shift > Bits) {
        const auto *below = Children(n)[last];
        grown = PushLeaf(below, leaf, shift - Bits, owned && below->Refs == 1);
    }
    if (!grown && n->Count == Slots) return nullptr; // Slot-full to the right all the way down.
    const uint32_t count = grown ? n->Count : n->Count + 1, idx = count - 1;
    const auto *child = grown ? grown : NewPath(shift - Bits, leaf);

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
    return c;
}

// The trie with `leaf` on the end of it, where a filled tail goes. `told` is what the trie held before
// it. `root` may be null, and both it and `leaf` come in with the reference the answer keeps.
template<Shape S> void PushChunk(const Node *&root, uint32_t &shift, const Node *leaf, uint64_t told) {
    if (!root) { // The first full tail is the whole trie.
        root = leaf;
        shift = 0;
        return;
    }
    // A strict trie is full exactly when its elements fill the root, and a relaxed one has to be asked.
    // Either way a root that is one lone chunk falls straight through to the level that goes on top.
    const Node *grown = nullptr;
    if constexpr (Relaxed(S)) {
        if (shift) grown = PushLeaf(root, leaf, shift, root->Refs == 1);
    } else {
        if (told != Covers(shift)) grown = PushLeaf(root, leaf, told, shift, root->Refs == 1);
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
    Children(top)[1] = NewPath(shift, leaf);
    if constexpr (Relaxed(S)) {
        const uint64_t each[]{SubtreeSize(root, shift), leaf->Count};
        SetSizes(top, each, 2, shift + Bits);
    }
    root = top;
    shift += Bits;
}

// `value` says what to store given what the index holds now: the one thing `Set` and `Update` differ
// in. A template and not a pointer, so `Set` compiles as it would have on its own.
template<Shape S, typename F> const Node *SetAt(const Node *n, uint64_t index, F value, uint32_t shift, bool owned) {
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
    const auto *child = SetAt<S>(old, index, value, shift - Bits, owned && old->Refs == 1);
    if (child == old) return n;
    if (!owned) return WithChild<S>(n, n->Count, idx, child);
    Children(Mutable(n))[idx] = child;
    Release(old, shift - Bits);
    return n;
}

// The subtrie under `n` cut to its first `count` elements, null when nothing is left of it, with the
// leaf that ends the cut handed back in `tail` rather than kept. Only the rightmost kept path is copied:
// every subtrie left of it is kept whole and shared.
template<Shape S> const Node *Cut(const Node *n, uint64_t count, uint32_t shift, const Node *&tail) {
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
    const auto *child = Cut<S>(Children(n)[idx], within + 1, shift - Bits, tail);
    if (!child && !idx) return nullptr;
    return WithPrefix<S>(n, child ? idx + 1 : idx, child, shift);
}

// The subtrie under `n` with its first `count` elements dropped, `count` being neither none of it nor
// all of it. Only the leftmost kept path is copied.
const Node *Dropped(const Node *n, uint64_t count, uint32_t shift) {
    if (!shift) {
        auto *c = Alloc(n->Count - uint32_t(count));
        std::memcpy(Values(c), Values(n) + count, (n->Count - count) * sizeof(uint64_t));
        return c;
    }
    uint64_t within = count;
    const auto idx = Slot<Shape::Relaxed>(n, within, shift);
    const auto *child = within ? Dropped(Children(n)[idx], within, shift - Bits) : nullptr;
    return WithSuffix(n, idx, child, shift);
}

// How many nodes a joined run may take beyond the fewest that would do. Bagwell and Rompf's `e`, and
// the same 2 immer uses.
constexpr uint32_t Extra = 2;

// `center` is the join of what sat under the last child of `left` and the first child of `right`, all
// three standing at `shift`. Their children -- `left`'s bar its last, `center`'s, `right`'s bar its
// first -- are one run at `shift - Bits`, and this hands back a node at `shift + Bits` holding that run
// repacked into one or two children. `left` and `right` may be null, and all three are borrowed.
const Node *Rebalance(const Node *left, const Node *center, const Node *right, uint32_t shift) {
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
        if (below) Measure(c, below);
        packed[i] = c;
    }

    // Roofed over twice: one node per `Slots` of the run, and one node over those.
    const uint32_t roofs = (count + Slots - 1) / Slots;
    auto *top = AllocInner(roofs);
    for (uint32_t p = 0; p < roofs; ++p) {
        const uint32_t from = p * Slots, to = from + Slots < count ? from + Slots : count;
        auto *node = AllocInner(to - from);
        std::memcpy(static_cast<void *>(Children(node)), static_cast<const void *>(packed + from), (to - from) * sizeof(const Node *));
        Measure(node, shift);
        Children(top)[p] = node;
    }
    Measure(top, shift + Bits);
    return top;
}

// The join of two subtries, as a node standing one level above the taller of them and holding one or
// two children at that level. Neither may be empty.
const Node *Joined(const Node *left, uint32_t lshift, const Node *right, uint32_t rshift) {
    if (lshift > rshift) {
        const auto *center = Joined(Children(left)[left->Count - 1], lshift - Bits, right, rshift);
        const auto *out = Rebalance(left, center, nullptr, lshift);
        Release(center, lshift);
        return out;
    }
    if (lshift < rshift) {
        const auto *center = Joined(left, lshift, Children(right)[0], rshift - Bits);
        const auto *out = Rebalance(nullptr, center, right, rshift);
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
        return top;
    }
    const auto *center = Joined(Children(left)[left->Count - 1], lshift - Bits, Children(right)[0], rshift - Bits);
    const auto *out = Rebalance(left, center, right, lshift);
    Release(center, lshift);
    return out;
}

// Everything `v` holds as one trie, its tail folded in. Carries a reference.
const Node *Flattened(const FlexVector &v, uint32_t &shift) {
    if (!v.Root) {
        Retain(v.Tail);
        shift = 0;
        return v.Tail;
    }
    const auto *root = v.Root;
    Retain(root);
    Retain(v.Tail);
    shift = v.Shift;
    PushChunk<Shape::Relaxed>(root, shift, v.Tail, TailOffset(v));
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

// How many elements sit under `n`, or zero for a node that breaks the invariant -- which no well formed
// node returns, every node standing over at least one element.
template<Shape S> uint64_t CheckNode(const Node *n, uint32_t shift) {
    if (!n->Count || n->Count > Slots) return 0;
    if (!shift) return n->Count;
    const auto *sizes = n->Sizes;
    if (sizes && (!Relaxed(S) || sizes->Count != n->Count)) return 0; // A strict node carries no table.
    uint64_t total = 0;
    bool strict = true;
    for (uint32_t i = 0, nc = n->Count; i < nc; ++i) {
        const auto under = CheckNode<S>(Children(n)[i], shift - Bits);
        if (!under) return 0;
        total += under;
        if (sizes && Values(sizes)[i] != total) return 0;
        if (i + 1 < nc && under != uint64_t{1} << shift) strict = false;
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
template<Shape S> void Clear(Trie<S> &v) {
    if (v.Root) Release(v.Root, v.Shift);
    if (v.Tail) Release(v.Tail, 0);
    v.Root = v.Tail = nullptr;
    v.Size = 0;
    v.Shift = 0;
}

// Everything an append does beyond storing into the tail: a fresh tail when there is none or the one
// there is has to be copied, and the trie when the tail has filled. Out of line, and so out of the
// caller, since one push in a chunk's worth reaches it and the caller has to stay small.
template<Shape S> [[gnu::noinline]] void PushBackSlow(Trie<S> &v, uint64_t value) {
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
    PushChunk<S>(v.Root, v.Shift, leaf, told);
}

template<Shape S> void PushBackMut(Trie<S> &v, uint64_t value) {
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
template<Shape S, typename F> void SetWith(Trie<S> &v, uint64_t index, F value) {
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
    const auto *root = SetAt<S>(v.Root, index, value, v.Shift, v.Root->Refs == 1);
    if (root == v.Root) return; // Written in place, so the vector already stands on the answer.
    Release(v.Root, v.Shift);
    v.Root = root;
}

template<Shape S> void SetMut(Trie<S> &v, uint64_t index, uint64_t value) {
    SetWith(v, index, [value](uint64_t) { return value; });
}
template<Shape S> void UpdateMut(Trie<S> &v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    SetWith(v, index, [fn, context](uint64_t current) { return fn(context, current); });
}

template<Shape S> void TakeMut(Trie<S> &v, uint64_t count) {
    if (count >= v.Size) return;
    if (!count) return Clear(v);
    const auto told = TailOffset(v);
    if (count > told) { // The cut lands in the tail, so the trie is untouched and only the count moves.
        const auto tc = uint32_t(count - told);
        if (v.Tail->Refs == 1) {
            Mutable(v.Tail)->Count = tc;
        } else {
            auto *tail = CopiedLeaf(v.Tail, tc);
            Release(v.Tail, 0);
            v.Tail = tail;
        }
        v.Size = count;
        return;
    }
    const Node *tail = nullptr;
    const auto *root = Cut<S>(v.Root, count, v.Shift, tail);
    auto shift = v.Shift;
    if (root) Collapse(root, shift);
    Release(v.Root, v.Shift);
    Release(v.Tail, 0);
    v.Root = root;
    v.Tail = tail;
    v.Size = count;
    v.Shift = root ? shift : 0;
}

void DropMut(FlexVector &v, uint64_t count) {
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
    const auto *root = Dropped(v.Root, count, v.Shift);
    auto shift = v.Shift;
    Collapse(root, shift);
    Release(v.Root, v.Shift);
    v.Root = root;
    v.Size -= count;
    v.Shift = shift;
}
} // namespace

template<Shape S> Trie<S>::Trie(const Trie &o) : Trie(o.Root, o.Tail, o.Size, o.Shift) { Claim(*this); }
template<Shape S>
Trie<S>::Trie(const Trie<Shape::Strict> &o)
    requires(S == Shape::Relaxed)
    : Trie(o.Root, o.Tail, o.Size, o.Shift) {
    Claim(*this);
}
// Default-initialized to empty by the member initializers, then handed `o`'s contents, leaving `o` empty
// rather than a vector whose size no longer matches the nodes it gave up.
template<Shape S> Trie<S>::Trie(Trie &&o) noexcept { Swap(*this, o); }
template<Shape S> Trie<S>::~Trie() {
    if (Root) Release(Root, Shift);
    if (Tail) Release(Tail, 0);
}
template<Shape S> Trie<S> &Trie<S>::operator=(const Trie &o) { return *this = Trie{o}; }
template<Shape S> Trie<S> &Trie<S>::operator=(Trie &&o) noexcept {
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
template<Shape S> const uint64_t &Trie<S>::operator[](uint64_t index) const {
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
template<Shape S> Trie<S> PushBack(const Trie<S> &v, uint64_t value) {
    Trie<S> out{v};
    PushBackMut(out, value);
    return out;
}
template<Shape S> Trie<S> &&PushBack(Trie<S> &&v, uint64_t value) {
    PushBackMut(v, value);
    return std::move(v);
}
template<Shape S> Trie<S> Set(const Trie<S> &v, uint64_t index, uint64_t value) {
    Trie<S> out{v};
    SetMut(out, index, value);
    return out;
}
template<Shape S> Trie<S> &&Set(Trie<S> &&v, uint64_t index, uint64_t value) {
    SetMut(v, index, value);
    return std::move(v);
}
template<Shape S> Trie<S> Update(const Trie<S> &v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    Trie<S> out{v};
    UpdateMut(out, index, fn, context);
    return out;
}
template<Shape S> Trie<S> &&Update(Trie<S> &&v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    UpdateMut(v, index, fn, context);
    return std::move(v);
}
template<Shape S> Trie<S> Take(const Trie<S> &v, uint64_t count) {
    Trie<S> out{v};
    TakeMut(out, count);
    return out;
}
template<Shape S> Trie<S> &&Take(Trie<S> &&v, uint64_t count) {
    TakeMut(v, count);
    return std::move(v);
}
FlexVector Drop(const FlexVector &v, uint64_t count) {
    FlexVector out{v};
    DropMut(out, count);
    return out;
}
FlexVector &&Drop(FlexVector &&v, uint64_t count) {
    DropMut(v, count);
    return std::move(v);
}

FlexVector Concat(const FlexVector &a, const FlexVector &b) {
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
    const auto *root = Flattened(a, shift);
    Retain(b.Tail);
    if (!b.Root) return {root, b.Tail, a.Size + b.Size, shift};

    const auto *top = Joined(root, shift, b.Root, b.Shift);
    const auto taller = shift > b.Shift ? shift : b.Shift;
    Release(root, shift);
    root = top;
    shift = taller + Bits;
    Collapse(root, shift);
    return {root, b.Tail, a.Size + b.Size, shift};
}

FlexVector PushFront(const FlexVector &v, uint64_t value) {
    auto *leaf = Alloc(1);
    Values(leaf)[0] = value;
    return Concat(FlexVector{nullptr, leaf, 1, 0}, v);
}

FlexVector Insert(const FlexVector &v, uint64_t pos, uint64_t value) { return Concat(PushBack(Take(v, pos), value), Drop(v, pos)); }
FlexVector Insert(const FlexVector &v, uint64_t pos, const FlexVector &values) { return Concat(Concat(Take(v, pos), values), Drop(v, pos)); }
FlexVector Erase(const FlexVector &v, uint64_t pos) { return Erase(v, pos, pos + 1); }
FlexVector Erase(const FlexVector &v, uint64_t first, uint64_t last) {
    if (first >= last) return v;
    return Concat(Take(v, first), Drop(v, last));
}

template<Shape S> Trie<S> Build(const uint64_t *first, const uint64_t *last) {
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
            nodes[p] = inner;
        }
        count = parents;
    }
    const auto *root = nodes[0];
    std::free(static_cast<void *>(nodes));
    return {root, tail, n, shift};
}

template<Shape S> void ForEachChunk(const Trie<S> &v, void (*visit)(void *, const uint64_t *, const uint64_t *), void *context) {
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

template<Shape S> bool operator==(const Trie<S> &a, const Trie<S> &b) {
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

template<Shape S> bool Check(const Trie<S> &v) {
    if (!v.Size) return !v.Root && !v.Tail && v.Shift == 0;
    if (!v.Tail || !v.Tail->Count || v.Tail->Count > Slots) return false;
    const auto told = v.Size - v.Tail->Count;
    // A strict trie holds a whole number of chunks: nothing under it is ever short.
    if (!Relaxed(S) && told % Slots) return false;
    if (!told) return !v.Root && v.Shift == 0;
    if (!v.Root || v.Shift % Bits || v.Shift >= 64) return false; // The root stands on a level boundary.
    if (v.Shift && v.Root->Count < 2) return false; // And no higher than it has to.
    return CheckNode<S>(v.Root, v.Shift) == told;
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

template<Shape S> Iterator begin(const Trie<S> &v) {
    Iterator it{v}; // The copy is the reference that holds the structure below still.
    if (!v.Size) return it;
    it.Rest = v.Tail;
    if (v.Root) Descend(it, v.Root);
    else it.Advance(); // Nothing but a tail, and `Advance` picks that up once the trie is done.
    return it;
}

// Both shapes are compiled here and nowhere else: a header that offered them would have to offer the
// node, and the node is this file's own.
template struct Trie<Shape::Strict>;
template struct Trie<Shape::Relaxed>;
template Vector Build(const uint64_t *, const uint64_t *);
template FlexVector Build(const uint64_t *, const uint64_t *);
template Vector PushBack(const Vector &, uint64_t);
template FlexVector PushBack(const FlexVector &, uint64_t);
template Vector &&PushBack(Vector &&, uint64_t);
template FlexVector &&PushBack(FlexVector &&, uint64_t);
template Vector Set(const Vector &, uint64_t, uint64_t);
template FlexVector Set(const FlexVector &, uint64_t, uint64_t);
template Vector &&Set(Vector &&, uint64_t, uint64_t);
template FlexVector &&Set(FlexVector &&, uint64_t, uint64_t);
template Vector Update(const Vector &, uint64_t, uint64_t (*)(void *, uint64_t), void *);
template FlexVector Update(const FlexVector &, uint64_t, uint64_t (*)(void *, uint64_t), void *);
template Vector &&Update(Vector &&, uint64_t, uint64_t (*)(void *, uint64_t), void *);
template FlexVector &&Update(FlexVector &&, uint64_t, uint64_t (*)(void *, uint64_t), void *);
template Vector Take(const Vector &, uint64_t);
template FlexVector Take(const FlexVector &, uint64_t);
template Vector &&Take(Vector &&, uint64_t);
template FlexVector &&Take(FlexVector &&, uint64_t);
template void ForEachChunk(const Vector &, void (*)(void *, const uint64_t *, const uint64_t *), void *);
template void ForEachChunk(const FlexVector &, void (*)(void *, const uint64_t *, const uint64_t *), void *);
template bool operator==(const Vector &, const Vector &);
template bool operator==(const FlexVector &, const FlexVector &);
template bool Check(const Vector &);
template bool Check(const FlexVector &);
template Iterator begin(const Vector &);
template Iterator begin(const FlexVector &);
} // namespace vec
