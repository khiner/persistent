# persistent
Persistent data structures (inspired by immer)

[immer](https://github.com/arximboldi/immer) is amazing. This project's current goal is to match the observable behavior of its HAMT — the same mapping, size, and iteration contents — with a from-scratch implementation focused on simplicity and performance. immer is the oracle for what the map does, not for how it is represented: hash mixing, branching factor, and node layout are ours to choose.

Clang and libc++ only.

See [docs/Literature.md](docs/Literature.md) for the work this builds on. Why this implementation is
shaped the way it is belongs next to the code that is shaped that way, so it lives in the comments.
