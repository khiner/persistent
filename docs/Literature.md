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
- **No hash mixing.** immer feeds `Hash{}(key)` straight into the trie, and `std::hash<uint64_t>` is the identity. Trie shape is therefore fully determined by the raw low bits of the key. Measured (§10.2), this is a better bet than it looks: it is *optimal* for a dense key set, since a counter gives a perfectly full trie with every key at the same depth, and modest strides cost only the few levels their zeroed low bits waste. It takes a large alignment before it really hurts. We are not bound by it (see §7), but beating it takes a hash that preserves density rather than one that scrambles.
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

## 10. What to carry into the design

**Bit-exact parity with immer is not a goal.** The oracle is behavioural: same key-to-value mapping, same size, same iteration contents. Two consequences worth stating separately, because they are easy to conflate:

- Everything about *shape* is now ours to choose — `B`, hash mixing, node layout, collision strategy, small-map special cases.
- **Canonicality is not something we gave up.** CHAMP's canonical form earns its keep on its own merits (cheap equality, cheap diff, history independence per §4), independent of immer. Keep it, and assert it with our own invariant check in the shape of immer's `check_champ()` — self-referential now rather than differential, since we can no longer diff our trie against theirs structurally.

Design positions, in descending confidence:

1. **Values inline in the node**, paper-faithful, against immer's separate values block: one allocation and one indirection fewer per node. This was the main performance thesis before, and it is now measured. It holds everywhere except one shape, and the exception is worth stating precisely because it is a property of the thesis and not a bug.

   Lookup cost oscillates with `n`, on random keys. (Everything in this entry was measured with random keys and a multiplicative hash. A dense key set does not oscillate at all -- see position 2 -- and any bijection scatters random keys the same way, so the shape of the finding stands.) Terminal occupancy is `n / B^depth`, so it cycles through 1 to `2^B` for every factor of `2^B` in `n`, and the margin against immer cycles with it. Both libraries oscillate and the phase differs because the branching factor does, which is why B=5 and B=6 come out almost exactly complementary. Lookup hit, ns/op, min of three runs:

   | n | 1e3 | 2e3 | 4e3 | 8e3 | 1.6e4 | 3.2e4 | 6.4e4 | 1.28e5 | 2.56e5 | 5.12e5 | 1e6 |
   |---|---|---|---|---|---|---|---|---|---|---|---|
   | B=5 | 6.8 | 4.7 | 4.5 | 6.1 | 9.3 | 9.6 | 7.3 | 7.4 | 9.9 | 14.0 | 16.5 |
   | **B=6** | **4.0** | 6.3 | 6.9 | **4.4** | **3.9** | **4.6** | 6.8 | 10.1 | 11.0 | **8.8** | **10.4** |
   | immer | 5.6 | 4.3 | 4.3 | 5.7 | 6.9 | 7.9 | 6.7 | 7.2 | 9.2 | 13.1 | 17.9 |

   B=6 wins 6 of 11 sizes by 23–43% and loses 5 by 1–60%, for a geometric mean 11% ahead. B=5 is flatter and worse, winning 1 of 11. Where one is bad the other is good, so a branching factor picked per map would be near-uniformly ahead — but B is baked into the shape of the trie, and a map cannot change it without being rebuilt.

   **What the oscillation actually is.** Not node size, and not the cache: padding `Entry` from 16 bytes to 32, doubling the byte spread of every node and the footprint of the whole map, changes lookup by under 2% at every size. What it is instead is the spread of *depths* a walk can end at. Occupancy near `2^B` is exactly the point where a level is half resolved, so terminal depth is split most evenly between two levels, and the walk's trip count stops being predictable. Timing the same lookups grouped by terminal depth instead of shuffled is worth 44–69%:

   | n | depth mix | shuffled | grouped |
   |---|---|---|---|
   | 1e3 | 80% / 20% | 8.6 | 3.8 |
   | 2e3 | 62% / 38% | 11.2 | 3.5 |
   | 4e3 | 38% / 61% | 9.1 | 2.9 |
   | 8e3 | 15% / 83% | 4.3 | 2.4 |

   Grouping buys locality as well as predictability, so that number is an upper bound on the branch effect alone. But the ranking is unambiguous, and it lands on the one thing a canonical trie cannot negotiate: CHAMP puts every entry at the shallowest level where its hash prefix is unique, so terminal depth is a distribution and not a constant. immer inherits it too.

   Things that do not fix it, all measured: prefetching the entry region a level ahead is 3–6% *worse*; the extra popcount is not the cost, since the gap survives ARMv9.4 scalar popcount (`+cssc`, worth ~20% to us and ~24% to immer, and machine-specific besides); B=4 is worse than both 5 and 6 at every size tried; and **hoisting each child's datamap into its parent** — which does remove one dependent load, and does buy 12% on lookup at 1e5 — costs 8 bytes per child and comes out a net loss, 7% off build and erase at 1e5 and 16–20% off at 1e6. Splitting keys from values would halve the byte spread, which the padding result says is not what costs.

   **Where the thesis costs, now that everything else has been paid off: looking up keys that are not there, in a band of sizes around a million.** Three explanations were tested and ruled out. Not shape: simulating the walk gives our trie and immer's the same node count and the same visits per failed lookup, on all four key patterns, to two decimals. Not the allocator: building with every node malloc'd and freed, no reuse at all and the same compiler flags, leaves the same cells losing by the same margins and gives up 5–22% on build. Not the walk: immer's inner loop carries an extra node-kind check ours does not, and is otherwise the same instructions in the same dependent chain.

   What is left is bytes. Swept against `n` on random keys, the miss margin is +13% at 200k, crosses over by 400k, bottoms out near -12% at 800k, and recovers to -6% at 1.4M — a working set outgrowing a cache rather than anything in the code. A hit reads the entry out of the node the walk is already on, where immer reads it out of a separate block, and that is the 15–25% we take on the hit row. A miss reads no value, so immer skips that block and pays nothing for having it, while we carry its 16MB of entries inside the region the walk covers either way: 23.4MB of node memory against immer's 12.4MB. Padding `Entry` to 32 bytes moves the same region the other way and costs 8 points of miss margin and 11 of hit, so it is the bytes and not the branch. Across repeated sweeps the lookup-miss row is the only one that reproduces as a loss, 1–13% on two to four of the twelve key shapes, against a 44% geometric-mean margin over the rest. Splitting keys from values inside the node addresses it directly, and was built and measured: it halves the loss and costs more than that back on the hit row. See the open questions. Nothing else tried moved it — not the order of the two bitmap tests, a branchless terminal, the slab size, or putting the entries in front of the children, which is 18% worse.
