#pragma once

// CHAMP node: inner nodes (two bitmaps + values + child pointers) and
// collision nodes (linear list of items sharing the same full hash).
//
// Memory layout matches immer's detail/hamts/node.hpp:
//   - Inner node  : single allocation holding bitmaps + child-pointer array.
//                   Values are in a *separate* reference-counted allocation so
//                   that copying children and copying values are independent.
//   - Collision   : single allocation holding count + trailing item array.
//
// Both node types share reference counting via an intrusive atomic count.

#include "bits.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace persistent {
namespace detail {

// ---------------------------------------------------------------------------
// Reference count helpers
// ---------------------------------------------------------------------------
struct refs_t {
    mutable std::atomic<count_t> count{1};
    void inc() const noexcept { count.fetch_add(1, std::memory_order_relaxed); }
    // Returns true when the count reaches zero (caller must deallocate).
    bool dec() const noexcept { return count.fetch_sub(1, std::memory_order_acq_rel) == 1; }
    bool unique() const noexcept { return count.load(std::memory_order_relaxed) == 1; }
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
template <typename T, typename Hash, typename Equal, bits_t B>
struct node_t;

// ---------------------------------------------------------------------------
// Values block – separately reference-counted array of T.
// Layout: [refs_t][T[n]]
// ---------------------------------------------------------------------------
template <typename T>
struct values_t {
    refs_t refs{};

    // trailing T[] follow immediately in memory (accessed via data())
    T* data() noexcept {
        return reinterpret_cast<T*>(
            reinterpret_cast<std::byte*>(this) + sizeof(values_t));
    }
    const T* data() const noexcept {
        return reinterpret_cast<const T*>(
            reinterpret_cast<const std::byte*>(this) + sizeof(values_t));
    }

    static values_t* make(count_t n) {
        auto bytes = sizeof(values_t) + sizeof(T) * n;
        auto* p = ::operator new(bytes);
        return ::new (p) values_t{};
    }

    // Construct a values block as a copy of [src, src+n), possibly replacing
    // the element at position `pos` with `v`.
    static values_t* make_copy(const T* src, count_t n,
                               count_t pos, const T& v) {
        auto* vb = make(n);
        auto* dst = vb->data();
        for (count_t i = 0; i < n; ++i) {
            if (i == pos) ::new (dst + i) T(v);
            else          ::new (dst + i) T(src[i]);
        }
        return vb;
    }

    // Construct a values block inserting `v` at position `pos` (shifting right).
    static values_t* make_insert(const T* src, count_t n,
                                 count_t pos, const T& v) {
        auto* vb = make(n + 1);
        auto* dst = vb->data();
        for (count_t i = 0; i < pos; ++i) ::new (dst + i) T(src[i]);
        ::new (dst + pos) T(v);
        for (count_t i = pos; i < n; ++i) ::new (dst + i + 1) T(src[i]);
        return vb;
    }

    // Construct a values block removing element at position `pos`.
    static values_t* make_remove(const T* src, count_t n, count_t pos) {
        assert(n > 0);
        auto* vb = make(n - 1);
        auto* dst = vb->data();
        for (count_t i = 0; i < pos; ++i)      ::new (dst + i)     T(src[i]);
        for (count_t i = pos + 1; i < n; ++i)  ::new (dst + i - 1) T(src[i]);
        return vb;
    }

    void inc() const noexcept { refs.inc(); }
    bool dec() const noexcept { return refs.dec(); }
    bool unique() const noexcept { return refs.unique(); }

    void destroy(count_t n) {
        auto* p = data();
        for (count_t i = 0; i < n; ++i) p[i].~T();
        this->~values_t();
        ::operator delete(this);
    }
};

// ---------------------------------------------------------------------------
// Inner-node body – stored at the head of a node_t allocation.
// Child pointers trail immediately after in memory.
// Layout: [refs_t][inner_t][node_t* children[n]]
// ---------------------------------------------------------------------------
template <typename T, typename Hash, typename Equal, bits_t B>
struct inner_t {
    using bitmap_type = bitmap_t<B>;
    using node_type   = node_t<T, Hash, Equal, B>;

    bitmap_type datamap{0}; // slots with inline values
    bitmap_type nodemap{0}; // slots with child nodes
    values_t<T>* values{nullptr};

    count_t data_count()  const noexcept { return popcount(datamap); }
    count_t child_count() const noexcept { return popcount(nodemap); }

