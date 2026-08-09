#pragma once

// persistent::set<T> — immutable hash-set backed by a CHAMP trie.
// Matches immer::set's public API and algorithmic behaviour.

#include "champ.hpp"
#include "bits.hpp"

#include <functional>
#include <initializer_list>

namespace persistent {

template <typename T,
          typename Hash  = std::hash<T>,
          typename Equal = std::equal_to<T>,
          detail::bits_t B = 5u>
class set {
    using engine_t = detail::champ<T, Hash, Equal, B>;

    struct key_of_t {
        const T& operator()(const T& v) const noexcept { return v; }
    };

    struct key_eq_t {
        bool operator()(const T& a, const T& b) const { return Equal{}(a, b); }
    };

public:
    using value_type     = T;
    using size_type      = std::size_t;
    using const_iterator = typename engine_t::iterator;
    using iterator       = const_iterator;

    // -----------------------------------------------------------------------
    // Constructors
    // -----------------------------------------------------------------------
    set() = default;

    set(std::initializer_list<T> il) {
        for (const auto& v : il)
            engine_ = engine_.template insert<T>(v, key_of_t{}, key_eq_t{});
    }

    // -----------------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------------
    size_type size()  const noexcept { return engine_.size(); }
    bool      empty() const noexcept { return engine_.empty(); }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------
    bool count(const T& v) const noexcept {
        return engine_.template get<T>(v, key_of_t{}, key_eq_t{}) != nullptr;
    }

    const T* find(const T& v) const noexcept {
        return engine_.template get<T>(v, key_of_t{}, key_eq_t{});
    }

    // -----------------------------------------------------------------------
    // Modifiers (return a new set; *this is unchanged)
    // -----------------------------------------------------------------------
    [[nodiscard]] set insert(T v) const {
        set s;
        s.engine_ = engine_.template insert<T>(std::move(v), key_of_t{}, key_eq_t{});
        return s;
    }

    [[nodiscard]] set erase(const T& v) const {
        set s;
        s.engine_ = engine_.template erase<T>(v, key_of_t{}, key_eq_t{});
        return s;
    }

    // -----------------------------------------------------------------------
    // Iteration
    // -----------------------------------------------------------------------
    const_iterator begin() const { return engine_.begin(); }
    const_iterator end()   const { return engine_.end(); }

    // -----------------------------------------------------------------------
    // Equality
    // -----------------------------------------------------------------------
    bool operator==(const set& o) const noexcept { return engine_.equals(o.engine_); }
    bool operator!=(const set& o) const noexcept { return !(*this == o); }

private:
    engine_t engine_;
};

} // namespace persistent