2. **Fold the hash, do not scramble it.** One xor-shift, `x ^= x >> 32`. This position started as "mix the hash, a splitmix64 finalizer", and measuring it is what changed it — twice, since the second xor-shift it used to have is gone too.

   The original argument was right about the hazard and wrong about the remedy. The hazard is real: keys that arrive aligned — heap addresses, anything scaled by a power of two — have low bits that never vary, and an identity hash spends whole levels resolving nothing. What the argument missed is that scrambling has a cost, and the cost is bigger than the hazard. A dense key set is the *best* case a trie has: every node full, every key at the same depth, neighbouring keys in the same node. Counters, object ids and table row numbers hand that over for free, and any good mixer destroys it. immer, hashing with the identity, keeps it.

   A fold keeps both. Each xor-shift is a bijection that maps `[0, 2^k)` into itself, so a dense key set stays dense, while the folding still fills in low bits that never vary from bits that do. Measured at a million entries against immer, going from the multiply to the fold:

   | | lookup hit | build move | iterate |
   |---|---|---|---|
   | sequential | -77% → **-21%** | -105% → **-42%** | -133% → **-62%** |
   | pointer, 16-byte stride | -112% → **-7%** | -82% → **+34%** | -127% → **+21%**|
   | pointer, 64-byte stride | -63% → **-30%** | -30% → **+28%** | +52% → **+59%** |
   | random | +42% → +38% | +42% → +43% | +14% → +17% |

   Random keys are indifferent, as they must be — any bijection scatters them equally. Everything else improves, several from a rout to a win.

   What is left is that a dense counter is *exactly* the key set an identity hash is optimal for, and we are still 21% behind on lookup there. Density also costs on the write side, since it puts every key at the maximum depth and so makes every path copy full length: build-copy on sequential keys is the one row the fold made worse.

   **This is the row of the benchmark that was missing.** Until now every measurement used uniformly random 64-bit keys, which is the one distribution that cannot tell an identity hash from a good one. `HamtBench all` now sweeps a counter and two heap-address strides as well.

   **The second xor-shift then had to go as well, and for the same reason one step further in.** "Each xor-shift maps `[0, 2^k)` into itself, so a dense key set stays dense" is true, and it is also the wrong invariant. A counter is dense on an *interval*. A heap address is dense on a *stride*, and a stride is not an interval: folding bits 16 and up into the low four leaves the set a permutation of itself but no longer a lattice, and the trie that was going to be full is scattered instead. Below 65536 keys the folded bits do not vary and nothing happens, which is why this only shows at scale and why the first round of measurements missed it. Simulated at `B = 5`, terminal nodes and entries per node:

   | | `x ^= x >> 32` | with `x ^= x >> 16` too |
   |---|---|---|
   | pointer x64, 1e5 | 16914 nodes, 5.9 each | 68289 nodes, 1.5 each |
   | pointer x16, 1e6 | 67651 nodes, 14.8 each | 99361 nodes, 10.1 each |
   | sequential, 1e6 | 33825 nodes, 29.6 each | unchanged |
   | multiplicative, for scale | | 310202 nodes, 3.2 each |

   Four times the nodes for the same entries, on the key shape the second fold was meant to protect. What it protected against — keys whose low bits never vary at all — costs one level at the root, which is cheaper than a trie four times as loose. The remaining fold covers the case a stride does not: keys that differ only above bit 32.

   Where the sequential deficit used to come from is worth keeping, because it is what settled `B`. A million dense keys occupy 20 bits. Five divides 20 and six does not, so at `B = 6` the last level held four of its sixty-four slots where immer's held thirty-two — eight times the nodes to allocate while building and eight times the pointers to chase while iterating. That is not a property of the hash and it was never going to be fixed by one. It is fixed by `B = 5`, where our terminal occupancy is immer's by construction. See the first open question.