    // Physical index of value for logical bit `bit`.
    count_t data_index(bitmap_type bit)  const noexcept { return popcount(datamap & (bit - 1u)); }
    // Physical index of child for logical bit `bit`.
    count_t child_index(bitmap_type bit) const noexcept { return popcount(nodemap & (bit - 1u)); }

    T* vals() noexcept { return values ? values->data() : nullptr; }
    const T* vals() const noexcept { return values ? values->data() : nullptr; }

    node_type** children() noexcept {
        return reinterpret_cast<node_type**>(
            reinterpret_cast<std::byte*>(this) + sizeof(inner_t));
    }
    // Returns pointer to the (const) array; the node_type* elements are non-const.
    node_type* const* children() const noexcept {
        return reinterpret_cast<node_type* const*>(
            reinterpret_cast<const std::byte*>(this) + sizeof(inner_t));
    }
};

// ---------------------------------------------------------------------------
// Collision-node body.
// Layout: [refs_t][collision_t][T items[count]]
// ---------------------------------------------------------------------------
template <typename T>
struct collision_t {
    count_t count{0};

    T* items() noexcept {
        return reinterpret_cast<T*>(
            reinterpret_cast<std::byte*>(this) + sizeof(collision_t));
    }
    const T* items() const noexcept {
        return reinterpret_cast<const T*>(
            reinterpret_cast<const std::byte*>(this) + sizeof(collision_t));
    }
};

// ---------------------------------------------------------------------------
// node_t – the unified node wrapper.
//
// Allocation layout (shared prefix):
//   [refs_t][kind_flag (1 byte)][inner_t OR collision_t][trailing data]
//
// We encode the node kind in a flag byte that precedes the body so we can
// always determine the type without out-of-band information.  (immer uses
// depth-based inference in release builds; we keep an explicit tag for
// safety.)
// ---------------------------------------------------------------------------

enum class node_kind : std::uint8_t { inner = 0, collision = 1 };

template <typename T, typename Hash, typename Equal, bits_t B>
struct node_t {
    using hash_t      = decltype(Hash{}(std::declval<const T&>()));
    using bitmap_type = bitmap_t<B>;
    using inner_type  = inner_t<T, Hash, Equal, B>;
    using coll_type   = collision_t<T>;
    using self_t      = node_t;

    refs_t    refs;
    node_kind kind;
    // inner_t or collision_t follow immediately (placed by placement-new in make_*)

    // --- accessors ---
    bool is_inner()     const noexcept { return kind == node_kind::inner; }
    bool is_collision() const noexcept { return kind == node_kind::collision; }

    inner_type* inner() noexcept {
        return reinterpret_cast<inner_type*>(
            reinterpret_cast<std::byte*>(this) + inner_offset());
    }
    const inner_type* inner() const noexcept {
        return reinterpret_cast<const inner_type*>(
            reinterpret_cast<const std::byte*>(this) + inner_offset());
    }
    coll_type* collision() noexcept {
        return reinterpret_cast<coll_type*>(
            reinterpret_cast<std::byte*>(this) + coll_offset());
    }
    const coll_type* collision() const noexcept {
        return reinterpret_cast<const coll_type*>(
            reinterpret_cast<const std::byte*>(this) + coll_offset());
    }

    void inc() const noexcept { refs.inc(); }
    bool dec() const noexcept { return refs.dec(); }
    bool unique() const noexcept { return refs.unique(); }

    // --- layout offsets ---
    static constexpr std::size_t inner_offset() noexcept {
        // Align inner_t after refs+kind.
        constexpr auto base = sizeof(refs_t) + sizeof(node_kind);
        constexpr auto align = alignof(inner_type);
        return (base + align - 1u) & ~(align - 1u);
    }
    static constexpr std::size_t coll_offset() noexcept {
        constexpr auto base = sizeof(refs_t) + sizeof(node_kind);
        constexpr auto align = alignof(coll_type);
        return (base + align - 1u) & ~(align - 1u);
    }
    static constexpr std::size_t children_offset() noexcept {
        return inner_offset() + sizeof(inner_type);
    }
    static constexpr std::size_t items_offset() noexcept {
        return coll_offset() + sizeof(coll_type);
    }

