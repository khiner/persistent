# Literature

Running notes on the work that informs this project. Additions welcome — each entry should say what to take from it, not just what it is.

## 1. Core lineage

Read in this order.

**Bagwell, [_Ideal Hash Trees_](https://lampwww.epfl.ch/papers/idealhashtrees.pdf) (EPFL TR, 2001), building on his _Fast and Space Efficient Trie Searches_ (2000).**
The origin. Array-mapped trie, sparse node = bitmap + popcount-indexed compact array. Everything below is a delta on this.

**Steindorfer & Vinju, [_Optimizing Hash-Array Mapped Tries for Fast and Lean Immutable JVM Collections_](https://michael.steindorfer.name/publications/oopsla15.pdf) (OOPSLA 2015).**
The CHAMP paper — the spec we are implementing. Three changes over HAMT:
1. Split the single bitmap into `datamap` + `nodemap`.
2. Store payloads and child pointers in separate regions of one array, data front-loaded, children from the back.
3. Maintain a canonical form by collapsing singleton subtrees back inline on delete.

Reported 1.3–6.7x iteration and 3–25.4x equality over HAMT, with a smaller footprint. The canonicality is the load-bearing part: it makes "same content implies same shape" true, which is what fast equality and fast diff are built on.

**Steindorfer, [_Efficient Immutable Collections_](https://homepages.cwi.nl/~jurgenv/phdtheses/PhDThesis_Michael_Steindorfer.pdf) (PhD thesis, UvA 2017).**
The long form. Worth it for the node-layout arithmetic, the memory-footprint methodology, and the delete/compaction rules stated precisely — the OOPSLA paper compresses these.

**Steindorfer & Vinju, HHAMT — [_To-Many or To-One? All-in-One!_](https://michael.steindorfer.name/publications/pldi18.pdf) (PLDI 2018), [arXiv 1608.01036](https://arxiv.org/pdf/1608.01036).**
Heterogeneous payloads via an extra bitmap per node, so one node type serves set / map / multimap and small values can live unboxed. Relevant if we later want `Set` and `Map` to share one node implementation rather than templating twice.

**Bolívar Puente, [_Persistence for the Masses: RRB-Vectors in a Systems Language_](https://public.sinusoid.es/misc/immer/immer-icfp17.pdf) (ICFP 2017).**
immer's own paper. Nominally about vectors, but the valuable half for us is the treatment of memory policies, refcounting, and transients — the only paper in this list written about doing this in C++ with no GC, which is our exact constraint.

## 2. What immer actually does

Read from the `immer/` submodule, since it is our oracle.

- `B = 5` (`immer/config.hpp:151`), `bitmap_t = uint32_t`, `hash_t = size_t` (64-bit), so `max_depth = 13` and collision nodes appear only at `shift == 65`.
- Two node kinds (`immer/detail/hamts/node.hpp`): `inner_t { nodemap, datamap, values*, children[] }` and `collision_t { count, values[] }`.
- **Divergence from the paper:** immer puts values in a separately heap-allocated `values_t` block with its own refcount and `ownee`, not inline in the node. Every data hit costs an extra pointer chase and every node costs two allocations. It is a deliberate trade for transience — the values block can be shared and mutated independently — and it is our clearest performance opening.
- Canonicalization on erase is real (`do_sub`: singleton collapse via `copy_inner_replace_inline`, collision 2 to 1, root special-cased at `shift == 0`), and immer ships `check_champ()` to assert it. Bit-exact parity is therefore a well-defined target, testable structurally and not just through `Get`.
- **No hash mixing.** immer feeds `Hash{}(key)` straight into the trie, and `std::hash<uint64_t>` is the identity. Trie shape is therefore fully determined by the raw low bits of the key. This is a better bet than it looks: it is *optimal* for a dense key set, since a counter gives a perfectly full trie with every key at the same depth, and modest strides cost only the few levels their zeroed low bits waste. It takes a large alignment before it really hurts. We are not bound by it (see §7), but beating it takes a hash that preserves density rather than one that scrambles.
- **Diff already exists** in the oracle: `champ::diff` plus `diff_data_node` / `diff_node_data` / `diff_data_data` / `diff_collisions`, short-circuiting on pointer equality, so O(|diff|) when `b` derives from `a`. Same algorithm as the immutable-js [efficient trie diff PR](https://github.com/immutable-js/immutable-js/pull/953). This is the baseline our diff addon has to beat.

## 3. Implementations worth reading

Ordered roughly by how much we can steal.

- **Clojure `PersistentHashMap`** — the reference HAMT. Three node kinds: `BitmapIndexedNode`, `ArrayNode`, `HashCollisionNode`, with the bitmap node promoted to a **dense 32-slot `ArrayNode` above 16 entries**. CHAMP drops that promotion. Whether the dense-node fast path is worth reintroducing is a measurable question. Best writeups: dmiller's 2024 ClojureCLR-next series ([part 1](https://dmiller.github.io/clojure-clr-next/general/2024/07/02/persistent-hash-map-part-1.html), [2](https://dmiller.github.io/clojure-clr-next/general/2024/07/02/persistent-hash-map-part-2.html), [3](https://dmiller.github.io/clojure-clr-next/general/2024/07/02/persistent-hash-map-part-3.html)) and [higher-order.net, 2010](http://blog.higher-order.net/2010/08/16/assoc-and-clojures-persistenthashmap-part-ii.html).
- **Scala 2.13 `immutable.HashMap`** — production CHAMP, [source](https://github.com/scala/scala/blob/2.13.x/src/library/scala/collection/immutable/HashMap.scala). Two bug reports are effectively a test plan for us: [scala/scala#6725](https://github.com/scala/scala/pull/6725) (value-overwrite and null-value semantics) and [scala-dev#525](https://github.com/scala/scala-dev/issues/525) (extra `equals` calls when looking up a *missing* key whose hash partially collides — a real CHAMP-specific regression). It also has an `updated` variant that shallowly mutates the root and immediate children.
- **Erlang/Elixir maps** ([docs](https://www.erlang.org/doc/system/maps.html), [erl_map.c](https://github.com/erlang/otp/blob/master/erts/emulator/beam/erl_map.c), [Jesper Andersen's "Breaking Erlang Maps" series](https://medium.com/@jlouis666/breaking-erlang-maps-1-31952b8729e6)) — **hybrid representation**: ≤32 entries is a "flatmap" (two sorted arrays, keys and values), above that it switches to a HAMT. Strong idea: for small maps a flat sorted pair of arrays beats any trie on every axis. Costs a representation tag and a conversion path.
- **Haskell `unordered-containers`** — [Tibell's HIW 2011 talk](https://wiki.haskell.org/wikiupload/6/65/HIW2011-Talk-Tibell.pdf), [blog on bulk creation](https://blog.johantibell.com/2012/03/improvements-to-hashmap-and-hashset.html). Lessons on bulk construction (`fromList`) and on the `SmallArray#` switch, i.e. shaving the array header off every node.
- **Rust `rpds` / `im` / `imbl`** — [rpds](https://github.com/orium/rpds) parameterizes the smart pointer via `archery` because **`Rc` vs `Arc` is up to 2x on clone-heavy workloads**. Direct evidence that our refcount must not be atomic by default. [michaelwoerister/hamt-rs](https://github.com/michaelwoerister/hamt-rs) and [jameysharp/persistent-map](https://github.com/jameysharp/persistent-map) differ on collision handling (chained list vs linear probing) — a cheap experiment.
- **OCaml HAMT, [Gagallium](https://gallium.inria.fr/blog/implementing-hamt-for-ocaml/)** — candid implementation report. Finds *lookups* far faster than OCaml's balanced `Map` (3.9s vs 7.9s) while *inserts* are slower (8.6s vs 7.4s), and misses faster than hits. Also: bulk-copy-then-mutate can lose to targeted inserts. Matches the shape we should expect.
- **Simple hash tries in C** — [Wellons, _An easy-to-implement, arena-friendly hash map_](https://nullprogram.com/blog/2023/09/30/) and [nrk, _Hash based trees and tries_](https://nrk.neocities.org/articles/hash-trees-and-tries). One struct, no bitmap: `{ key, value, node *slots[4] }`. Two hash bits per level, insert walks down to the first null slot and allocates there, uniform hash bits remove any need to rebalance. Wellons gets lock-free concurrency almost for free, since a pointer is only ever written once from null.

  Read as a challenge to CHAMP it does not hold up, and the reasons are instructive:
  - **Not persistent.** Insert mutates a shared node in place. Path copying restores persistence but costs the ABA-free property, costs the arena its fit (path copies are garbage an arena cannot reclaim), and copies about log4(n) nodes against CHAMP's log32(n) — 2.5x the dependent misses at a million entries, which is the exact bottleneck §7 is about.
  - **No deletion**, in either article, and not by oversight: an entry sits at an internal node with live children, so removing it means promoting a descendant. Wellons suggests gravestones and notes they go badly with an arena.
  - **Not canonical.** Shape depends on insertion order — of two keys sharing a hash prefix, whichever arrives first takes the shallower slot. It is a digital search tree, and those are history-dependent by construction. That forecloses O(1) structural equality and cross-lineage diff (§4).

  nrk's finding that **branching above 4 buys almost nothing while costing a lot of memory** is correct and specific to a design with no bitmap, where a node pays for all B pointers regardless of occupancy. It does not transfer: the bitmap is precisely the mechanism that decouples fanout from memory cost, and it is why CHAMP can afford 32-way branching. nrk's own numbers also put the trie ~38% behind a plain mutable hash table on insert (16,263 vs 11,816 cycles at 131,072 items), against a baseline chosen for avoiding resizes rather than for speed.
- **C HAMT: [mkirchner/hamt](https://github.com/mkirchner/hamt)**, **C++: [philsquared/hash_trie](https://github.com/philsquared/hash_trie)** — small readable codebases, useful for layout comparison.
- **[popcount.org, _Introduction to HAMT_](https://idea.popcount.org/2012-07-25-introduction-to-hamt/)** — the clearest short explanation of the bitmap+popcount trick.
- **Pony, [_Persistent Data Structures for Concurrent Programs_](https://www.ponylang.io/blog/2026/03/persistent-data-structures-for-concurrent-programs/) (Mar 2026)** — recent but derivative, a restatement of the CHAMP paper plus tail-optimized vectors. Listed only as evidence of where the state of the art currently sits: nothing new since 2018.

## 4. Diff, equality, reconciliation

The addon we care about most after the map itself.

- **Pointer-equality diff** — immer's `champ::diff` and the immutable-js PR above. O(|diff|) but only when the two maps share ancestry. This is the floor.
- **Incremental / multiset hashing** — AdHASH originates in Bellare & Micciancio, _A New Paradigm for Collision-Free Hashing_ (EUROCRYPT 1997), extended to multisets by [Clarke et al., _Incremental Multiset Hash Functions and Their Application to Memory Integrity Checking_](https://iacr.org/archive/asiacrypt2003/05_Session05/12_151/paper.pdf) (ASIACRYPT 2003). One commutative incremental hash per node gives O(1) probabilistic equality and lets diff skip equal subtrees **even when pointers differ**, generalizing immer's diff to unrelated maps. Cost: one word per node plus maintenance on every write. (The [champ-trie](https://github.com/YuriyKrasilnikov/champ-trie) Rust repo advertises exactly this as "AdHash" but has three commits and no benchmarks — the idea is sound, the repo is not evidence.)
- **Merkle Search Trees** — [Auvolat & Taïani, SRDS 2019](https://inria.hal.science/hal-02303490), [Rust implementation](https://github.com/domodwyer/merkle-search-tree). Order-preserving, deterministic structure (shape depends only on content), designed so two replicas can reconcile by comparing subtree hashes. The ordered-key analogue of what we would get by adding node hashes to CHAMP.
- **Prolly trees** (probabilistic B-trees) — [Dolt docs](https://docs.dolthub.com/architecture/storage-engine/prolly-tree), [DoltHub explainer](https://www.dolthub.com/blog/2024-03-03-prolly-trees/), [chunking post](https://www.dolthub.com/blog/2022-06-27-prolly-chunker/), [scaling post, 2025](https://www.dolthub.com/blog/2025-05-16-millions-of-versions/). Content-defined chunking makes node boundaries a function of content, so the tree is history-independent and diffs between *independently built* versions are cheap. The production answer to "fast diff between unrelated versions".
- **Range-based set reconciliation** — [Meyer, arXiv 2212.13567](https://arxiv.org/pdf/2212.13567) (SRDS 2023) and the [2026 follow-up on storage backends](https://arxiv.org/abs/2603.19820). Recursive range fingerprint comparison. Aimed at networked replicas, but the recursion is the same one a hash-annotated trie diff would run.
- **History independence** — the formal name for CHAMP's canonical form. [Uniquely represented data structures (Golovin, CMU)](http://reports-archive.adm.cs.cmu.edu/anon/2008/CMU-CS-08-135.pdf), and recent theory in [History-Independent Concurrent Hash Tables (STOC 2025, arXiv 2503.21016)](https://arxiv.org/pdf/2503.21016). Useful vocabulary, and the reason our canonicality tests should be stated as "shape is a function of content, full stop".

## 5. Memory management and mutation

- **Reinking, Xie, de Moura & Leijen, [_Perceus: Garbage Free Reference Counting with Reuse_](https://xnning.github.io/papers/perceus.pdf) (PLDI 2021), and FBIP.**
  Precise refcounting plus reuse analysis, so a functional update becomes an in-place mutation when the refcount is 1. The principled account of what immer's transients do by hand. In C++ we get the dynamic version nearly free: check `refs == 1` along the copy path and mutate instead of copying. That subsumes most of the need for a separate transient type, and it has to be decided before the node layout is fixed.
- **Clojure transients** — [hypirion, _Understanding Clojure's Transients_](https://hypirion.com/musings/understanding-clojure-transients). The explicit-ownership design, and why it needs a thread-owner token.
- **`Rc` vs `Arc`** — see rpds above. Up to 2x. Non-atomic by default, atomic as a policy.
- **Rodeh, _Deferred Reference Counters for Copy-On-Write B-trees_ (IBM TR)** — techniques for amortizing refcount traffic in a COW structure. Relevant if refcount churn shows up in profiles.
- **Arena allocation** — Wellons above. Trades "dead nodes accumulate" for O(1) alloc and batch free. Worth considering as an alternative *memory policy*, not as the default.

## 6. Concurrency (context, not a current goal)

- **Prokopec, Bronson, Bagwell & Odersky, [_Concurrent Tries with Efficient Non-Blocking Snapshots_](https://dl.acm.org/doi/10.1145/2145816.2145836) (PPoPP 2012).** Ctries: lock-free hash trie with O(1) snapshots via a generation-stamped indirection node. The mutable-with-snapshots dual of what we are building.
- **Prokopec, cache-tries ([PPoPP 2018](https://dl.acm.org/doi/10.1145/3200691.3178498), [analysis](https://arxiv.org/abs/1712.09636), [removal and compaction](http://aleksandar-prokopec.com/publications/cachetrie-remove/)).** Auxiliary "cache" array pointing directly at a deep trie level, turning O(log32 n) into expected O(1). Not transplantable as written, but the idea — a side table that skips the top levels — is the only published route to sub-logarithmic trie lookup we found. Filed.

## 7. Hardware and micro-optimization

- **Zhou et al., [_Cuckoo Trie_](https://arxiv.org/abs/2201.09331) (SOSP 2021).** Take the diagnosis, not the structure: trie traversal is a chain of dependent DRAM accesses that an out-of-order core cannot overlap, and that chain — not instruction count — is the bottleneck. Their fix is a hashed node representation so all levels prefetch in parallel. Our tractable version: fewer indirections per level (inline values), a shallower trie, and prefetching the next level before computing the offset.
- **Leis et al., ART (ICDE 2013) and [Binna et al., HOT](https://15721.courses.cs.cmu.edu/spring2019/papers/08-oltpindexes2/p521-binna.pdf) (SIGMOD 2018).** Adaptive per-occupancy node layouts and height minimization for ordered indexes. Mostly a control group — CHAMP's bitmap already is the compact-sparse-node answer, and adaptive layouts buy their space with branches. HOT is also the best written source on SIMD/BMI2 inside a trie node.
- **popcount and PEXT** — [Lemire/Langdale, _Bits to indexes in BMI2 and AVX-512_](https://branchfree.org/2018/05/22/bits-to-indexes-in-bmi2-and-avx-512/). `popcount(bitmap & (bit - 1))` is the hot instruction in every operation. Note that PEXT is microcoded and slow on pre-Zen3 AMD, so it is not a free win.
- **Hash quality** — [Fibonacci hashing (Probably Dance)](https://probablydance.com/2018/06/16/fibonacci-hashing-the-optimization-that-the-world-forgot-or-a-better-alternative-to-integer-modulo/), [Reynolds on avalanche](http://marc-b-reynolds.github.io/math/2019/08/10/Avalanche.html). `std::hash` on integers is the identity, so trie shape follows raw key bits: sequential keys give a dense shallow trie (good), pointer-like keys with zeroed low bits give a degenerate one (bad). immer does no mixing and inherits the bad case. A splitmix64-style finalizer costs a few cycles on the critical path and buys a shape that no longer depends on key distribution — plus, at 64-bit output, collision nodes become unreachable in practice (birthday bound around 2^32 keys) rather than merely rare.

## 8. Other structures, for later

- **Sequences.** [RRB-Trees](https://infoscience.epfl.ch/record/169879) (Bagwell & Rompf 2011) → [RRB Vector](https://dl.acm.org/doi/10.1145/2858949.2784739) (Stucki et al., ICFP 2015) → immer ICFP 2017 → Scala's [Vector rewrite](https://github.com/scala/scala/pull/8534). Practical lesson from the last: fixed fingers at *both* ends, as part of the structure, beat a single movable tail treated as a transient optimization. Background: [hypirion's persistent vector series](https://hypirion.com/musings/understanding-persistent-vector-pt-1).
- **Ordered maps.** [Rodeh, _B-trees, Shadowing, and Clones_](https://archive.kernel.org/oldwiki/btrfs.wiki.kernel.org/images-btrfs/6/68/Btree_TOS.pdf) (TOS 2008) — the COW B-tree behind btrfs/ZFS/LMDB. [Tonsky's persistent-sorted-set](https://github.com/tonsky/persistent-sorted-set) and [DataScript internals](https://tonsky.me/blog/datascript-internals/) — a persistent B+ tree that beats a red-black tree ~3x on range scans because nodes are contiguous arrays. [Stratified B-trees](https://arxiv.org/pdf/1103.4282) for fully-persistent external dictionaries.
- **Write-optimized.** [Hitchhiker trees](https://github.com/replikativ/hitchhiker-tree) (Greenberg, Strange Loop 2016), [intro](https://blog.datopia.org/2018/11/03/hitchhiker-tree/) — B+ tree with per-node append buffers so most writes touch only the root, then path-copy. The batching idea if we ever want cheap bulk updates.
- **Bulk operations.** [Blelloch, Ferizovic & Sun, _Just Join for Parallel Ordered Sets_](https://www.cs.cmu.edu/~blelloch/papers/BFS16.pdf) (SPAA 2016) and [_PAM_](https://arxiv.org/pdf/1612.05665) (PPoPP 2018). Union / intersection / difference with optimal work on persistent trees, all from a single `join` primitive. The trie analogue is a recursive node-wise merge with pointer-equality short-circuit — structurally the same recursion as diff. Read this before designing `merge`/`union`, not the CHAMP papers.
- **Theory.** [_Confluently Persistent Sets and Maps_](https://arxiv.org/pdf/1301.3388) for merge/diff complexity bounds. _Verified Persistent Catenable Deques_ (JFLA 2026) if we ever want a deque.

## 9. State of the art, as of 2026

Nothing substantive has been published on CHAMP itself since the 2018 HHAMT paper. The design is settled and recent activity is entirely in implementations (Scala 2.13, Clojure, Rust, Pony 2026). Where the field *has* moved is adjacent:
- content-addressed / history-independent structures for **diff and reconciliation** between unrelated versions (prolly trees, MSTs, range-based reconciliation),
- **memory-level parallelism** as the real bottleneck in trie traversal (Cuckoo Trie),
- **compiler-assisted in-place reuse** replacing hand-rolled transients (Perceus).

Those three are where a 2026 CHAMP can plausibly beat a 2015 one.
