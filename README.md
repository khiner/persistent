# persistent
Persistent data structures (inspired by immer)

Five immutable containers in C++23, written from scratch, tested against [immer](https://github.com/arximboldi/immer), and benchmarked against it everywhere but the box.

- `box::Box<T>` — one value of any type, held on the heap behind a reference count. Copying is a counter bump, and two boxes that never diverged compare equal without reading the value, so a `T` that is expensive to copy and expensive to compare becomes cheap at both. Update, equality, and the conversions that let a box stand in for the value it holds.
- `hamt::Map` — a map from 64-bit keys to 64-bit values, backed by a CHAMP hash array mapped trie. Lookup, insert or assign, erase, update, build from a range, equality, diff, iteration.
- `hamt::Set` — a set of 64-bit keys, the same trie over elements that are their own key, so a node is half as wide. Membership, insert, erase, build from a range, equality, diff, iteration. Inserting a key it already holds gives the set straight back, having copied nothing.
- `vec::Vector` — a sequence of 64-bit values, backed by a radix balanced trie with a tail. Index, push back, set, update, take, build from a range, equality, iteration.
- `vec::FlexVector` — the same sequence, backed by a *relaxed* radix balanced trie: it gives up the rule that every node is full, and gets concatenation, a cut from the front, and insertion and erasure in the middle, all logarithmic. A `Vector` converts to one in constant time, and not the reverse.

Every operation returns a new container and leaves its input unchanged. Containers share the nodes they have in common, and a node is freed when the last container holding it is. A write given a container as an rvalue — `InsertOrAssign(std::move(m), key, value)` — writes through every node on its path that nothing else holds, and copies the ones that are shared.

All four tries also expose immer-style transients for mutable batches. Call `transient()` on a persistent container, use methods such as `push_back`, `set`, `insert`, `update`, and `erase`, then call `persistent()`. Converting an lvalue preserves it; converting an rvalue transfers it. Calling `persistent()` on a transient lvalue takes a cheap snapshot and leaves the transient usable.

Single threaded: reference counts are plain integers, and the tries race even between two that share nothing, their nodes coming from shared slabs. Clang and libc++ only.

The four tries are compiled against `uint64_t`, so their nodes are one width and come out of a slab allocator. The box is header-only and a template, since the value it holds is the whole point of it, and its cell is one `new`.

    cmake -B build && cmake --build build
    ./build/tests/BoxTest && ./build/tests/HamtTest && ./build/tests/SetTest && ./build/tests/VectorTest && ./build/tests/FlexVectorTest
    ./build/bench/HamtBench && ./build/bench/SetBench && ./build/bench/VectorBench && ./build/bench/FlexVectorBench

`HamtAuditTest`, `SetAuditTest`, `VectorAuditTest` and `FlexVectorAuditTest` are the same tests over a build that frees nodes to the allocator, so leaks are visible to a sanitizer. The box has no audit twin, its cells going to the allocator already.

[docs/Literature.md](docs/Literature.md) lists the papers and implementations this is based on. Why the code is shaped the way it is lives in comments next to the code.
