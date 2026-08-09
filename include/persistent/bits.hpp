#pragma once

// Compile-time constants and bitmap utilities for the CHAMP HAMT.
// Matches immer's detail/hamts/bits.hpp logic.

#include <cstdint>
#include <climits>
#include <type_traits>

namespace persistent {
namespace detail {

using bits_t  = std::uint32_t;
using count_t = std::uint32_t;
using shift_t = std::uint32_t;

// Select the bitmap integer type based on branching factor B.
template <bits_t B>
struct bitmap_type {
    using type = std::uint8_t;
};
template <> struct bitmap_type<4u> { using type = std::uint16_t; };
template <> struct bitmap_type<5u> { using type = std::uint32_t; };
template <> struct bitmap_type<6u> { using type = std::uint64_t; };

template <bits_t B>
using bitmap_t = typename bitmap_type<B>::type;

// Number of branches (children) per node.
template <bits_t B>
inline constexpr count_t branches = count_t{1u} << B;

// Bit-mask for extracting B bits from a hash.
template <bits_t B, typename T = count_t>
inline constexpr T mask = static_cast<T>(branches<B> - 1u);

// Maximum trie depth for a given hash type and branching factor.
// At this depth we must use collision nodes.
template <typename hash_t, bits_t B>
inline constexpr count_t max_depth =
    (static_cast<count_t>(sizeof(hash_t) * CHAR_BIT) + B - 1u) / B;

// Maximum shift value (shift used at the deepest level).
template <typename hash_t, bits_t B>
inline constexpr shift_t max_shift =
    static_cast<shift_t>(max_depth<hash_t, B>) * B;

// Portable popcount (count of set bits).
inline count_t popcount(std::uint8_t  x) noexcept { return static_cast<count_t>(__builtin_popcount(x));  }
inline count_t popcount(std::uint16_t x) noexcept { return static_cast<count_t>(__builtin_popcount(x));  }
inline count_t popcount(std::uint32_t x) noexcept { return static_cast<count_t>(__builtin_popcount(x));  }
inline count_t popcount(std::uint64_t x) noexcept { return static_cast<count_t>(__builtin_popcountll(x)); }

} // namespace detail
} // namespace persistent
