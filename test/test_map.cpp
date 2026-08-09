#include <catch2/catch_test_macros.hpp>

#include <persistent/map.hpp>

#include <immer/map.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
template <typename M>
std::vector<std::pair<typename M::key_type, typename M::mapped_type>>
to_sorted_vec(const M& m) {
    std::vector<std::pair<typename M::key_type, typename M::mapped_type>> v;
    for (const auto& kv : m) v.emplace_back(kv.first, kv.second);
    std::sort(v.begin(), v.end());
    return v;
}

// ---------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------
TEST_CASE("map: empty", "[map]") {
    persistent::map<int, int> m;
    REQUIRE(m.size() == 0);
    REQUIRE(m.empty());
}

TEST_CASE("map: single insert and lookup", "[map]") {
    auto m = persistent::map<int, int>{}.insert(42, 100);
    REQUIRE(m.size() == 1);
    REQUIRE(!m.empty());
    REQUIRE(m.count(42));
    REQUIRE(*m.find(42) == 100);
    REQUIRE(!m.count(99));
}

TEST_CASE("map: insert is persistent", "[map]") {
    auto m0 = persistent::map<int, int>{};
    auto m1 = m0.insert(1, 10);
    auto m2 = m1.insert(2, 20);

    REQUIRE(m0.size() == 0);
    REQUIRE(m1.size() == 1);
    REQUIRE(m2.size() == 2);

    REQUIRE(!m0.count(1));
    REQUIRE(m1.count(1));
    REQUIRE(*m1.find(1) == 10);
    REQUIRE(!m1.count(2));
    REQUIRE(m2.count(1));
    REQUIRE(m2.count(2));
}

TEST_CASE("map: update existing key", "[map]") {
    auto m0 = persistent::map<int, int>{}.insert(1, 10);
    auto m1 = m0.insert(1, 99);
    REQUIRE(m0.size() == 1);
    REQUIRE(m1.size() == 1);
    REQUIRE(*m0.find(1) == 10);
    REQUIRE(*m1.find(1) == 99);
}

TEST_CASE("map: erase", "[map]") {
    auto m0 = persistent::map<int, int>{}.insert(1, 10).insert(2, 20).insert(3, 30);
    auto m1 = m0.erase(2);
    REQUIRE(m0.size() == 3);
    REQUIRE(m1.size() == 2);
    REQUIRE(m1.count(1));
    REQUIRE(!m1.count(2));
    REQUIRE(m1.count(3));
}

TEST_CASE("map: erase nonexistent key is a no-op", "[map]") {
    auto m0 = persistent::map<int, int>{}.insert(1, 10);
    auto m1 = m0.erase(99);
    REQUIRE(m1.size() == 1);
    REQUIRE(m1.count(1));
}

TEST_CASE("map: equality", "[map]") {
    auto m0 = persistent::map<int, int>{}.insert(1, 10).insert(2, 20);
    auto m1 = persistent::map<int, int>{}.insert(2, 20).insert(1, 10);
    auto m2 = persistent::map<int, int>{}.insert(1, 10).insert(2, 99);

    REQUIRE(m0 == m1);
    REQUIRE(m0 != m2);
}

TEST_CASE("map: string keys", "[map]") {
    persistent::map<std::string, int> m;
    m = m.insert("foo", 1).insert("bar", 2).insert("baz", 3);
    REQUIRE(m.size() == 3);
    REQUIRE(*m.find("foo") == 1);
    REQUIRE(*m.find("bar") == 2);
    REQUIRE(*m.find("baz") == 3);
    REQUIRE(!m.find("qux"));
}

TEST_CASE("map: initializer_list constructor", "[map]") {
    persistent::map<int, int> m{{1, 10}, {2, 20}, {3, 30}};
    REQUIRE(m.size() == 3);
    REQUIRE(*m.find(1) == 10);
    REQUIRE(*m.find(2) == 20);
    REQUIRE(*m.find(3) == 30);
}

// ---------------------------------------------------------------------------
// Large-scale test: compare against immer::map
// ---------------------------------------------------------------------------
TEST_CASE("map: matches immer::map for many insertions", "[map][immer]") {
    constexpr int N = 1000;
    persistent::map<int, int> pm;
    immer::map<int, int>      im;

    for (int i = 0; i < N; ++i) {
        pm = pm.insert(i, i * 2);
        im = im.insert({i, i * 2});
    }

    REQUIRE(pm.size() == static_cast<std::size_t>(N));
    REQUIRE(im.size() == static_cast<std::size_t>(N));

    for (int i = 0; i < N; ++i) {
        const int* pv = pm.find(i);
        const int* iv = im.find(i);
        REQUIRE(pv != nullptr);
        REQUIRE(iv != nullptr);
        REQUIRE(*pv == *iv);
    }
}

TEST_CASE("map: matches immer::map for many insertions and erasures", "[map][immer]") {
    constexpr int N = 500;
    persistent::map<int, int> pm;
    immer::map<int, int>      im;

    for (int i = 0; i < N; ++i) {
        pm = pm.insert(i, i * 3);
        im = im.insert({i, i * 3});
    }

    for (int i = 0; i < N; i += 2) {
        pm = pm.erase(i);
        im = im.erase(i);
    }

    REQUIRE(pm.size() == im.size());

    for (int i = 0; i < N; ++i) {
        if (i % 2 == 0) {
            REQUIRE(pm.find(i) == nullptr);
            REQUIRE(im.find(i) == nullptr);
        } else {
            REQUIRE(pm.find(i) != nullptr);
            REQUIRE(im.find(i) != nullptr);
            REQUIRE(*pm.find(i) == *im.find(i));
        }
    }
}

TEST_CASE("map: iteration matches immer::map", "[map][immer]") {
    constexpr int N = 200;
    persistent::map<int, int> pm;
    immer::map<int, int>      im;

    for (int i = 0; i < N; ++i) {
        pm = pm.insert(i, i + 1);
        im = im.insert({i, i + 1});
    }

    // Collect and sort both (iteration order may differ, but contents must match).
    std::unordered_map<int, int> pm_map, im_map;
    for (const auto& kv : pm) pm_map[kv.first] = kv.second;
    for (const auto& kv : im) im_map[kv.first] = kv.second;

    REQUIRE(pm_map == im_map);
}

TEST_CASE("map: update existing key matches immer", "[map][immer]") {
    persistent::map<int, int> pm;
    immer::map<int, int>      im;

    for (int i = 0; i < 100; ++i) {
        pm = pm.insert(i % 10, i);
        im = im.insert({i % 10, i});
    }

    for (int i = 0; i < 10; ++i) {
        const int* pv = pm.find(i);
        const int* iv = im.find(i);
        REQUIRE(pv != nullptr);
        REQUIRE(iv != nullptr);
        REQUIRE(*pv == *iv);
    }
}

TEST_CASE("map: at() throws for missing key", "[map]") {
    persistent::map<int, int> m;
    m = m.insert(1, 10);
    REQUIRE(m.at(1) == 10);
    REQUIRE_THROWS_AS(m.at(99), std::out_of_range);
}
