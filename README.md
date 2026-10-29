# persistent

Five immutable containers in C++23, written from scratch, inspired by and tested against [immer](https://github.com/arximboldi/immer).
All except `Box` are benchmarked against immer.

## Containers

- `box::Box<T>` — a reference-counted heap value.
  Copies share the value, and boxes sharing the same allocation compare equal without reading it.
  Supports update, equality, and conversion to the stored type.
- `hamt::Map` — a map from 64-bit keys to 64-bit values, backed by a CHAMP hash array mapped trie.
  Lookup, insert or assign, erase, update, build from a range, equality, diff, iteration.
- `hamt::Set` — a set of 64-bit keys, using the same trie as `Map` with nodes half as wide.
  Membership, insert, erase, build from a range, equality, diff, iteration.
  Inserting an existing key copies nothing.
- `vec::Vector` — a sequence of 64-bit values, backed by a radix balanced trie with a tail.
  Index, push back, set, update, take, build from a range, equality, diff, iteration.
- `vec::FlexVector` — a sequence of 64-bit values, backed by a relaxed radix balanced trie.
  Supports logarithmic concatenation, removal from the front, and insertion and erasure in the middle.
  Conversion from `Vector` takes constant time.
  Reverse conversion is not supported.
- `vec::Weighted` — a `FlexVector` with a weight on each slot.
  Every inner node stores the running weight under each child, so `Weight`, `WeightBefore` (the weight before an index) and `Seek` (the first index whose running weight exceeds k) are single descents.
  The weight is fixed at build time. `VECTOR_WEIGHT_KEY` is the slot value to count. The default, 10, counts newlines, which indexes text by line.
  Conversion to `FlexVector` takes constant time.

## Vector diff

This library adds vector diffing, implementing [a feature I requested for immer](https://github.com/arximboldi/immer/pull/199#issuecomment-1961739189).
`Diff` reports the range replacements needed to turn one vector into another, skipping shared subtrees by pointer comparison.
A middle insertion is one addition.
Moving a contiguous block is one removal and one addition.

`Apply` applies those replacements to a `FlexVector`: `Apply(a, Diff(a, b), b)` produces `b`.
Changes store indices, so replacement values can also come from another source vector.
To keep undo history, retain the snapshots.
They already share unchanged nodes.

## Ownership and mutation

Persistent updates return a new container.
Updates to an lvalue leave it unchanged.
Containers share unchanged nodes and free each node when its last owner releases it.
Updates to an rvalue, such as `InsertOrAssign(std::move(m), key, value)`, reuse unshared nodes and copy shared ones.

All four tries support immer-style transients for mutable batches.
Call `transient()`, make changes with methods such as `push_back`, `set`, `insert`, `update`, or `erase`, then call `persistent()`.
Converting an lvalue preserves it.
Converting an rvalue transfers its data.
Calling `persistent()` on a transient lvalue takes a cheap snapshot and leaves the transient usable.

Single-threaded only.
Reference counts are non-atomic.
Shared slab allocators make concurrent trie operations unsafe, even on unrelated containers.

## Build and test

Requires Clang and libc++.
The four tries use `uint64_t` keys and values.
`Box<T>` is header-only.

    cmake -B build && cmake --build build
    ./build/tests/BoxTest && ./build/tests/HamtTest && ./build/tests/SetTest && ./build/tests/VectorTest && ./build/tests/FlexVectorTest && ./build/tests/WeightedTest
    ./build/bench/HamtBench && ./build/bench/SetBench && ./build/bench/VectorBench && ./build/bench/FlexVectorBench

`HamtAuditTest`, `SetAuditTest`, `VectorAuditTest`, `FlexVectorAuditTest`, and `WeightedAuditTest` run the same tests with node deallocation enabled so sanitizers can detect leaks.
`BoxTest` needs no separate audit build because boxes already deallocate their storage.

[docs/Literature.md](docs/Literature.md) lists the papers and implementations this is based on.
