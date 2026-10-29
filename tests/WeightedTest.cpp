// Tests for the weighted vector, built to count newlines, against a flat array.
//   cmake --build build --target WeightedTest WeightedAuditTest
//   ./build/tests/WeightedTest && ./build/tests/WeightedAuditTest

#include "Vector.h"

#include <boost/ut.hpp>

#include <random>
#include <vector>

namespace {
using Values = std::vector<uint64_t>;
constexpr uint64_t Newline = '\n';

Values Walked(const vec::Weighted &v) {
    Values out;
    for (const auto x : v) out.push_back(x);
    return out;
}

// Random values with a newline about every twentieth, plus stretches with no newlines and stretches
// of only newlines.
Values Prose(uint64_t n, uint64_t seed) {
    std::mt19937_64 rng{seed};
    Values out(n);
    for (uint64_t i = 0; i < n; ++i) {
        const auto r = rng() % 100;
        out[i] = r < 5 || (i / 300) % 7 == 3 ? Newline : 'a' + r % 26;
        if ((i / 500) % 11 == 5) out[i] = 'x';
    }
    return out;
}

bool Holds(const vec::Weighted &v, const Values &expected) {
    if (!vec::Check(v) || v.Size != expected.size() || Walked(v) != expected) return false;
    uint64_t marks = 0;
    for (uint64_t i = 0; i <= expected.size(); ++i) {
        if (vec::WeightBefore(v, i) != marks) return false;
        if (i < expected.size() && expected[i] == Newline) {
            if (vec::Seek(v, marks) != i) return false;
            ++marks;
        }
    }
    return vec::Weight(v) == marks;
}

vec::Weighted Pushed(const Values &values) {
    vec::Weighted v;
    for (const auto x : values) v = vec::PushBack(std::move(v), x);
    return v;
}
} // namespace

using namespace boost::ut;

int main() {
    "empty"_test = [] {
        const vec::Weighted v;
        expect(Holds(v, {}));
        expect(vec::Weight(v) == 0_u64);
        expect(vec::WeightBefore(v, 0) == 0_u64);
    };

    "built and pushed"_test = [] {
        for (const auto n : {1u, 2u, 63u, 64u, 65u, 4096u, 4097u, 20'000u}) {
            const auto values = Prose(n, n);
            expect(Holds(vec::Weighted{values.begin(), values.end()}, values)) << "built " << n;
            expect(Holds(Pushed(values), values)) << "pushed " << n;
        }
    };

    "set and transient set"_test = [] {
        auto values = Prose(5'000, 1);
        auto v = vec::Weighted{values.begin(), values.end()};
        const auto shared = v; // A second reference. The first write copies the path, later writes are in place.
        std::mt19937_64 rng{2};
        for (int i = 0; i < 300; ++i) {
            const auto at = rng() % values.size();
            const auto x = rng() % 2 ? Newline : uint64_t{'q'};
            values[at] = x;
            v = i % 2 ? vec::Set(std::move(v), at, x) : vec::Update(std::move(v), at, [x](uint64_t) { return x; });
        }
        expect(Holds(v, values));
        expect(Holds(shared, Prose(5'000, 1)));
        auto t = std::move(v).transient();
        for (uint64_t i = 0; i < 500; ++i) {
            values.push_back(i % 9 ? 'k' : Newline);
            t.push_back(values.back());
            values[i * 3] = Newline;
            t.set(i * 3, Newline);
        }
        expect(Holds(t.persistent(), values));
    };

    "mixed writes against the oracle"_test = [] {
        std::mt19937_64 rng{7};
        auto values = Prose(3'000, 9);
        auto v = vec::Weighted{values.begin(), values.end()};
        for (int step = 0; step < 400; ++step) {
            const auto n = values.size();
            switch (rng() % 8) {
                case 0: {
                    const auto x = rng() % 3 ? uint64_t{'z'} : Newline;
                    values.push_back(x);
                    v = vec::PushBack(v, x);
                    break;
                }
                case 1: {
                    const auto keep = rng() % (n + 1);
                    values.resize(keep);
                    v = vec::Take(v, keep);
                    break;
                }
                case 2: {
                    const auto drop = rng() % (n + 1);
                    values.erase(values.begin(), values.begin() + int64_t(drop));
                    v = vec::Drop(v, drop);
                    break;
                }
                case 3: {
                    const auto more = Prose(rng() % 700, step);
                    values.insert(values.end(), more.begin(), more.end());
                    v = vec::Concat(v, vec::Weighted{more.begin(), more.end()});
                    break;
                }
                case 4: {
                    const auto at = rng() % (n + 1);
                    const auto x = rng() % 3 ? uint64_t{'w'} : Newline;
                    values.insert(values.begin() + int64_t(at), x);
                    v = vec::Insert(v, at, x);
                    break;
                }
                case 5: {
                    const auto at = rng() % (n + 1);
                    const auto more = Prose(rng() % 300, step + 1000);
                    values.insert(values.begin() + int64_t(at), more.begin(), more.end());
                    v = vec::Insert(v, at, vec::Weighted{more.begin(), more.end()});
                    break;
                }
                case 6: {
                    if (!n) break;
                    const auto first = rng() % n, last = first + rng() % (n - first + 1);
                    values.erase(values.begin() + int64_t(first), values.begin() + int64_t(last));
                    v = vec::Erase(v, first, last);
                    break;
                }
                case 7: {
                    const auto more = Prose(rng() % 100, step + 2000);
                    values.insert(values.begin(), more.begin(), more.end());
                    v = vec::Concat(vec::Weighted{more.begin(), more.end()}, v);
                    break;
                }
            }
            expect(Holds(v, values) >> fatal) << "step " << step;
        }
    };

    "reads as a plain vector"_test = [] {
        const auto values = Prose(1'000, 5);
        const vec::Weighted text{values.begin(), values.end()};
        const vec::FlexVector plain = text;
        expect(plain.Size == text.Size);
        expect(vec::Check(plain));
        expect(vec::Concat(plain, plain).Size == 2'000_u64);
    };

    "reclamation"_test = [] {
        {
            auto values = Prose(20'000, 6);
            auto v = vec::Weighted{values.begin(), values.end()};
            const auto shared = v;
            for (uint64_t i = 0; i < 20'000; i += 3) v = vec::Set(v, i, Newline);
            v = vec::Concat(vec::Take(std::move(v), 7'000), Pushed(Prose(900, 8)));
            v = vec::Drop(std::move(v), 500);
            expect(v.Size == 7'400_u64);
            expect(shared.Size == 20'000_u64);
        }
        if (const auto live = vec::LiveNodes()) expect(*live == 0_u64);
    };
}
