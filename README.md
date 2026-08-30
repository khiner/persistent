# persistent
Persistent data structures (inspired by immer)

Five immutable containers in C++23, written from scratch, tested against [immer](https://github.com/arximboldi/immer), and benchmarked against it everywhere but the box.

- `box::Box<T>` — one value of any type, held on the heap behind a reference count. Copying increments a count, and two boxes that never diverged compare equal without reading the value, so a `T` that is expensive to copy and compare is cheap at both. Update, equality, and the conversions that let a box be used as the value it holds.
- `hamt::Map` — a map from 64-bit keys to 64-bit values, backed by a CHAMP hash array mapped trie. Lookup, insert or assign, erase, update, build from a range, equality, diff, iteration.
- `hamt::Set` — a set of 64-bit keys, the same trie over elements that are their own key, so a node is half as wide. Membership, insert, erase, build from a range, equality, diff, iteration. Inserting a key it already holds returns the same set and copies nothing.
- `vec::Vector` — a sequence of 64-bit values, backed by a radix balanced trie with a tail. Index, push back, set, update, take, build from a range, equality, diff, iteration.
- `vec::FlexVector` — the same sequence, backed by a *relaxed* radix balanced trie, where a node may be short, so concatenation, a cut from the front, and insertion and erasure in the middle are all logarithmic. A `Vector` converts to one in constant time, and not the reverse.

`Diff` reports the runs two containers differ in. For the maps and sets it is the CHAMP walk. For the vectors it is a shortest edit script over a longest common extension that skips a shared subtree by pointer compare, so its cost is the number of changes. An insertion is reported as one element added: one element inserted into the middle of a million is reported in a couple of microseconds, and a thousand-element block moved across it as two runs of a thousand. immer has diff for maps and sets only, and this implements [the request for vectors](https://github.com/arximboldi/immer/pull/199). `Apply` is the inverse, `Apply(a, Diff(a, b), b)` is `b`, and it takes a source other than `b`, so a change set applies to a vector it was not computed on. `FlexVector` only, since a change moves elements. A history is kept by keeping the snapshots, which already share every node they have in common.

Every operation returns a new container and leaves its input unchanged. Containers share the nodes they have in common, and a node is freed when the last container holding it is. A write given a container as an rvalue — `InsertOrAssign(std::move(m), key, value)` — writes through every node on its path that nothing else holds, and copies the ones that are shared.

All four tries also expose immer-style transients for mutable batches. Call `transient()` on a persistent container, call `push_back`, `set`, `insert`, `update`, or `erase`, then call `persistent()`. Converting an lvalue preserves it, and converting an rvalue transfers it. Calling `persistent()` on a transient lvalue takes a cheap snapshot and leaves the transient usable.

Single threaded: reference counts are plain integers, and the tries race even between two that share nothing, their nodes coming from shared slabs. Clang and libc++ only.

The four tries are compiled against `uint64_t`, so their nodes are one width and come out of a slab allocator. The box is header-only and a template over the value it holds, and its cell is one `new`.

    cmake -B build && cmake --build build
    ./build/tests/BoxTest && ./build/tests/HamtTest && ./build/tests/SetTest && ./build/tests/VectorTest && ./build/tests/FlexVectorTest
    ./build/bench/HamtBench && ./build/bench/SetBench && ./build/bench/VectorBench && ./build/bench/FlexVectorBench

`HamtAuditTest`, `SetAuditTest`, `VectorAuditTest` and `FlexVectorAuditTest` are the same tests over a build that frees nodes to the allocator, so leaks are visible to a sanitizer. The box has no audit test, since its cells go to the allocator already.

[docs/Literature.md](docs/Literature.md) lists the papers and implementations this is based on.
