#pragma once

// What both benchmarks are built on: a timer, a row of it against immer, and the verdict over every row
// printed.

#include <pthread.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace bench {
using Clock = std::chrono::steady_clock;

// `BENCH_ROUNDS` buys steadiness with wall clock -- nine to read one cell closely -- but does not steady
// the summary a change is judged on, so three is the default.
inline const uint32_t Rounds = [] {
    const auto *rounds = std::getenv("BENCH_ROUNDS");
    const auto n = rounds ? std::strtoul(rounds, nullptr, 10) : 0;
    return n ? uint32_t(n) : 3;
}();
constexpr double MinRoundNanos = 2e6;
// Keeps the optimizer from deleting the work.
inline uint64_t Sink = 0;

// Nanoseconds per operation, best of `Rounds`, since timing noise only ever adds. A round repeats until
// it has run for `MinRoundNanos`, then divides, so the small sizes stay comparable: a thousand
// operations alone are close enough to the clock's own cost to be mostly noise.
template<typename F> double Time(uint64_t ops, F &&f) {
    double best = 1e300;
    for (uint32_t i = 0; i < Rounds; ++i) {
        uint64_t reps = 0;
        double ns;
        const auto start = Clock::now();
        do {
            Sink += f();
            ++reps;
            ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        } while (ns < MinRoundNanos);
        const auto per_op = ns / double(ops * reps);
        best = per_op < best ? per_op : best;
    }
    return best;
}

// The sweep's verdict, over every row it prints. One cell's margin moves on nothing but where the linker
// put the code, which rerunning the same binary does not resample, so a single cell is not evidence. The
// mean holds steadier, since a layout that costs one cell pays another back.
inline double LogRatios = 0;
inline uint32_t Cells = 0, Behind = 0;

inline void Row(const char *name, double ours, double immer) {
    std::printf("  %-14s %8.2f %8.2f   %+6.1f%%\n", name, ours, immer, 100 * (immer - ours) / immer);
    LogRatios += std::log(immer / ours);
    Behind += ours > immer;
    ++Cells;
}

inline void Begin() {
    // Apple silicon runs a plain process on whichever core is free, and an efficiency core is about half
    // as fast here, so without this the readings are bimodal.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::printf("ns/op, lower is better. Last column is our margin over immer.\n");
}

inline void End() {
    std::printf("\n%u cells, %u behind immer, geometric mean %+.1f%%  (checksum %llu)\n", Cells, Behind, 100 * (1 - std::exp(-LogRatios / Cells)), (unsigned long long)Sink);
}
} // namespace bench
