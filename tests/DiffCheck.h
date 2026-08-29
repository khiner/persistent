#pragma once

// Checks on a diff, for either vector shape.
// `a` with every reported run replaced is `b`, and the runs touch no more positions than a shortest script.

#include "Vector.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace diffcheck {
using Values = std::vector<uint64_t>;

// The stages of a diff, as flags, so a test can state which of them it expects to have resolved a stretch.
// `Stripped` is none of them: stripping the common prefix and suffix was the whole answer.
enum Question : uint32_t { Stripped = 0,
                           Probed = 1,
                           Ran = 2,
                           Hashed = 4,
                           Searched = 8,
                           Whole = 16 };

// The counts as they stand, to compare against after a diff.
using Mark = std::optional<vec::Phases>;
inline Mark Marked() { return vec::DiffPhases(); }

// The stages that resolved a stretch since `mark`, as a set of flags. None outside an audit build.
inline uint32_t Asked(const Mark &mark) {
    const auto now = vec::DiffPhases();
    if (!mark || !now) return Stripped;
    uint32_t asked = Stripped;
    if (now->Probed > mark->Probed) asked |= Probed;
    if (now->Ran > mark->Ran) asked |= Ran;
    if (now->Hashed > mark->Hashed) asked |= Hashed;
    if (now->Searched > mark->Searched) asked |= Searched;
    if (now->Whole > mark->Whole) asked |= Whole;
    return asked;
}

// Whether every stage that resolved a stretch since `mark` is in `allowed`, and at least one did unless `allowed` is empty.
inline bool Settled(const Mark &mark, uint32_t allowed) {
    if (!mark) return true;
    const auto asked = Asked(mark);
    return !(asked & ~allowed) && (asked || !allowed);
}

template<vec::Shape S> std::vector<vec::Change> Diffed(const vec::Trie<S> &a, const vec::Trie<S> &b) {
    std::vector<vec::Change> changes;
    vec::Diff(a, b, [&changes](const vec::Change &c) { changes.push_back(c); });
    return changes;
}

// Whether the diff is exactly one run with the given counts, at `index` on both sides.
template<vec::Shape S> bool OneChange(const vec::Trie<S> &a, const vec::Trie<S> &b, uint64_t index, uint64_t removed, uint64_t added) {
    const auto changes = Diffed(a, b);
    return changes.size() == 1 && changes[0].Index == index && changes[0].Removed == removed && changes[0].To == index && changes[0].Added == added;
}

// Whether applying the diff to `a` over a plain array gives `b`, with the runs in order, none empty, and no two adjacent.
// The last three conditions matter since one run covering everything would rebuild just as well.
template<vec::Shape S> bool Rebuilds(const vec::Trie<S> &a, const vec::Trie<S> &b) {
    Values out;
    uint64_t at = 0; // How far into `a` the runs so far reach.
    bool first = true;
    for (const auto &c : Diffed(a, b)) {
        if (c.Index + c.Removed > a.Size || c.To + c.Added > b.Size) return false;
        if (!c.Removed && !c.Added) return false;
        if (first ? c.Index < at : c.Index <= at) return false; // In order and never adjacent.
        first = false;
        for (uint64_t i = at; i < c.Index; ++i) out.push_back(a[i]);
        if (c.To != out.size()) return false;
        for (uint64_t i = 0; i < c.Added; ++i) out.push_back(b[c.To + i]);
        at = c.Index + c.Removed;
    }
    for (uint64_t i = at; i < a.Size; ++i) out.push_back(a[i]);
    if (out.size() != b.Size) return false;
    for (uint64_t i = 0; i < b.Size; ++i)
        if (out[i] != b[i]) return false;
    return true;
}

// How many positions the diff touches.
// A run costs the larger of its two counts, since an overwrite is one edit.
inline uint64_t Edits(const std::vector<vec::Change> &changes) {
    uint64_t n = 0;
    for (const auto &c : changes) n += c.Removed > c.Added ? c.Removed : c.Added;
    return n;
}

// The cost of a shortest Levenshtein script, by the quadratic dynamic program, so only for short inputs.
inline uint64_t ShortestScript(const Values &a, const Values &b) {
    std::vector<uint64_t> row(b.size() + 1), next(b.size() + 1);
    for (uint64_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (uint64_t i = 0; i < a.size(); ++i) {
        next[0] = i + 1;
        for (uint64_t j = 0; j < b.size(); ++j) {
            const auto over = row[j] + (a[i] == b[j] ? 0 : 1), removed = row[j + 1] + 1, added = next[j] + 1;
            next[j + 1] = std::min({over, removed, added});
        }
        row.swap(next);
    }
    return row[b.size()];
}

template<vec::Shape S> Values Held(const vec::Trie<S> &v) {
    Values out;
    for (uint64_t i = 0; i < v.Size; ++i) out.push_back(v[i]);
    return out;
}

inline bool Obeys(const vec::FlexVector &a, const vec::FlexVector &b) {
    const auto changes = Diffed(a, b);
    return vec::Apply(a, changes.data(), changes.data() + changes.size(), b) == b;
}

// Whether `Rebuilds` holds both ways and each way touches no more positions than a shortest script.
template<vec::Shape S> bool Diffs(const vec::Trie<S> &a, const vec::Trie<S> &b) {
    if (!Rebuilds(a, b) || !Rebuilds(b, a)) return false;
    const auto shortest = ShortestScript(Held(a), Held(b));
    return Edits(Diffed(a, b)) <= shortest && Edits(Diffed(b, a)) <= shortest;
}
} // namespace diffcheck
