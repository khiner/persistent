#include "Vector.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <utility>

// Branching factor exponent: 32 slots per node at 5, 64 at 6. CMakeLists.txt has the choice and what
// was measured either way, and this is the same default for a build that does not set it.
#ifndef VECTOR_BITS
#define VECTOR_BITS 6
#endif

namespace vec {
namespace {
constexpr uint32_t Bits = VECTOR_BITS;
static_assert(Bits == 5 || Bits == 6, "The two widths the trade has been measured at");

constexpr uint64_t Mask = (uint64_t{1} << Bits) - 1;
constexpr uint32_t Slots = 1u << Bits;
// How many elements a node standing at `shift` covers.
constexpr uint64_t Covers(uint32_t shift) { return uint64_t{1} << (shift + Bits); }
} // namespace

static_assert(Iterator::MaxDepth >= (64 + Bits - 1) / Bits, "The iterator stack has to hold a whole path");

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
    mutable uint32_t Refs;
    // How many of the slots are taken. Rounding the node up to its alignment leaves room for it, so
    // keeping it costs nothing over deriving it from where the node sits, and a copy reads it straight off.
    uint32_t Count;
};

namespace {
const Node **Children(Node *n) { return n->Children; }
const Node *const *Children(const Node *n) { return n->Children; }
uint64_t *Values(Node *n) { return n->Values; }
const uint64_t *Values(const Node *n) { return n->Values; }

// Nodes are malloc'd, never in fact const. Only reached when the node is uniquely owned.
Node *Mutable(const Node *n) { return const_cast<Node *>(n); }

void Retain(const Node *n) { ++n->Refs; }

// Every node is cut to the same size, a full slot count of pointers being a full slot count of values.
// That is what lets a tail grow in place: the room a push wants is already there.
constexpr uint32_t NodeBytes = uint32_t(sizeof(Node));

// Nodes are cut from slabs, and a freed node goes back to its own slab rather than to the allocator, so
// the memory stays ours until exit. That blinds the sanitizers -- a freed node stays reachable -- so
// -DVECTOR_AUDIT frees to the allocator instead and counts the live ones. Hamt.cpp has the same
// allocator, and why reuse is kept inside a slab.
#ifdef VECTOR_AUDIT
uint64_t Live = 0;
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

Slab *Partial = nullptr, *AllSlabs = nullptr;

Slab *SlabOf(const Node *n) { return reinterpret_cast<Slab *>(reinterpret_cast<uintptr_t>(n) & ~uintptr_t(SlabBytes - 1)); }
#endif

// Carries one reference, which passes to the caller. The slots are left as they were, so `count` of them
// are the caller's to fill.
Node *Alloc(uint32_t count) {
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

// `shift` is the level `n` stands on, which is what says whether its slots hold children.
void Release(const Node *n, uint32_t shift) {
    if (--n->Refs) return;
    if (shift) {
        const auto *const *cs = Children(n);
        for (uint32_t i = 0, nc = n->Count; i < nc; ++i) Release(cs[i], shift - Bits);
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
Node *CopiedLeaf(const Node *n, uint32_t count) {
    auto *c = Alloc(count);
    std::memcpy(Values(c), Values(n), (count < n->Count ? count : n->Count) * sizeof(uint64_t));
    return c;
}

// A fresh inner node standing for `n` with `count` children, the one at `idx` being `child`, which comes
// in with the reference the new node keeps. Every other child is copied by reference, so each is claimed.
// A `count` past what `n` holds appends.
Node *WithChild(const Node *n, uint32_t count, uint32_t idx, const Node *child) {
    auto *c = Alloc(count);
    auto **cs = Children(c);
    const auto *const *src = Children(n);
    std::memcpy(static_cast<void *>(cs), static_cast<const void *>(src), idx * sizeof(const Node *));
    std::memcpy(static_cast<void *>(cs + idx + 1), static_cast<const void *>(src + idx + 1), (count - idx - 1) * sizeof(const Node *));
    for (uint32_t i = 0; i < idx; ++i) Retain(cs[i]);
    for (uint32_t i = idx + 1; i < count; ++i) Retain(cs[i]);
    cs[idx] = child;
    return c;
}

// The levels between `shift` and the leaves, as a chain holding `leaf` at the bottom. One child each,
// since the chunk it carries is the first thing to reach that far.
const Node *NewPath(uint32_t shift, const Node *leaf) {
    for (; shift; shift -= Bits) {
        auto *n = Alloc(1);
        Children(n)[0] = leaf;
        leaf = n;
    }
    return leaf;
}

// The chunk holding `index`, under a node standing at `shift`. Every path is the same length, so the
// walk counts levels down rather than testing for a chunk.
const Node *ChunkAt(const Node *n, uint64_t index, uint32_t shift) {
    for (; shift; shift -= Bits) n = Children(n)[(index >> shift) & Mask];
    return n;
}

// `owned` means every node from the root down to `n` is held exactly once, so writing through it is
// unobservable, and returning `n` leaves the path above untouched.
//
// Puts `leaf` on the end of the subtrie under `n`, which holds `told` elements counted from the start of
// the whole trie. An append only ever touches the rightmost path, so the slot the chunk lands in either
// holds the one partly filled child or holds nothing yet.
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
    return WithChild(n, idx + 1, idx, child);
}

// `value` says what to store given what the index holds now, which is where `Set` and `Update` part
// company. A template and not a pointer, so `Set` compiles as it would have on its own.
template<typename F> const Node *SetAt(const Node *n, uint64_t index, F value, uint32_t shift, bool owned) {
    if (!shift) {
        const auto slot = uint32_t(index & Mask);
        if (!owned) {
            auto *c = CopiedLeaf(n, n->Count);
            Values(c)[slot] = value(Values(n)[slot]);
            return c;
        }
        auto &held = Values(Mutable(n))[slot];
        held = value(held);
        return n;
    }
    const auto idx = uint32_t((index >> shift) & Mask);
    const auto *old = Children(n)[idx];
    const auto *child = SetAt(old, index, value, shift - Bits, owned && old->Refs == 1);
    if (child == old) return n;
    if (!owned) return WithChild(n, n->Count, idx, child);
    Children(Mutable(n))[idx] = child;
    Release(old, shift - Bits);
    return n;
}

// The subtrie under `n` cut down to its first `count` elements, and in `chunk` the one holding the
// element just past the cut, which becomes the new tail. Both come out of the one descent, the cut
// walking the path that chunk sits at the end of. Only the rightmost kept path is copied: every subtrie
// left of it is kept whole and shared.
const Node *Truncated(const Node *n, uint64_t count, uint32_t shift, const Node *&chunk) {
    if (count == Covers(shift)) { // Whole, so it stands as it is, and what follows it is not under here.
        Retain(n);
        return n;
    }
    assert(shift > 0); // The trie holds a whole number of chunks, so a leaf is never half kept.
    const auto keep = uint32_t((count - 1) >> shift) + 1;
    auto *c = Alloc(keep);
    auto **cs = Children(c);
    const auto *const *src = Children(n);
    std::memcpy(static_cast<void *>(cs), static_cast<const void *>(src), (keep - 1) * sizeof(const Node *));
    for (uint32_t i = 0; i + 1 < keep; ++i) Retain(cs[i]);
    cs[keep - 1] = Truncated(src[keep - 1], count - (uint64_t(keep - 1) << shift), shift - Bits, chunk);
    // The cut fell on the end of that child, so what follows it is the first chunk of the child after it.
    if (!chunk && keep < n->Count) chunk = ChunkAt(src[keep], 0, shift - Bits);
    return c;
}

bool SameNodes(const Node *a, const Node *b, uint32_t shift) {
    if (a == b) return true; // Structure the two vectors share settles a whole subtrie at once.
    if (a->Count != b->Count) return false;
    if (!shift) return std::memcmp(Values(a), Values(b), a->Count * sizeof(uint64_t)) == 0;
    const auto *const *ca = Children(a);
    const auto *const *cb = Children(b);
    for (uint32_t i = 0, nc = a->Count; i < nc; ++i)
        if (!SameNodes(ca[i], cb[i], shift - Bits)) return false;
    return true;
}

// `count` is how many elements sit under `n`, which fixes its shape: every child but the last is full,
// and the last holds what is left.
bool CheckNode(const Node *n, uint64_t count, uint32_t shift) {
    if (!shift) return n->Count == Slots && count == Slots; // Every chunk in the trie is a full one.
    const auto keep = uint32_t((count - 1) >> shift) + 1;
    if (n->Count != keep) return false;
    for (uint32_t i = 0; i + 1 < keep; ++i)
        if (!CheckNode(Children(n)[i], uint64_t{1} << shift, shift - Bits)) return false;
    return CheckNode(Children(n)[keep - 1], count - (uint64_t(keep - 1) << shift), shift - Bits);
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

void Swap(Vector &a, Vector &b) noexcept {
    std::swap(a.Root, b.Root);
    std::swap(a.Tail, b.Tail);
    std::swap(a.Size, b.Size);
    std::swap(a.Shift, b.Shift);
}
} // namespace

Vector::Vector(const Vector &o) : Root(o.Root), Tail(o.Tail), Size(o.Size), Shift(o.Shift) {
    if (Root) Retain(Root);
    if (Tail) Retain(Tail);
}
// Default-initialized to empty by the member initializers, then handed `o`'s contents, leaving `o` empty
// rather than a vector whose size no longer matches the nodes it gave up.
Vector::Vector(Vector &&o) noexcept { Swap(*this, o); }
Vector::~Vector() {
    if (Root) Release(Root, Shift);
    if (Tail) Release(Tail, 0);
}
Vector &Vector::operator=(const Vector &o) { return *this = Vector{o}; }
Vector &Vector::operator=(Vector &&o) noexcept {
    // A write handed an rvalue gives back the same vector, so `v = PushBack(std::move(v), x)` assigns
    // from itself, and this is what makes that free.
    if (this != &o) Swap(*this, o);
    return *this;
}

namespace {
// Where the trie ends and the tail begins. The trie holds a whole number of chunks, so this is the size
// with its low bits cleared.
uint64_t TailOffset(uint64_t size) { return (size - 1) & ~Mask; }

// The whole of a write to an index the vector already holds. Nothing from here down knows whether the
// caller brought a value or a function of the one being displaced.
template<typename F> void SetWith(Vector &v, uint64_t index, F value) {
    const auto told = TailOffset(v.Size);
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
    const auto *root = SetAt(v.Root, index, value, v.Shift, v.Root->Refs == 1);
    if (root == v.Root) return; // Written in place, so the vector already stands on the answer.
    Release(v.Root, v.Shift);
    v.Root = root;
}

void SetMut(Vector &v, uint64_t index, uint64_t value) {
    SetWith(v, index, [value](uint64_t) { return value; });
}
void UpdateMut(Vector &v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    SetWith(v, index, [fn, context](uint64_t current) { return fn(context, current); });
}

// Everything an append does beyond storing into the tail: a fresh tail when there is none or the one
// there is has to be copied, and the trie when the tail has filled. Out of line, and so out of the
// caller, since one push in a chunk's worth reaches it and the caller is what has to stay small.
[[gnu::noinline]] void PushBackSlow(Vector &v, uint64_t value) {
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
    if (!told) { // The first full tail is the whole trie.
        v.Root = leaf;
        v.Shift = 0;
        return;
    }
    if (told == Covers(v.Shift)) { // No room under the root, so a level goes on top of it.
        auto *root = Alloc(2);
        Children(root)[0] = v.Root;
        Children(root)[1] = NewPath(v.Shift, leaf);
        v.Root = root;
        v.Shift += Bits;
        return;
    }
    const auto *root = PushLeaf(v.Root, leaf, told, v.Shift, v.Root->Refs == 1);
    if (root == v.Root) return;
    Release(v.Root, v.Shift);
    v.Root = root;
}

void PushBackMut(Vector &v, uint64_t value) {
    // A tail this vector alone holds grows in place, every node having room for a full chunk. That is
    // the whole of a push onto a vector being given up: one store, one count, one size.
    const auto tc = v.Tail ? v.Tail->Count : 0;
    if (!tc || tc == Slots || v.Tail->Refs != 1) return PushBackSlow(v, value);
    Values(Mutable(v.Tail))[tc] = value;
    Mutable(v.Tail)->Count = tc + 1;
    ++v.Size;
}

void TakeMut(Vector &v, uint64_t count) {
    if (count >= v.Size) return;
    if (!count) {
        // Field by field rather than by assignment, here and below: what the vector was standing on has
        // been let go of already, and assigning would hand it to a temporary that lets go of it again.
        if (v.Root) Release(v.Root, v.Shift);
        Release(v.Tail, 0);
        v.Root = v.Tail = nullptr;
        v.Size = 0;
        v.Shift = 0;
        return;
    }
    const auto told = TailOffset(v.Size), ntold = TailOffset(count);
    if (ntold == told) {
        // The cut lands in the tail, so the trie is untouched and only the count moves.
        const auto tc = uint32_t(count - ntold);
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
    // Levels the shorter trie no longer needs go first, so the cut starts where the new root will stand.
    const auto *n = v.Root;
    auto shift = v.Shift;
    while (shift && ntold <= (uint64_t{1} << shift)) {
        n = Children(n)[0];
        shift -= Bits;
    }
    const Node *chunk = nullptr;
    const auto *root = ntold ? Truncated(n, ntold, shift, chunk) : nullptr;
    // Dropping levels can leave the new tail's chunk outside what was cut, and a trie cut away entirely
    // never held it. Then it is wherever the index says, which is one descent of the trie as it stands.
    if (!chunk) chunk = ChunkAt(v.Root, ntold, v.Shift);
    // The chunk is kept whole only when the cut falls on its end. A shorter one is copied, the trie it
    // came out of being still held.
    const auto tc = uint32_t(count - ntold);
    const Node *tail = chunk;
    if (tc == Slots) Retain(chunk);
    else tail = CopiedLeaf(chunk, tc);

    Release(v.Root, v.Shift);
    Release(v.Tail, 0);
    v.Root = root;
    v.Tail = tail;
    v.Size = count;
    v.Shift = ntold ? shift : 0;
}
} // namespace

// The two forms of every write, over one implementation. The form that has to leave its argument alone
// retains it first: every node under it is then held twice, which is what makes the write copy. Same
// rule as everywhere else here, reached by holding the vector rather than by a flag.
Vector PushBack(const Vector &v, uint64_t value) {
    Vector out{v};
    PushBackMut(out, value);
    return out;
}
Vector &&PushBack(Vector &&v, uint64_t value) {
    PushBackMut(v, value);
    return std::move(v);
}
Vector Set(const Vector &v, uint64_t index, uint64_t value) {
    Vector out{v};
    SetMut(out, index, value);
    return out;
}
Vector &&Set(Vector &&v, uint64_t index, uint64_t value) {
    SetMut(v, index, value);
    return std::move(v);
}
Vector Update(const Vector &v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    Vector out{v};
    UpdateMut(out, index, fn, context);
    return out;
}
Vector &&Update(Vector &&v, uint64_t index, uint64_t (*fn)(void *, uint64_t), void *context) {
    UpdateMut(v, index, fn, context);
    return std::move(v);
}
Vector Take(const Vector &v, uint64_t count) {
    Vector out{v};
    TakeMut(out, count);
    return out;
}
Vector &&Take(Vector &&v, uint64_t count) {
    TakeMut(v, count);
    return std::move(v);
}

Vector Build(const uint64_t *first, const uint64_t *last) {
    const auto n = uint64_t(last - first);
    if (!n) return {};
    const auto told = TailOffset(n);
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
            auto *inner = Alloc(uint32_t(to - from));
            std::memcpy(static_cast<void *>(Children(inner)), static_cast<const void *>(nodes + from), (to - from) * sizeof(const Node *));
            nodes[p] = inner;
        }
        count = parents;
    }
    const auto *root = nodes[0];
    std::free(static_cast<void *>(nodes));
    return {root, tail, n, shift};
}

const uint64_t &Vector::operator[](uint64_t index) const {
    const auto told = TailOffset(Size);
    if (index >= told) return Values(Tail)[index - told];
    return Values(ChunkAt(Root, index, Shift))[index & Mask];
}

const uint64_t *Get(const Vector &v, uint64_t index) { return index < v.Size ? &v[index] : nullptr; }

void ForEachChunk(const Vector &v, void (*visit)(void *, const uint64_t *, const uint64_t *), void *context) {
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
            // Every chunk in the trie is full, so none of them is asked how long it is.
            for (uint32_t i = 0, nc = n->Count; i < nc; ++i) visit(context, Values(cs[i]), Values(cs[i]) + Slots);
            while (depth && stack[depth - 1].Child == stack[depth - 1].ChildEnd) --depth;
            if (!depth) break;
            n = *stack[depth - 1].Child++;
        }
    }
    visit(context, Values(v.Tail), Values(v.Tail) + v.Tail->Count);
}