    // Size of an inner-node allocation with `n` children.
    static std::size_t inner_alloc_size(count_t n) noexcept {
        return children_offset() + sizeof(self_t*) * n;
    }
    // Size of a collision-node allocation with `n` items.
    static std::size_t coll_alloc_size(count_t n) noexcept {
        return items_offset() + sizeof(T) * n;
    }

    // --- Allocation helpers ---

    // Allocate and placement-new an inner node with `nchildren` child slots.
    // The values pointer is NOT set here; caller must set it if needed.
    static node_t* alloc_inner(count_t nchildren) {
        auto* p = ::operator new(inner_alloc_size(nchildren));
        auto* n = ::new (p) node_t{};
        n->kind = node_kind::inner;
        ::new (n->inner()) inner_type{};
        // Zero-initialise child pointer slots.
        auto** ch = n->inner()->children();
        for (count_t i = 0; i < nchildren; ++i) ch[i] = nullptr;
        return n;
    }

    static node_t* alloc_collision(count_t nitems) {
        auto* p = ::operator new(coll_alloc_size(nitems));
        auto* n = ::new (p) node_t{};
        n->kind = node_kind::collision;
        ::new (n->collision()) coll_type{};
        n->collision()->count = nitems;
        return n;
    }

    // ---------------------------------------------------------------------------
    // Factory helpers – create nodes from scratch or copy existing ones.
    // ---------------------------------------------------------------------------

    // Empty inner node (used as an initial root).
    static node_t* make_empty_inner() {
        return alloc_inner(0);
    }

    // Inner node containing a single inline value.
    static node_t* make_inner_1(bitmap_type bit, T v) {
        auto* n = alloc_inner(0);
        auto* in = n->inner();
        in->datamap = bit;
        in->nodemap = 0;
        in->values = values_t<T>::make(1);
        ::new (in->values->data()) T(std::move(v));
        return n;
    }

    // Inner node containing two inline values at different logical slots.
    static node_t* make_inner_2(bitmap_type bit1, T v1, bitmap_type bit2, T v2) {
        auto* n = alloc_inner(0);
        auto* in = n->inner();
        in->datamap = bit1 | bit2;
        in->nodemap = 0;
        in->values = values_t<T>::make(2);
        auto* d = in->values->data();
        // Store in ascending bit order (popcount ordering).
        if (bit1 < bit2) {
            ::new (d + 0) T(std::move(v1));
            ::new (d + 1) T(std::move(v2));
        } else {
            ::new (d + 0) T(std::move(v2));
            ::new (d + 1) T(std::move(v1));
        }
        return n;
    }

    // Collision node with two items.
    static node_t* make_collision_2(T a, T b) {
        auto* n = alloc_collision(2);
        auto* items = n->collision()->items();
        ::new (items + 0) T(std::move(a));
        ::new (items + 1) T(std::move(b));
        return n;
    }

    // Recursively build a sub-trie to merge two items that share hash bits.
    // `shift` is the shift to apply at this level (multiple of B).
    static node_t* make_merged(shift_t shift, T v1, hash_t hash1, T v2, hash_t hash2) {
        if (shift >= max_shift<hash_t, B>) {
            // All hash bits exhausted – true collision.
            return make_collision_2(std::move(v1), std::move(v2));
        }
        auto idx1 = static_cast<bitmap_type>((hash1 >> shift) & mask<B, hash_t>);
        auto idx2 = static_cast<bitmap_type>((hash2 >> shift) & mask<B, hash_t>);
        auto bit1 = bitmap_type{1u} << idx1;
        auto bit2 = bitmap_type{1u} << idx2;
        if (bit1 == bit2) {
            // Same slot at this level – recurse deeper.
            auto* child = make_merged(shift + B, std::move(v1), hash1, std::move(v2), hash2);
            auto* n = alloc_inner(1);
            auto* in = n->inner();
            in->nodemap = bit1;
            in->datamap = 0;
            in->values = nullptr;
            in->children()[0] = child;
            return n;
        } else {
            // Different slots – create a 2-value inner node.
            return make_inner_2(bit1, std::move(v1), bit2, std::move(v2));
        }
    }

    // ---------------------------------------------------------------------------
    // Copy helpers (persistent COW operations on inner nodes).
    // ---------------------------------------------------------------------------

