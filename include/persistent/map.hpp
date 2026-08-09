#pragma once

// persistent::map<K, T> — immutable hash-map backed by a CHAMP trie.
// Matches immer::map's public API and algorithmic behaviour.

#include "champ.hpp"
#include "bits.hpp"

#include <functional>
#include <optional>
#include <utility>

namespace persistent {

template <typename K,
          typename T,
          typename Hash  = std::hash<K>,
          typename Equal = std::equal_to<K>,
          detail::bits_t B = 5u>
class map {
    // The value type stored inside the trie is std::pair<K, T>.
    // We hash/compare by key only, matching immer's map behaviour.
    using value_t = std::pair<K, T>;

    // Adaptor: Hash(pair) → Hash(first).
    struct pair_hash {
        using hash_t = decltype(Hash{}(std::declval<const K&>()));
        hash_t operator()(const value_t& p) const { return Hash{}(p.first); }
        hash_t operator()(const K& k)       const { return Hash{}(k); }
    };

    // Adaptor: Equal(pair, pair) → Equal(first, first) AND equal(second, second).
    // Used by champ::equals_node for structural equality (both key and value must match).
    struct pair_equal {
        bool operator()(const value_t& a, const value_t& b) const {
            return Equal{}(a.first, b.first) && a.second == b.second;
        }
    };

    // Functor to extract a key from a stored pair.
    struct key_of_t {
        const K& operator()(const value_t& p) const noexcept { return p.first; }
    };

    // Functor to compare a stored-pair key with an external key.
    struct key_eq_t {
        bool operator()(const K& a, const K& b) const { return Equal{}(a, b); }
    };

    using engine_t = detail::champ<value_t, pair_hash, pair_equal, B>;

public:
    using key_type        = K;
    using mapped_type     = T;
    using value_type      = value_t;
    using size_type       = std::size_t;
    using const_iterator  = typename engine_t::iterator;
    using iterator        = const_iterator;

    // -----------------------------------------------------------------------
    // Constructors
    // -----------------------------------------------------------------------
    map() = default;

    map(std::initializer_list<value_type> il) {
        for (auto& kv : il)
            engine_ = engine_.template insert<K>(kv, key_of_t{}, key_eq_t{});
    }

    // -----------------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------------
    size_type size()  const noexcept { return engine_.size(); }
    bool      empty() const noexcept { return engine_.empty(); }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------
    const T* find(const K& k) const noexcept {
        const value_t* p = engine_.template get<K>(k, key_of_t{}, key_eq_t{});
        return p ? &p->second : nullptr;
    }

    bool count(const K& k) const noexcept { return find(k) != nullptr; }

    const T& at(const K& k) const {
        const T* p = find(k);
        if (!p) throw std::out_of_range("persistent::map::at");
        return *p;
    }

    // -----------------------------------------------------------------------
    // Modifiers (all return a new map; *this is unchanged)
    // -----------------------------------------------------------------------
    [[nodiscard]] map insert(K k, T v) const {
        map m;
        m.engine_ = engine_.template insert<K>(
            value_t{std::move(k), std::move(v)}, key_of_t{}, key_eq_t{});
        return m;
    }

    [[nodiscard]] map set(K k, T v) const { return insert(std::move(k), std::move(v)); }

    [[nodiscard]] map erase(const K& k) const {
        map m;
        m.engine_ = engine_.template erase<K>(k, key_of_t{}, key_eq_t{});
        return m;
    }

    // -----------------------------------------------------------------------
    // Iteration
    // -----------------------------------------------------------------------
    const_iterator begin() const { return engine_.begin(); }
    const_iterator end()   const { return engine_.end(); }

    // -----------------------------------------------------------------------
    // Equality
    // -----------------------------------------------------------------------
    bool operator==(const map& o) const noexcept { return engine_.equals(o.engine_); }
    bool operator!=(const map& o) const noexcept { return !(*this == o); }

private:
    engine_t engine_;
};

} // namespace persistent
