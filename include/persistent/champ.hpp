#pragma once

// CHAMP (Compressed Hash-Array Mapped Prefix-tree) engine.
// Implements persistent lookup, insert, erase, equality, and forward iteration.
// Closely mirrors immer's detail/hamts/champ.hpp.

#include "node.hpp"
#include "bits.hpp"

#include <cassert>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>

namespace persistent {
namespace detail {

// ---------------------------------------------------------------------------
// champ_iterator – depth-first, values-before-children traversal.
// Matches immer's champ_iterator ordering.
// ---------------------------------------------------------------------------
template <typename T, typename Hash, typename Equal, bits_t B>
class champ_iterator {
public:
    using node        = node_t<T, Hash, Equal, B>;
    using inner_type  = inner_t<T, Hash, Equal, B>;
    using coll_type   = collision_t<T>;
    using hash_t      = decltype(Hash{}(std::declval<const T&>()));

    using iterator_category = std::forward_iterator_tag;
    using value_type        = T;
    using reference         = const T&;
    using pointer           = const T*;
    using difference_type   = std::ptrdiff_t;

    champ_iterator() = default;

    explicit champ_iterator(const node* root) {
        push(root);
        advance_to_value();
    }

    reference operator*()  const noexcept { return *cur_; }
    pointer   operator->() const noexcept { return cur_;  }

    champ_iterator& operator++() {
        ++cur_;
        if (cur_ == cur_end_) {
            cur_     = nullptr;
            cur_end_ = nullptr;
            advance_to_value();
        }
        return *this;
    }
    champ_iterator operator++(int) { auto tmp = *this; ++*this; return tmp; }

    bool operator==(const champ_iterator& o) const noexcept { return cur_ == o.cur_; }
    bool operator!=(const champ_iterator& o) const noexcept { return cur_ != o.cur_; }

private:
    struct frame_t {
        const node* n;
        count_t child_idx; // 0 = "values not served yet"; 1+ = "serving children[child_idx-1...]"
    };

    static constexpr count_t kMaxDepth =
        max_depth<decltype(Hash{}(std::declval<const T&>())), B> + 2u;

    frame_t  stack_[kMaxDepth];
    count_t  depth_{0};
    const T* cur_{nullptr};
    const T* cur_end_{nullptr};

    void push(const node* n) {
        assert(depth_ < kMaxDepth);
        stack_[depth_++] = {n, 0};
    }

    void advance_to_value() {
        while (depth_ > 0) {
            auto& top = stack_[depth_ - 1];
            const node* n = top.n;

            if (n->is_collision()) {
                const auto* c = n->collision();
                if (c->count > 0) {
                    cur_     = c->items();
                    cur_end_ = c->items() + c->count;
                }
                --depth_;
                if (cur_) return;
                continue;
            }

            const auto* in = n->inner();

            // Serve inline values first (child_idx == 0 means not yet served).
            if (top.child_idx == 0) {
                top.child_idx = 1; // mark values as "about to be served"
                count_t nv = in->data_count();
                if (nv > 0) {
                    cur_     = in->vals();
                    cur_end_ = in->vals() + nv;
                    return;
                }
            }

            // Then serve children depth-first.
            count_t ci = top.child_idx - 1; // convert to 0-based child index
            count_t nc = in->child_count();
            if (ci < nc) {
                ++top.child_idx;
                push(in->children()[ci]);
                continue;
            }

            --depth_; // node fully visited
        }
    }
};

// ---------------------------------------------------------------------------
// sub_result – result of a delete operation.
// "nothing"   : key not found, trie unchanged.
// "singleton" : trie collapsed to a single item (stored by value for safety).
// "tree"      : new sub-trie node.
// ---------------------------------------------------------------------------
template <typename T, typename Hash, typename Equal, bits_t B>
struct sub_result {
    enum class kind_t { nothing, singleton, tree } kind{kind_t::nothing};
    // Using optional<T> avoids requiring T to be default-constructible.
    std::optional<T> singleton_val;
    node_t<T, Hash, Equal, B>* tree{nullptr};

    sub_result() = default;

    static sub_result make_singleton(T v) {
        sub_result r;
        r.kind          = kind_t::singleton;
        r.singleton_val = std::move(v);
        return r;
    }
    static sub_result make_tree(node_t<T, Hash, Equal, B>* n) {
        sub_result r;
        r.kind = kind_t::tree;
        r.tree = n;
        return r;
    }
};

// ---------------------------------------------------------------------------
// champ – the CHAMP engine.
// ---------------------------------------------------------------------------
template <typename T, typename Hash, typename Equal, bits_t B = 5u>
class champ {
public:
    using node        = node_t<T, Hash, Equal, B>;
    using node_ptr_t  = node_ptr<T, Hash, Equal, B>;
    using hash_t      = decltype(Hash{}(std::declval<const T&>()));
    using bitmap_type = bitmap_t<B>;
    using iterator    = champ_iterator<T, Hash, Equal, B>;
    using sub_res     = sub_result<T, Hash, Equal, B>;

