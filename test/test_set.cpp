#include <catch2/catch_test_macros.hpp>

#include <persistent/set.hpp>

#include <immer/set.hpp>

#include <string>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------
TEST_CASE("set: empty", "[set]") {
    persistent::set<int> s;
    REQUIRE(s.size() == 0);
    REQUIRE(s.empty());
}

TEST_CASE("set: single insert and count", "[set]") {
    auto s = persistent::set<int>{}.insert(42);
    REQUIRE(s.size() == 1);
    REQUIRE(!s.empty());
    REQUIRE(s.count(42));
    REQUIRE(!s.count(99));
}

TEST_CASE("set: insert is persistent", "[set]") {
    auto s0 = persistent::set<int>{};
    auto s1 = s0.insert(1);
    auto s2 = s1.insert(2);

    REQUIRE(s0.size() == 0);
    REQUIRE(s1.size() == 1);
    REQUIRE(s2.size() == 2);

    REQUIRE(!s0.count(1));
    REQUIRE(s1.count(1));
    REQUIRE(!s1.count(2));
    REQUIRE(s2.count(1));
    REQUIRE(s2.count(2));
}

TEST_CASE("set: inserting duplicate is idempotent", "[set]") {
    auto s0 = persistent::set<int>{}.insert(5);
    auto s1 = s0.insert(5);
    REQUIRE(s0.size() == 1);
    REQUIRE(s1.size() == 1);
    REQUIRE(s1.count(5));
}

TEST_CASE("set: erase", "[set]") {
    auto s0 = persistent::set<int>{}.insert(1).insert(2).insert(3);
    auto s1 = s0.erase(2);
    REQUIRE(s0.size() == 3);
    REQUIRE(s1.size() == 2);
    REQUIRE(s1.count(1));
    REQUIRE(!s1.count(2));
    REQUIRE(s1.count(3));
}

TEST_CASE("set: erase nonexistent key is a no-op", "[set]") {
    auto s0 = persistent::set<int>{}.insert(1);
    auto s1 = s0.erase(99);
    REQUIRE(s1.size() == 1);
    REQUIRE(s1.count(1));
}

TEST_CASE("set: equality", "[set]") {
    auto s0 = persistent::set<int>{}.insert(1).insert(2).insert(3);
    auto s1 = persistent::set<int>{}.insert(3).insert(1).insert(2);
    auto s2 = persistent::set<int>{}.insert(1).insert(2);

    REQUIRE(s0 == s1);
    REQUIRE(s0 != s2);
}

TEST_CASE("set: string elements", "[set]") {
    auto s = persistent::set<std::string>{}.insert("hello").insert("world");
    REQUIRE(s.size() == 2);
    REQUIRE(s.count("hello"));
    REQUIRE(s.count("world"));
    REQUIRE(!s.count("foo"));
}

TEST_CASE("set: initializer_list constructor", "[set]") {
    persistent::set<int> s{1, 2, 3, 4, 5};
    REQUIRE(s.size() == 5);
    for (int i = 1; i <= 5; ++i) REQUIRE(s.count(i));
    REQUIRE(!s.count(6));
}

// ---------------------------------------------------------------------------
// Large-scale comparison against immer::set
// ---------------------------------------------------------------------------
TEST_CASE("set: matches immer::set for many insertions", "[set][immer]") {
    constexpr int N = 1000;
    persistent::set<int> ps;
    immer::set<int>      is;

    for (int i = 0; i < N; ++i) {
        ps = ps.insert(i);
        is = is.insert(i);
    }

    REQUIRE(ps.size() == static_cast<std::size_t>(N));
    REQUIRE(is.size() == static_cast<std::size_t>(N));

    for (int i = 0; i < N; ++i) {
        REQUIRE(ps.count(i));
        REQUIRE(is.count(i));
    }
    REQUIRE(!ps.count(N));
    REQUIRE(!is.count(N));
}

TEST_CASE("set: matches immer::set for insertions and erasures", "[set][immer]") {
    constexpr int N = 500;
    persistent::set<int> ps;
    immer::set<int>      is;

    for (int i = 0; i < N; ++i) {
        ps = ps.insert(i);
        is = is.insert(i);
    }
    for (int i = 0; i < N; i += 2) {
        ps = ps.erase(i);
        is = is.erase(i);
    }

    REQUIRE(ps.size() == is.size());
    for (int i = 0; i < N; ++i) {
        bool pe = ps.count(i) != 0;
        bool ie = is.count(i) != 0;
        REQUIRE(pe == ie);
    }
}

TEST_CASE("set: iteration matches immer::set", "[set][immer]") {
    constexpr int N = 200;
    persistent::set<int> ps;
    immer::set<int>      is;

    for (int i = 0; i < N; ++i) {
        ps = ps.insert(i);
        is = is.insert(i);
    }

    std::unordered_set<int> ps_set, is_set;
    for (int v : ps) ps_set.insert(v);
    for (int v : is) is_set.insert(v);

    REQUIRE(ps_set == is_set);
}