3. **Nodes come from a slab per size, not from one free list per size.** Freed nodes are recycled either way, and the memory stays ours until exit either way. What changed is *which* node a build gets handed back, and that turned out to be worth more than anything inside the node.

   A single list per size returns whatever was released last, from wherever it came from. A process that has built and dropped a large map has lists full of that map's leavings spread over everything it ever touched, and the next map is assembled out of them. The benchmark runs twelve configurations in one process and so measures exactly this, which is why the same map was fast alone and slow in the suite: at a million 64-byte-strided keys, lookup ran 66% slower in the suite than in a fresh process — a 21% win over immer turned into a 29% loss — while immer's own number moved 3%. Nothing about the trie had changed. The map had simply been built out of the wrong memory.

   Cutting fresh nodes from a slab per size, and returning each freed node to the slab it came from rather than to a shared list, fixes it: a size's reuse stays inside the slabs that size has already taken, so a map's nodes stay near one another however much has been dropped before it. Slabs are aligned to their own size, so a release finds a node's slab by masking the pointer and needs no lookup. Nineteen losing cells against immer became twelve, and the free list's write-side advantage stayed — dropping the free list altogether fixes the same reads but costs 30–100% on build.

   Two details measured. The slab a release makes partial goes to the **front** of its size's list, not behind the slab being filled: the node just freed is the one a build wants next, since growing a node releases the size below it and the next node to grow asks for that size again. Behind was 7% worse. And node sizes are rounded up to a **pair of words**, so a 16-byte header never straddles a cache line — one visit in eight otherwise, worth about 3% on cold lookups for four bytes a node.
4. **Refcount-1 in-place mutation** rather than an explicit transient type — Perceus's insight applied dynamically. Non-atomic refcount by default.
5. **Diff: pointer-equality short-circuit first.** Per-node incremental multiset hash only if measurement justifies the extra word — but it is the single change that would make our diff strictly better than immer's rather than equal to it.
6. **`B` is settled at 5 by measurement, not inherited.** See below.

**Baseline.** immer is the control — mature, persistent, same API shape, already submoduled and already driven by the test. If we do not beat it, that is the finding. It also gives `B = 5` versus `B = 6` for free. A second, optional floor is `std::unordered_map` plus a full copy per update, which says at what size structural sharing starts paying for itself. Nothing else earns its keep as a control: anything simpler than CHAMP that is worth comparing against has to be made persistent first, at which point it is no longer the simple thing.

## 11. Open questions

Roughly in order of how much they matter.

- ~~**`B = 5` or `B = 6`?**~~ **Answered again, the other way: B = 5**, and it is the default. B=6 was the read-side answer while reads were slower than they had to be. Once the allocator stopped scattering nodes (position 3) and the lookup stopped returning an `optional` (below), the depth B=6 buys stopped paying for what it costs, and what it costs is terminal occupancy.

  Occupancy at the last level is `2^(W mod B)`, where `W` is the number of hash bits a key set varies over, so how full the terminal nodes are depends on `B` against the key set. B=5 is the only choice that cannot come out behind immer, and the argument is short. A walk consumes the hash in fixed windows, so a terminal node holds the bits between the last window boundary below `W` and `W` itself. If our boundaries are a subset of immer's, our terminal window starts no later than immer's and our occupancy is at least immer's for every `W`. immer's boundaries are the multiples of 5, a window cannot exceed 6 bits because a slot map has to fit a machine word, and the only subset of the multiples of 5 whose gaps are all at most 6 is the whole set.

  So at B=5 our occupancy is immer's by construction on every key shape — which throws every shape-driven row back onto per-node cost, where a 16-byte header against immer's 32 and inline entries against its separate values block put us ahead. At B=6 the lottery is independent, and the benchmark's key widths (10, 17 and 20 bits for a counter, 14, 21 and 24 for a 16-byte stride) are unkind: a million dense keys give B=6 four entries a leaf against immer's thirty-two, and a thousand 16-byte-strided pointers give four against sixteen. Measured over the whole sweep, same code, both branching factors:

  | | cells slower than immer, of 84 | geometric-mean margin |
  |---|---|---|
  | **B = 5** | **3**, all of them the lookup-miss row, 1–13% | +44% |
  | B = 6 | 6, including iteration 19–87% behind on two shapes | +49% |

  B=6 has the better average and the worse tail. Its two bad shapes are bad because six does not divide their key widths, and no hash fixes that: a rotation that helps a twenty-bit counter hurts a ten-bit one. B=5's three losses are all the same row and all the same cause, below.

  B=6 still wins the lookup rows it always won — one fewer dependent chase at a million entries is real — and the knob is still there for a lookup-dominated workload on keys that happen to suit it. It is no longer the default.