    champ() : root_(node::make_empty_inner()), size_(0) {}
    champ(node* root, std::size_t sz) : root_(root), size_(sz) {}

    std::size_t size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }
    node* root_node()   const noexcept { return root_.get(); }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------
    template <typename K, typename KeyOf, typename KeyEq>
    const T* get(const K& k, const KeyOf& key_of, const KeyEq& key_eq) const noexcept {
        auto hash = Hash{}(k);
        const node* n = root_.get();
        for (shift_t shift = 0; shift < max_shift<hash_t, B>; shift += B) {
            if (n->is_collision()) break;
            const auto* in = n->inner();
            auto idx = static_cast<bitmap_type>((hash >> shift) & mask<B, hash_t>);
            auto bit = bitmap_type{1u} << idx;
            if (in->nodemap & bit) {
                n = in->children()[in->child_index(bit)];
            } else if (in->datamap & bit) {
                const T* v = in->vals() + in->data_index(bit);
                return key_eq(key_of(*v), k) ? v : nullptr;
            } else {
                return nullptr;
            }
        }
        if (n->is_collision()) {
            const auto* c = n->collision();
            const T* items = c->items();
            for (count_t i = 0; i < c->count; ++i)
                if (key_eq(key_of(items[i]), k)) return items + i;
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Insert / update
    // -----------------------------------------------------------------------
    template <typename K, typename KeyOf, typename KeyEq>
    champ insert(T v, const KeyOf& key_of, const KeyEq& key_eq) const {
        auto hash = Hash{}(key_of(v));
        auto [new_root, inserted] = do_add(root_.get(), v, hash, 0, key_of, key_eq);
        return champ(new_root, size_ + (inserted ? 1 : 0));
    }

    // -----------------------------------------------------------------------
    // Erase
    // -----------------------------------------------------------------------
    template <typename K, typename KeyOf, typename KeyEq>
    champ erase(const K& k, const KeyOf& key_of, const KeyEq& key_eq) const {
        auto hash = Hash{}(k);
        auto res = do_sub(root_.get(), k, hash, 0, key_of, key_eq);
        if (res.kind == sub_res::kind_t::nothing) {
            root_.get()->inc();
            return champ(root_.get(), size_);
        } else if (res.kind == sub_res::kind_t::tree) {
            return champ(res.tree, size_ - 1);
        } else {
            // Root collapsed to one item.  Re-wrap in a minimal inner node.
            T sv = std::move(*res.singleton_val);
            auto h = Hash{}(key_of(sv));
            auto idx = static_cast<bitmap_type>(h & mask<B, hash_t>);
            auto bit = bitmap_type{1u} << idx;
            return champ(node::make_inner_1(bit, std::move(sv)), size_ - 1);
        }
    }

    // -----------------------------------------------------------------------
    // Equality
    // -----------------------------------------------------------------------
    bool equals(const champ& other) const noexcept {
        if (size_ != other.size_) return false;
        return equals_node(root_.get(), other.root_.get());
    }

    // -----------------------------------------------------------------------
    // Iteration
    // -----------------------------------------------------------------------
    iterator begin() const { return size_ > 0 ? iterator(root_.get()) : iterator{}; }
    iterator end()   const { return iterator{}; }

private:
    node_ptr_t  root_;
    std::size_t size_{0};

    // -----------------------------------------------------------------------
    // Internal: recursive add. Returns {new_root, was_inserted}.
    // -----------------------------------------------------------------------
    template <typename KeyOf, typename KeyEq>
    std::pair<node*, bool>
    do_add(const node* n, const T& v, hash_t hash, shift_t shift,
           const KeyOf& key_of, const KeyEq& key_eq) const
    {
        if (shift >= max_shift<hash_t, B>) {
            // Must be (or become) a collision node.
            if (n->is_collision()) {
                const auto* c = n->collision();
                for (count_t i = 0; i < c->count; ++i) {
                    if (key_eq(key_of(c->items()[i]), key_of(v)))
                        return {node::copy_collision_replace(n, i, v), false};
                }
                return {node::copy_collision_insert(n, v), true};
            }
            assert(false && "unexpected inner node at max depth");
            return {nullptr, false};
        }

        assert(n->is_inner());
        const auto* in = n->inner();
        auto idx = static_cast<bitmap_type>((hash >> shift) & mask<B, hash_t>);
        auto bit = bitmap_type{1u} << idx;

        if (in->nodemap & bit) {
            count_t ci = in->child_index(bit);
            auto [child, inserted] = do_add(in->children()[ci], v, hash, shift + B, key_of, key_eq);
            return {node::copy_inner_replace_child(n, ci, child), inserted};

        } else if (in->datamap & bit) {
            count_t vi = in->data_index(bit);
            const T* existing = in->vals() + vi;
            if (key_eq(key_of(*existing), key_of(v))) {
                return {node::copy_inner_replace_value(n, vi, v), false};
            }
            auto child = node::make_merged(
                shift + B, v, hash, *existing, Hash{}(key_of(*existing)));
            return {node::copy_inner_replace_val_with_child(n, bit, vi, child), true};

        } else {
            return {node::copy_inner_insert_value(n, bit, v), true};
        }
    }

    // -----------------------------------------------------------------------
    // Internal: recursive sub (delete).
    // -----------------------------------------------------------------------
    template <typename K, typename KeyOf, typename KeyEq>
    sub_res
    do_sub(const node* n, const K& k, hash_t hash, shift_t shift,
           const KeyOf& key_of, const KeyEq& key_eq) const
    {
        if (shift >= max_shift<hash_t, B>) {
            assert(n->is_collision());
            const auto* c = n->collision();
            for (count_t i = 0; i < c->count; ++i) {
                if (key_eq(key_of(c->items()[i]), k)) {
                    if (c->count == 2) {
                        // Collapse to singleton.
                        T sv = c->items()[1 - i];
                        return sub_res::make_singleton(std::move(sv));
                    }
                    return sub_res::make_tree(node::copy_collision_remove(n, i));
                }
            }
            return sub_res{}; // not found
        }

        assert(n->is_inner());
        const auto* in = n->inner();
        auto idx = static_cast<bitmap_type>((hash >> shift) & mask<B, hash_t>);
        auto bit = bitmap_type{1u} << idx;

        if (in->nodemap & bit) {
            count_t ci = in->child_index(bit);
            auto res = do_sub(in->children()[ci], k, hash, shift + B, key_of, key_eq);
            switch (res.kind) {
            case sub_res::kind_t::nothing:
                return sub_res{};
            case sub_res::kind_t::tree:
                return sub_res::make_tree(node::copy_inner_replace_child(n, ci, res.tree));
            case sub_res::kind_t::singleton: {
                T sv = std::move(*res.singleton_val);
                // Propagate singleton upward if this node would collapse to nothing
                // (no inline values, exactly 1 child, not the root).
                if (in->child_count() == 1 && in->data_count() == 0 && shift > 0) {
                    return sub_res::make_singleton(std::move(sv));
                }
                // Inline the singleton as a value in this node.
                auto h_sv  = Hash{}(key_of(sv));
                auto idx_sv = static_cast<bitmap_type>((h_sv >> shift) & mask<B, hash_t>);
                auto bit_sv = bitmap_type{1u} << idx_sv;
                return sub_res::make_tree(
                    node::copy_inner_replace_child_with_val(n, bit_sv, ci, sv));
            }
            }

        } else if (in->datamap & bit) {
            count_t vi = in->data_index(bit);
            if (!key_eq(key_of(in->vals()[vi]), k)) return sub_res{};

            count_t nv = in->data_count();
            count_t nc = in->child_count();

            if (nc > 0 || nv > 2) {
                return sub_res::make_tree(node::copy_inner_remove_value(n, bit, vi));
            }
            if (nv == 2) {
                // Collapse to the other value.
                count_t other = 1 - vi;
                T sv = in->vals()[other];
                if (shift > 0) {
                    return sub_res::make_singleton(std::move(sv));
                }
                // Root: keep as a 1-value inner node.
                return sub_res::make_tree(node::copy_inner_remove_value(n, bit, vi));
            }
            // nv == 1 (and nc == 0): removing the only value.
            return sub_res::make_tree(node::copy_inner_remove_value(n, bit, vi));
        }

        return sub_res{}; // not found
    }

    // -----------------------------------------------------------------------
    // Structural equality.
    // -----------------------------------------------------------------------
    static bool equals_node(const node* a, const node* b) noexcept {
        if (a == b) return true;
        if (a->is_collision() != b->is_collision()) return false;
        if (a->is_collision()) {
            const auto* ca = a->collision();
            const auto* cb = b->collision();
            if (ca->count != cb->count) return false;
            const T* ia = ca->items();
            const T* ib = cb->items();
            // Collision items are stored in insertion order, which can differ
            // between two structurally equivalent nodes.  Use a set-membership
            // check so order does not matter.
            for (count_t i = 0; i < ca->count; ++i) {
                bool found = false;
                for (count_t j = 0; j < cb->count; ++j) {
                    if (Equal{}(ia[i], ib[j])) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }
        const auto* ia = a->inner();
        const auto* ib = b->inner();
        if (ia->datamap != ib->datamap || ia->nodemap != ib->nodemap) return false;
        count_t nv = ia->data_count();
        for (count_t i = 0; i < nv; ++i)
            if (!Equal{}(ia->vals()[i], ib->vals()[i])) return false;
        count_t nc = ia->child_count();
        for (count_t i = 0; i < nc; ++i)
            if (!equals_node(ia->children()[i], ib->children()[i])) return false;
        return true;
    }
};

} // namespace detail
} // namespace persistent