bool operator==(const Vector &a, const Vector &b) {
    if (a.Size != b.Size) return false;
    if (!a.Size) return true;
    if (a.Root == b.Root && a.Tail == b.Tail) return true;
    if (std::memcmp(Values(a.Tail), Values(b.Tail), a.Tail->Count * sizeof(uint64_t)) != 0) return false;
    // Equal sizes mean equal shapes, the trie being a function of the size, so the two walk in step.
    return !a.Root || SameNodes(a.Root, b.Root, a.Shift);
}

bool Check(const Vector &v) {
    if (!v.Size) return !v.Root && !v.Tail && v.Shift == 0;
    if (!v.Tail) return false;
    const auto told = TailOffset(v.Size);
    if (v.Tail->Count != v.Size - told) return false;
    if (!told) return !v.Root && v.Shift == 0;
    if (!v.Root || v.Shift % Bits || v.Shift >= 64) return false; // The root stands on a level boundary.
    // The root covers the trie and stands no higher than it has to.
    if (told > Covers(v.Shift)) return false;
    if (v.Shift && told <= (uint64_t{1} << v.Shift)) return false;
    return CheckNode(v.Root, told, v.Shift);
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
    if (Rest) { // The trie is walked out, so the tail is what is left.
        Cur = Values(Rest);
        End = Cur + Rest->Count;
        Rest = nullptr;
        return;
    }
    Cur = End = nullptr;
}

Iterator begin(const Vector &v) {
    Iterator it{v}; // The copy is the reference that holds the structure below still.
    if (!v.Size) return it;
    it.Rest = v.Tail;
    if (v.Root) Descend(it, v.Root);
    else it.Advance(); // Nothing but a tail, which is what `Advance` picks up once the trie is done.
    return it;
}
} // namespace vec