    // Copy inner node, replacing child at physical index `ci` with `child`.
    static node_t* copy_inner_replace_child(const node_t* src, count_t ci, node_t* child) {
        const auto* in = src->inner();
        count_t nc = in->child_count();
        auto* dst = alloc_inner(nc);
        auto* din = dst->inner();
        din->datamap = in->datamap;
        din->nodemap = in->nodemap;
        // Share values array.
        if (in->values) {
            in->values->inc();
            din->values = in->values;
        }
        auto* sc = in->children();
        auto* dc = din->children();
        for (count_t i = 0; i < nc; ++i) {
            if (i == ci) {
                dc[i] = child; // new child already has refcount 1
            } else {
                sc[i]->inc();
                dc[i] = sc[i];
            }
        }
        return dst;
    }

    // Copy inner node, replacing value at physical index `vi` with `v`.
    static node_t* copy_inner_replace_value(const node_t* src, count_t vi, T v) {
        const auto* in = src->inner();
        count_t nv = in->data_count();
        count_t nc = in->child_count();
        auto* dst = alloc_inner(nc);
        auto* din = dst->inner();
        din->datamap = in->datamap;
        din->nodemap = in->nodemap;
        din->values = values_t<T>::make_copy(in->vals(), nv, vi, v);
        auto* sc = in->children();
        auto* dc = din->children();
        for (count_t i = 0; i < nc; ++i) { sc[i]->inc(); dc[i] = sc[i]; }
        return dst;
    }

    // Copy inner node, inserting a new value at position `vi` and updating datamap.
    static node_t* copy_inner_insert_value(const node_t* src, bitmap_type bit, T v) {
        const auto* in = src->inner();
        count_t nv = in->data_count();
        count_t nc = in->child_count();
        count_t vi = in->data_index(bit);
        auto* dst = alloc_inner(nc);
        auto* din = dst->inner();
        din->datamap = in->datamap | bit;
        din->nodemap = in->nodemap;
        din->values = values_t<T>::make_insert(in->vals(), nv, vi, v);
        auto* sc = in->children();
        auto* dc = din->children();
        for (count_t i = 0; i < nc; ++i) { sc[i]->inc(); dc[i] = sc[i]; }
        return dst;
    }

    // Copy inner node, removing value at position `vi` and updating datamap.
    static node_t* copy_inner_remove_value(const node_t* src, bitmap_type bit, count_t vi) {
        const auto* in = src->inner();
        count_t nv = in->data_count();
        count_t nc = in->child_count();
        auto* dst = alloc_inner(nc);
        auto* din = dst->inner();
        din->datamap = in->datamap & ~bit;
        din->nodemap = in->nodemap;
        din->values = values_t<T>::make_remove(in->vals(), nv, vi);
        auto* sc = in->children();
        auto* dc = din->children();
        for (count_t i = 0; i < nc; ++i) { sc[i]->inc(); dc[i] = sc[i]; }
        return dst;
    }

    // Copy inner node, replacing an inline value (at `vi`) with a child node
    // (updating datamap and nodemap accordingly, and inserting child at `ci`).
    static node_t* copy_inner_replace_val_with_child(
        const node_t* src, bitmap_type bit, count_t vi, node_t* child)
    {
        const auto* in = src->inner();
        count_t nv = in->data_count();
        count_t nc = in->child_count();
        count_t ci = in->child_index(bit);  // position for new child in existing nodemap
        auto* dst = alloc_inner(nc + 1);
        auto* din = dst->inner();
        din->datamap = in->datamap & ~bit;
        din->nodemap = in->nodemap | bit;
        din->values = values_t<T>::make_remove(in->vals(), nv, vi);
        auto* sc = in->children();
        auto* dc = din->children();
        for (count_t i = 0; i < ci; ++i)      { sc[i]->inc(); dc[i] = sc[i]; }
        dc[ci] = child;
        for (count_t i = ci; i < nc; ++i)     { sc[i]->inc(); dc[i + 1] = sc[i]; }
        return dst;
    }

    // Copy inner node, replacing a child at index `ci` with an inline value
    // (the child collapsed to a singleton during deletion).
    static node_t* copy_inner_replace_child_with_val(
        const node_t* src, bitmap_type bit, count_t ci, const T& v)
    {
        const auto* in = src->inner();
        count_t nv = in->data_count();
        count_t nc = in->child_count();
        count_t vi = in->data_index(bit);  // insertion position in new values
        auto* dst = alloc_inner(nc - 1);
        auto* din = dst->inner();
        din->datamap = in->datamap | bit;
        din->nodemap = in->nodemap & ~bit;
        din->values = values_t<T>::make_insert(in->vals(), nv, vi, v);
        auto* sc = in->children();
        auto* dc = din->children();
        for (count_t i = 0; i < ci; ++i)          { sc[i]->inc(); dc[i] = sc[i]; }
        for (count_t i = ci + 1; i < nc; ++i)     { sc[i]->inc(); dc[i - 1] = sc[i]; }
        return dst;
    }