- ~~**Should `Get` return `std::optional<uint64_t>`?**~~ **Answered: no, it returns `const uint64_t *`**, like immer's `find`. Letting a caller who only reads the value skip the copy is the smaller half. The larger half is that merging the found and not-found paths into one `optional` in registers made clang reconstruct the value on the hit path, and the walk is short enough for that to show: 16–20% on lookup at a thousand keys, and it turned the pointer-stride lookup rows from losses into wins.
- ~~**Key/value layout inside the node: interleaved pairs or split key and value arrays?**~~ **Answered: interleaved.** The argument for splitting was right about the mechanism: a walk reads keys and never reads a value, so two arrays take the 8MB of values at a million entries out of the region a walk covers, 23.4MB down to 15.4MB against immer's 12.4MB of node and child memory. Measured on its own it did that — the two lookup-miss cells that reproduce as losses went from -9…-13% and -6…-12% to -7% and -2%, and iteration improved from +22% to +36%, since two dense streams beat one interleaved one.

  Across the whole sweep it lost: five cells slower than immer instead of two, and the geometric mean down from +42.1% to +40.4%. The cost is every lookup that hits, which now reads from two places instead of one — 17% down to 12% on random keys, enough to turn three pointer-stride cells from wins into losses. It also costs the iterator its `const Entry &`.
- **Small-map flat representation** (Erlang's ≤32 sorted arrays) — now permitted. But CHAMP already handles small maps as a single root node with inline values, and Erlang's win comes partly from BEAM's heavier HAMT. Measure before building the second code path.
- ~~**Bucketed leaves**~~ — **answered: no.** Letting a slot hold up to K entries in a small leaf instead of forcing a child, splitting only above K, is canonical (the rule is a function of contents) and it looks compelling on paper: the level at which a key resolves drops from "where my prefix is unique" to "where my subtrie has at most K keys", which is half a level shallower and far more concentrated. That is the wrong metric. A leaf is itself a node, so reaching an entry inside one costs the hop the shallower resolution saved, and only a subtrie of exactly one key is inline in its parent. Counted as node visits, K=8 against K=1 over ten sizes from 1e3 to 1e6: 3.39 against 3.39 at 1.28e5, 3.98 against 4.04 at 1e6, and inside a percent everywhere else. Concentration barely moves either. It buys nothing and costs a second node kind, a linear scan, and a merge rule on erase. The same arithmetic applies to any bucketing scheme, Erlang's flatmap included, whenever the bucket is reached through a pointer rather than stored inline.
- **Dense array node above some occupancy** (Clojure's `ArrayNode`) — now permitted, and it does not break canonicality if the promotion threshold is a pure function of occupancy. Trades memory for a skipped popcount. Low expected value against CHAMP's compact array, but cheap to test.
- **Cache the hash in the leaf, or recompute?** Doubles entry size for a uint64→uint64 map, so probably no here, but it is the right answer for expensive-to-hash keys and it makes collision comparison trivial (see scala-dev#525).
- ~~**Node header packing.**~~ **Answered at B=5: 16 bytes, and both counts fit.** The header is a refcount, the popcounts of both bitmaps, and the bitmaps: 4 + 2 + 2 + 4 + 4 at B=5. The entry count moved in beside the child count for nothing, because the word the refcount shares was half spare and the bitmaps have to start 8-byte aligned regardless — and reading a count beats recomputing it in every pass that walks a node, which on this target means a round trip through the vector unit rather than an instruction. 16 is also the floor: dropping both counts leaves 12, which alignment rounds straight back to 16, since the arrays behind the header are pointers and 64-bit entries. Going below means moving the refcount out of the node entirely, and the only place to put it — a side array indexed by slab block — needs a division by a block size that is not a power of two. At B=6 the bitmaps are two full words and the header is 24, which is where the old entry stood.

  What is *not* free is the padding the allocator adds: node sizes round up to a pair of words so headers stay 16-byte aligned and never straddle a cache line, which costs four bytes a node on average and buys about 3% on cold lookups. See position 3.
