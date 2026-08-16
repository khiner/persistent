# persistent
Persistent data structures (inspired by immer)

Two immutable containers in C++23, written from scratch, tested and benchmarked against [immer](https://github.com/arximboldi/immer).

- `hamt::Map` — a map from 64-bit keys to 64-bit values, backed by a CHAMP hash array mapped trie. Lookup, set, erase, update, build from a range, equality, diff, iteration.
- `vec::Vector` — a sequence of 64-bit values, backed by a radix balanced trie with a tail. Index, push back, set, update, take, build from a range, equality, iteration.

Every operation returns a new container and leaves its input unchanged. Containers share the nodes they have in common, and a node is freed when the last container holding it is. A write given a container as an rvalue — `Set(std::move(m), key, value)` — writes through every node on its path that nothing else holds, and copies the ones that are shared.

Single threaded: reference counts are plain integers, and two threads race even on containers that share nothing. Clang and libc++ only.

    cmake -B build && cmake --build build
    ./build/tests/HamtTest && ./build/tests/VectorTest
    ./build/bench/HamtBench && ./build/bench/VectorBench

`HamtAuditTest` and `VectorAuditTest` are the same tests over a build that frees nodes to the allocator, so leaks are visible to a sanitizer.

[docs/Literature.md](docs/Literature.md) lists the papers and implementations this is based on. Why the code is shaped the way it is lives in comments next to the code.