    // Copy collision node, appending a new item.
    static node_t* copy_collision_insert(const node_t* src, T v) {
        const auto* c = src->collision();
        count_t n = c->count;
        auto* dst = alloc_collision(n + 1);
        auto* dc = dst->collision();
        dc->count = n + 1;
        auto* si = c->items();
        auto* di = dc->items();
        for (count_t i = 0; i < n; ++i) ::new (di + i) T(si[i]);
        ::new (di + n) T(std::move(v));
        return dst;
    }

    // Copy collision node, replacing item at position `pos`.
    static node_t* copy_collision_replace(const node_t* src, count_t pos, T v) {
        const auto* c = src->collision();
        count_t n = c->count;
        auto* dst = alloc_collision(n);
        auto* dc = dst->collision();
        dc->count = n;
        auto* si = c->items();
        auto* di = dc->items();
        for (count_t i = 0; i < n; ++i) {
            if (i == pos) ::new (di + i) T(std::move(v));
            else          ::new (di + i) T(si[i]);
        }
        return dst;
    }

    // Copy collision node, removing item at position `pos`.
    static node_t* copy_collision_remove(const node_t* src, count_t pos) {
        const auto* c = src->collision();
        count_t n = c->count;
        assert(n >= 2);
        auto* dst = alloc_collision(n - 1);
        auto* dc = dst->collision();
        dc->count = n - 1;
        auto* si = c->items();
        auto* di = dc->items();
        for (count_t i = 0, j = 0; i < n; ++i)
            if (i != pos) ::new (di + j++) T(si[i]);
        return dst;
    }

    // ---------------------------------------------------------------------------
    // Memory management
    // ---------------------------------------------------------------------------

    static void delete_inner(node_t* n) noexcept {
        auto* in = n->inner();
        if (in->values && in->values->dec())
            in->values->destroy(in->data_count());
        in->~inner_type();
        n->~node_t();
        ::operator delete(n);
    }

    static void delete_collision(node_t* n) noexcept {
        auto* c = n->collision();
        auto* items = c->items();
        for (count_t i = 0; i < c->count; ++i) items[i].~T();
        c->~coll_type();
        n->~node_t();
        ::operator delete(n);
    }

    // Recursively decrement refcounts; free when they reach zero.
    // `depth` runs from 0 at the root.
    static void dec_recursive(node_t* p, count_t depth) {
        if (!p->dec()) return;
        if (p->is_collision()) {
            delete_collision(p);
        } else {
            auto* in = p->inner();
            auto** ch = in->children();
            count_t nc = in->child_count();
            for (count_t i = 0; i < nc; ++i)
                dec_recursive(ch[i], depth + 1);
            delete_inner(p);
        }
    }
};

// ---------------------------------------------------------------------------
// RAII node pointer – decrements refcount on destruction.
// ---------------------------------------------------------------------------
template <typename T, typename Hash, typename Equal, bits_t B>
struct node_ptr {
    using node = node_t<T, Hash, Equal, B>;
    node* p{nullptr};

    node_ptr() = default;
    explicit node_ptr(node* n) noexcept : p(n) {}
    node_ptr(const node_ptr& o) noexcept : p(o.p) { if (p) p->inc(); }
    node_ptr(node_ptr&& o) noexcept : p(o.p) { o.p = nullptr; }
    node_ptr& operator=(const node_ptr& o) noexcept {
        if (&o != this) {
            if (p) node::dec_recursive(p, 0);
            p = o.p;
            if (p) p->inc();
        }
        return *this;
    }
    node_ptr& operator=(node_ptr&& o) noexcept {
        if (&o != this) {
            if (p) node::dec_recursive(p, 0);
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    ~node_ptr() { if (p) node::dec_recursive(p, 0); }
    node* get() const noexcept { return p; }
    node* operator->() const noexcept { return p; }
    node& operator*() const noexcept { return *p; }
    // Release ownership without decrementing refcount.
    node* release() noexcept { auto* tmp = p; p = nullptr; return tmp; }
};

} // namespace detail
} // namespace persistent
