// Standing benchmark against immer, the reference implementation.
//   cmake --build build --target HamtBench && ./build/bench/HamtBench
// Sweep the branching factor by configuring a second build dir with -DHAMT_BITS=6.

#include "Hamt.h"

// Included with plain -I rather than -isystem (see bench/CMakeLists.txt), so silence immer's warnings here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#pragma clang diagnostic pop

#include <chrono>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using ImmerMap = immer::map<uint64_t, uint64_t>;

// Nanoseconds per operation, best of `Rounds`. Timing noise only ever adds, so the minimum is the
// closest we get to the real cost. `Sink` is there to keep the optimizer from deleting the work.
constexpr uint32_t Rounds = 5;
uint64_t Sink = 0;

template<typename F> double Time(uint64_t ops, F &&f) {
    double best = 1e300;
    for (uint32_t i = 0; i < Rounds; ++i) {
        const auto start = Clock::now();
        Sink += f();
        const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        best = ns < best ? ns : best;
    }
    return best / double(ops);
}

std::vector<uint64_t> RandomKeys(uint64_t n, uint64_t seed) {
    std::mt19937_64 rng{seed};
    std::vector<uint64_t> keys(n);
    for (auto &key : keys) key = rng();
    return keys;
}

hamt::Map Built(const std::vector<uint64_t> &keys) {
    hamt::Map m{};
    for (uint64_t i = 0; i < keys.size(); ++i) m = hamt::Set(std::move(m), keys[i], i);
    return m;
}

ImmerMap ImmerBuilt(const std::vector<uint64_t> &keys) {
    auto t = ImmerMap{}.transient();
    for (uint64_t i = 0; i < keys.size(); ++i) t.set(keys[i], i);
    return t.persistent();
}

void Row(const char *name, double ours, double immer) {
    std::printf("  %-14s %8.1f %8.1f   %+6.1f%%\n", name, ours, immer, 100 * (immer - ours) / immer);
}

void Run(uint64_t n) {
    const auto keys = RandomKeys(n, 1), absent = RandomKeys(n, 2);
    const auto ours = Built(keys);
    const auto theirs = ImmerBuilt(keys);

    std::printf("\nn = %llu%s\n  %-14s %8s %8s %9s\n", (unsigned long long)n, "", "", "ours", "immer", "ours vs");

    Row("build copy",
        Time(n, [&] { hamt::Map m{}; for (uint64_t i = 0; i < n; ++i) m = hamt::Set(m, keys[i], i); return m.Size; }),
        Time(n, [&] { ImmerMap m; for (uint64_t i = 0; i < n; ++i) m = m.set(keys[i], i); return m.size(); }));
    Row("build move",
        Time(n, [&] { return Built(keys).Size; }),
        Time(n, [&] { return ImmerBuilt(keys).size(); }));
    Row("lookup hit",
        Time(n, [&] { uint64_t s = 0; for (auto key : keys) s += *hamt::Get(ours, key); return s; }),
        Time(n, [&] { uint64_t s = 0; for (auto key : keys) s += *theirs.find(key); return s; }));
    Row("lookup miss",
        Time(n, [&] { uint64_t s = 0; for (auto key : absent) s += hamt::Get(ours, key).has_value(); return s; }),
        Time(n, [&] { uint64_t s = 0; for (auto key : absent) s += theirs.find(key) != nullptr; return s; }));
    Row("erase move",
        Time(n, [&] { auto m = ours; for (auto key : keys) m = hamt::Erase(std::move(m), key); return m.Size; }),
        Time(n, [&] { auto m = theirs; for (auto key : keys) m = m.erase(key); return m.size(); }));
    Row("iterate",
        Time(n, [&] { uint64_t s = 0; for (const auto &e : ours) s += e.Value; return s; }),
        Time(n, [&] { uint64_t s = 0; for (const auto &e : theirs) s += e.second; return s; }));
    // Built separately from the same keys, so the two maps share nothing and equality has to walk them.
    const auto ours_twin = Built(keys);
    const auto theirs_twin = ImmerBuilt(keys);
    Row("equal",
        Time(n, [&] { return uint64_t(ours == ours_twin); }),
        Time(n, [&] { return uint64_t(theirs == theirs_twin); }));
}
} // namespace

int main() {
    std::printf("ns/op, lower is better. Last column is our margin over immer.\n");
    for (const uint64_t n : {uint64_t{1'000}, uint64_t{100'000}, uint64_t{1'000'000}}) Run(n);
    std::printf("\n(checksum %llu)\n", (unsigned long long)Sink);
}
