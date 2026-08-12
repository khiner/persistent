# persistent
Persistent data structures (inspired by immer)

[immer](https://github.com/arximboldi/immer) is amazing. This project's current goal is to match the observable behavior of its HAMT — the same mapping, size, and iteration contents — with a from-scratch implementation focused on simplicity and performance. immer is the oracle for what the map does, not for how it is represented: hash mixing, branching factor, and node layout are ours to choose.

See [docs/Literature.md](docs/Literature.md) for the work this builds on and the open design questions.
