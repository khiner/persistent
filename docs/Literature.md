# Literature

Running notes on the work that informs this project. Each entry says what to take from it.

## 1. Core lineage

Read in this order.

**Bagwell, [_Ideal Hash Trees_](https://lampwww.epfl.ch/papers/idealhashtrees.pdf) (EPFL TR, 2001), building on his _Fast and Space Efficient Trie Searches_ (2000).**
Array-mapped trie, sparse node = bitmap + popcount-indexed compact array. Everything below builds on this.

**Steindorfer & Vinju, [_Optimizing Hash-Array Mapped Tries for Fast and Lean Immutable JVM Collections_](https://michael.steindorfer.name/publications/oopsla15.pdf) (OOPSLA 2015).**
The CHAMP paper — the spec we are implementing. Three changes over HAMT:
1. Split the single bitmap into `datamap` + `nodemap`.
2. Store payloads and child pointers in separate regions of one array, data front-loaded, children from the back.
3. Maintain a canonical form by collapsing singleton subtrees back inline on delete.

Reported 1.3–6.7x iteration and 3–25.4x equality over HAMT, with a smaller footprint. Canonical form makes same content give the same shape, and fast equality and fast diff both depend on that.

**Steindorfer, [_Efficient Immutable Collections_](https://homepages.cwi.nl/~jurgenv/phdtheses/PhDThesis_Michael_Steindorfer.pdf) (PhD thesis, UvA 2017).**
The long form of the OOPSLA paper: the node-layout arithmetic, the memory-footprint methodology, and the delete and compaction rules stated precisely.

**Steindorfer & Vinju, HHAMT — [_To-Many or To-One? All-in-One!_](https://michael.steindorfer.name/publications/pldi18.pdf) (PLDI 2018), [arXiv 1608.01036](https://arxiv.org/pdf/1608.01036).**
Heterogeneous payloads via an extra bitmap per node, so one node type serves set, map, and multimap, with small values unboxed. `Set` and `Map` here are two instantiations of one trie, sharing the code and the allocator and compiling a node walk apiece. One node type for both would take this.

**Bolívar Puente, [_Persistence for the Masses: RRB-Vectors in a Systems Language_](https://public.sinusoid.es/misc/immer/immer-icfp17.pdf) (ICFP 2017).**
immer's paper. The half to take is memory policies, refcounting, and transients: the only paper here on doing this in C++ with no GC.

## 2. What immer does

Read from the `immer/` submodule, since it is our oracle.

- `B = 5` (`immer/config.hpp:151`), `bitmap_t = uint32_t`, `hash_t = size_t` (64-bit), so `max_depth = 13` and collision nodes appear only at `shift == 65`.
- Two node kinds (`immer/detail/hamts/node.hpp`): `inner_t { nodemap, datamap, values*, children[] }` and `collision_t { count, values[] }`.
- **Divergence from the paper:** immer puts values in a separately allocated `values_t` block with its own refcount and `ownee`, so a data hit is an extra pointer chase and a node is two allocations. The trade is for transience: the values block can be shared and mutated independently.
- immer canonicalizes on erase (`do_sub`: singleton collapse via `copy_inner_replace_inline`, collision 2 to 1, root special-cased at `shift == 0`) and ships `check_champ()` to assert it, so bit-exact parity is testable structurally as well as through `Get`.
- **No hash mixing.** immer feeds `Hash{}(key)` straight into the trie, and `std::hash<uint64_t>` is the identity, so trie shape is the raw low bits of the key. That is optimal for a dense key set, a counter giving a full trie with every key at the same depth, and a stride costs the levels its zeroed low bits waste. Beating it takes a hash that preserves density (§7).
- **Diff already exists** in the oracle: `champ::diff` plus `diff_data_node` / `diff_node_data` / `diff_data_data` / `diff_collisions`, short-circuiting on pointer equality, so O(|diff|) when `b` derives from `a`. Same algorithm as the immutable-js [efficient trie diff PR](https://github.com/immutable-js/immutable-js/pull/953).

## 3. Implementations worth reading

Ordered by how much is taken from each.

- **Clojure `PersistentHashMap`** — the reference HAMT. Three node kinds: `BitmapIndexedNode`, `ArrayNode`, `HashCollisionNode`, with the bitmap node promoted to a **dense 32-slot `ArrayNode` above 16 entries**. CHAMP drops that promotion. Best writeups: dmiller's 2024 ClojureCLR-next series ([part 1](https://dmiller.github.io/clojure-clr-next/general/2024/07/02/persistent-hash-map-part-1.html), [2](https://dmiller.github.io/clojure-clr-next/general/2024/07/02/persistent-hash-map-part-2.html), [3](https://dmiller.github.io/clojure-clr-next/general/2024/07/02/persistent-hash-map-part-3.html)) and [higher-order.net, 2010](http://blog.higher-order.net/2010/08/16/assoc-and-clojures-persistenthashmap-part-ii.html).
- **Scala 2.13 `immutable.HashMap`** — production CHAMP, [source](https://github.com/scala/scala/blob/2.13.x/src/library/scala/collection/immutable/HashMap.scala). Two bug reports are a test plan: [scala/scala#6725](https://github.com/scala/scala/pull/6725) (value-overwrite and null-value semantics) and [scala-dev#525](https://github.com/scala/scala-dev/issues/525) (extra `equals` calls when looking up a *missing* key whose hash partially collides — a CHAMP-specific regression). It also has an `updated` variant that shallowly mutates the root and immediate children.
- **Erlang/Elixir maps** ([docs](https://www.erlang.org/doc/system/maps.html), [erl_map.c](https://github.com/erlang/otp/blob/master/erts/emulator/beam/erl_map.c), [Jesper Andersen's "Breaking Erlang Maps" series](https://medium.com/@jlouis666/breaking-erlang-maps-1-31952b8729e6)) — **hybrid representation**: ≤32 entries is a flatmap, two sorted arrays of keys and values, and above that a HAMT. The flat form beats any trie on every axis at that size, at the cost of a representation tag and a conversion path.
- **Haskell `unordered-containers`** — [Tibell's HIW 2011 talk](https://wiki.haskell.org/wikiupload/6/65/HIW2011-Talk-Tibell.pdf), [blog on bulk creation](https://blog.johantibell.com/2012/03/improvements-to-hashmap-and-hashset.html). Lessons on bulk construction (`fromList`) and on the `SmallArray#` switch, which removes the array header from every node.
- **Rust `rpds` / `im` / `imbl`** — [rpds](https://github.com/orium/rpds) parameterizes the smart pointer via `archery` because **`Rc` vs `Arc` is up to 2x on clone-heavy workloads**. Evidence for a plain integer refcount by default, atomic as a policy. [michaelwoerister/hamt-rs](https://github.com/michaelwoerister/hamt-rs) and [jameysharp/persistent-map](https://github.com/jameysharp/persistent-map) differ on collision handling (chained list vs linear probing).
- **OCaml HAMT, [Gagallium](https://gallium.inria.fr/blog/implementing-hamt-for-ocaml/)** — lookups faster than OCaml's balanced `Map` (3.9s vs 7.9s), inserts slower (8.6s vs 7.4s), misses faster than hits, and bulk-copy-then-mutate can lose to targeted inserts.
- **Simple hash tries in C** — [Wellons, _An easy-to-implement, arena-friendly hash map_](https://nullprogram.com/blog/2023/09/30/) and [nrk, _Hash based trees and tries_](https://nrk.neocities.org/articles/hash-trees-and-tries). One struct, no bitmap: `{ key, value, node *slots[4] }`. Two hash bits per level, insert walks down to the first null slot and allocates there, uniform hash bits keep it balanced. Wellons gets lock-free concurrency almost for free, since a pointer is only ever written once from null.

  As an alternative to CHAMP it falls short in three ways:
  - **Not persistent.** Insert mutates a shared node in place. Path copying restores persistence but loses the ABA-free property, leaves garbage an arena cannot reclaim, and copies about log4(n) nodes against CHAMP's log32(n), 2.5x the dependent misses at a million entries, the bottleneck of §7.
  - **No deletion**, in either article: an entry sits at an internal node with live children, so removing it means promoting a descendant. Wellons suggests gravestones and notes an arena cannot reclaim them.
  - **Not canonical.** Shape depends on insertion order, since of two keys sharing a hash prefix the first takes the shallower slot. That rules out O(1) structural equality and cross-lineage diff (§4).

  nrk's finding that **branching above 4 buys little and costs much memory** is specific to a design with no bitmap, where a node pays for all B pointers regardless of occupancy. The bitmap decouples fanout from memory cost, which is why CHAMP affords 32-way branching. nrk's numbers also put the trie ~38% behind a plain mutable hash table on insert (16,263 vs 11,816 cycles at 131,072 items), against a baseline sized to avoid resizes.
- **C HAMT: [mkirchner/hamt](https://github.com/mkirchner/hamt)**, **C++: [philsquared/hash_trie](https://github.com/philsquared/hash_trie)** — small readable codebases, useful for layout comparison.
- **[popcount.org, _Introduction to HAMT_](https://idea.popcount.org/2012-07-25-introduction-to-hamt/)** — the clearest short explanation of the bitmap+popcount trick.
- **Pony, [_Persistent Data Structures for Concurrent Programs_](https://www.ponylang.io/blog/2026/03/persistent-data-structures-for-concurrent-programs/) (Mar 2026)** — a restatement of the CHAMP paper plus tail-optimized vectors, and evidence that nothing new has appeared since 2018.

## 4. Diff, equality, reconciliation

- **Pointer-equality diff** (§2) is the floor: O(|diff|) when the two maps share ancestry.
- **Incremental / multiset hashing** — AdHASH originates in Bellare & Micciancio, _A New Paradigm for Collision-Free Hashing_ (EUROCRYPT 1997), extended to multisets by [Clarke et al., _Incremental Multiset Hash Functions and Their Application to Memory Integrity Checking_](https://iacr.org/archive/asiacrypt2003/05_Session05/12_151/paper.pdf) (ASIACRYPT 2003). One commutative incremental hash per node gives O(1) probabilistic equality and lets diff skip equal subtrees **even when pointers differ**, generalizing immer's diff to unrelated maps. Cost: one word per node plus maintenance on every write.
- **Merkle Search Trees** — [Auvolat & Taïani, SRDS 2019](https://inria.hal.science/hal-02303490), [Rust implementation](https://github.com/domodwyer/merkle-search-tree). Order-preserving, deterministic structure (shape depends only on content), designed so two replicas can reconcile by comparing subtree hashes. The ordered-key analogue of CHAMP with node hashes.
- **Prolly trees** (probabilistic B-trees) — [Dolt docs](https://docs.dolthub.com/architecture/storage-engine/prolly-tree), [DoltHub explainer](https://www.dolthub.com/blog/2024-03-03-prolly-trees/), [chunking post](https://www.dolthub.com/blog/2022-06-27-prolly-chunker/), [scaling post, 2025](https://www.dolthub.com/blog/2025-05-16-millions-of-versions/). Content-defined chunking makes node boundaries a function of content, so the tree is history-independent and diffs between *independently built* versions are cheap.
- **Range-based set reconciliation** — [Meyer, arXiv 2212.13567](https://arxiv.org/pdf/2212.13567) (SRDS 2023) and the [2026 follow-up on storage backends](https://arxiv.org/abs/2603.19820). Recursive range fingerprint comparison. Aimed at networked replicas, but the recursion is the same one a hash-annotated trie diff would run.
- **History independence** — the formal name for CHAMP's canonical form. [Uniquely represented data structures (Golovin, CMU)](http://reports-archive.adm.cs.cmu.edu/anon/2008/CMU-CS-08-135.pdf), and recent theory in [History-Independent Concurrent Hash Tables (STOC 2025, arXiv 2503.21016)](https://arxiv.org/pdf/2503.21016). Useful vocabulary, and the form of the canonicality tests: shape is a function of content.

## 5. Memory management and mutation

- **Reinking, Xie, de Moura & Leijen, [_Perceus: Garbage Free Reference Counting with Reuse_](https://xnning.github.io/papers/perceus.pdf) (PLDI 2021), and FBIP.**
  Precise refcounting plus reuse analysis, so a functional update becomes an in-place mutation when the refcount is 1, the principled form of immer's transients. In C++ the dynamic version is a `refs == 1` check along the copy path, and covers most of the need for a separate transient type.
- **Clojure transients** — [hypirion, _Understanding Clojure's Transients_](https://hypirion.com/musings/understanding-clojure-transients). The explicit-ownership design, and why it needs a thread-owner token.
- **Rodeh, _Deferred Reference Counters for Copy-On-Write B-trees_ (IBM TR)** — techniques for amortizing refcount traffic in a COW structure.
- **Arena allocation** — Wellons above. O(1) allocation and a batch free, with dead nodes accumulating until then. An alternative memory policy.

## 6. Concurrency (context)

- **Prokopec, Bronson, Bagwell & Odersky, [_Concurrent Tries with Efficient Non-Blocking Snapshots_](https://dl.acm.org/doi/10.1145/2145816.2145836) (PPoPP 2012).** Ctries: lock-free hash trie with O(1) snapshots via a generation-stamped indirection node. The mutable-with-snapshots dual of what we are building.
- **Prokopec, cache-tries ([PPoPP 2018](https://dl.acm.org/doi/10.1145/3200691.3178498), [analysis](https://arxiv.org/abs/1712.09636), [removal and compaction](http://aleksandar-prokopec.com/publications/cachetrie-remove/)).** A side table pointing directly at a deep trie level, turning O(log32 n) into expected O(1): the only published route to sub-logarithmic trie lookup we found.

## 7. Hardware and micro-optimization

- **Zhou et al., [_Cuckoo Trie_](https://arxiv.org/abs/2201.09331) (SOSP 2021).** The diagnosis: trie traversal is a chain of dependent DRAM accesses an out-of-order core cannot overlap. Their fix is a hashed node representation so all levels prefetch in parallel.
- **Leis et al., ART (ICDE 2013) and [Binna et al., HOT](https://15721.courses.cs.cmu.edu/spring2019/papers/08-oltpindexes2/p521-binna.pdf) (SIGMOD 2018).** Adaptive per-occupancy node layouts and height minimization for ordered indexes. CHAMP's bitmap is already the compact sparse node, and adaptive layouts buy their space with branches. HOT is the best written source on SIMD/BMI2 inside a trie node.
- **popcount and PEXT** — [Lemire/Langdale, _Bits to indexes in BMI2 and AVX-512_](https://branchfree.org/2018/05/22/bits-to-indexes-in-bmi2-and-avx-512/). `popcount(bitmap & (bit - 1))` is the hot instruction in every operation. PEXT is microcoded and slow on pre-Zen3 AMD.
- **Hash quality** — [Fibonacci hashing (Probably Dance)](https://probablydance.com/2018/06/16/fibonacci-hashing-the-optimization-that-the-world-forgot-or-a-better-alternative-to-integer-modulo/), [Reynolds on avalanche](http://marc-b-reynolds.github.io/math/2019/08/10/Avalanche.html). A splitmix64-style finalizer costs a few cycles on the critical path and buys a shape independent of the key distribution (§2), and at 64-bit output collision nodes are unreachable in practice, the birthday bound being around 2^32 keys.

## 8. Other structures, for later

- **Ordered maps.** [Rodeh, _B-trees, Shadowing, and Clones_](https://archive.kernel.org/oldwiki/btrfs.wiki.kernel.org/images-btrfs/6/68/Btree_TOS.pdf) (TOS 2008) — the COW B-tree behind btrfs/ZFS/LMDB. [Tonsky's persistent-sorted-set](https://github.com/tonsky/persistent-sorted-set) and [DataScript internals](https://tonsky.me/blog/datascript-internals/) — a persistent B+ tree that beats a red-black tree ~3x on range scans because nodes are contiguous arrays. [Stratified B-trees](https://arxiv.org/pdf/1103.4282) for fully-persistent external dictionaries.
- **Write-optimized.** [Hitchhiker trees](https://github.com/replikativ/hitchhiker-tree) (Greenberg, Strange Loop 2016), [intro](https://blog.datopia.org/2018/11/03/hitchhiker-tree/) — B+ tree with per-node append buffers so most writes touch only the root, then path-copy.
- **Bulk operations.** [Blelloch, Ferizovic & Sun, _Just Join for Parallel Ordered Sets_](https://www.cs.cmu.edu/~blelloch/papers/BFS16.pdf) (SPAA 2016) and [_PAM_](https://arxiv.org/pdf/1612.05665) (PPoPP 2018). Union / intersection / difference with optimal work on persistent trees, all from a single `join` primitive. The trie analogue is a recursive node-wise merge with a pointer-equality short-circuit, the same recursion as diff.
- **Theory.** [_Confluently Persistent Sets and Maps_](https://arxiv.org/pdf/1301.3388) for merge/diff complexity bounds. _Verified Persistent Catenable Deques_ (JFLA 2026) for a deque.

## 9. The strict vector

The other structure implemented here: a trie indexed by the bits of the index, with every level full but the rightmost path, so an index is a path of shifts and masks.

- **Clojure `PersistentVector`** — the reference. 32-way radix balanced trie, plus the **tail**: the last partial chunk held beside the trie, so an append writes one node and reaches the trie once per chunk, and the trie holds a whole number of chunks. [hypirion's series](https://hypirion.com/musings/understanding-persistent-vector-pt-1) is the clearest account.
- **Bolívar Puente, ICFP 2017** (§1) — the C++ account. Its `vector` is this structure and its `flex_vector` is §10's, both over one node type, so a `vector` converts to a `flex_vector` in constant time.
- **Bagwell, _Ideal Hash Trees_** (§1) — the trie this descends from. The vector drops the hash and the bitmap: an index is already uniform, and a dense node is indexed directly.

A vector node is dense and its depth is a function of the size alone, where a map node is packed to the slots it uses and reached through a bitmap. So the branching factor is a plain trade: a wider node is a shallower trie, one fewer dependent load per read, and more bytes to copy on a write and more children to retain on a cut. `VECTOR_BITS` measures both halves.

## 10. The relaxed vector

The third structure implemented here, §9's with one rule relaxed: a node may be short. A short node carries a table of running subtree sizes, so an index is a short search per level, and concatenation, a cut from either end, and insertion and erasure in the middle are all logarithmic.

- **[RRB-Trees](https://infoscience.epfl.ch/record/169879)** (Bagwell & Rompf 2011) → **[RRB Vector](https://dl.acm.org/doi/10.1145/2858949.2784739)** (Stucki et al., ICFP 2015) → **immer ICFP 2017** (§1) → Scala's [Vector rewrite](https://github.com/scala/scala/pull/8534). The whole difficulty is the rebalancing rule on concatenation, how much slack a merged spine may keep before its children are redistributed, which Stucki et al. state precisely and Bagwell & Rompf loosely. The slack `e` is the nodes a joined run may take beyond the minimum: 2 here, as in immer and in Bagwell & Rompf's measurements.
- **[c-rrb](https://github.com/hypirion/c-rrb)**, L'orange's C implementation, and the [thesis](https://hypirion.com/thesis.pdf) behind it. The clearest statement of the concatenation in code: `create_concat_plan` and `execute_concat_plan`, which `Rebalance` in `Vector.cpp` follows. The bound: the join of two subtries comes back as a node with at most two children, so a merged run is at most twice the branching factor.
- **Differences here.** The size table is a node like any other, cut from the same slab and refcounted, so a write that moves no element shares the table with its copy. That is 22 points on `set move` at a thousand elements. A node whose children are full for their level has no table, so the shape is a function of the contents, a strict `Vector` converts to a `FlexVector` for free, and a vector nothing has been joined into indexes as fast as a strict one.
- **A cut copies its tables from the parent**, and this is the whole of the `take` and `drop` margins: recomputing them from each kept child ran `take` 12% behind immer at a million elements. Everything before a cut from the right stays in place, so the copy takes the parent's running totals and computes only the last entry, and a cut from the left takes them shifted down by the count dropped. Both then read those totals to decide whether the copy is strict and drops the table. That is parity on `take` and 10 points ahead on `drop`.
- **One tail, at the back.** Scala's lesson is that fixed fingers at both ends beat a single tail. `PushFront` here is a concatenation with a vector of one, as immer's is: 20% ahead of immer at a million elements and two orders of magnitude behind `PushBack`.

## 11. Vector diffing

The map's diff walks two tries in step and skips every shared node by pointer, which works because a key's path depends only on the key. A vector's path depends on the index, and an insertion shifts every index after it, so the same walk reports everything past an insertion as changed. The goal is the map's O(|diff|) for two vectors related by `Set`, `PushBack`, and `Take`, and a sequence diff over only the unshared parts for two related by `Insert`, `Erase`, `Drop`, or `Concat`.

**The search.**
- **Myers, [_An O(ND) Difference Algorithm and Its Variations_](http://www.xmailserver.org/diff2.pdf) (1986).** Furthest-reaching paths per diagonal, and the linear-space form that grows paths from both ends and splits where they meet. The snake extension is the only place the algorithm reads its input, so it is the only place structural sharing applies.
- **Ukkonen, _Algorithms for Approximate String Matching_ (1985).** The same diagonal transitions with a substitution as a unit step. `Middle` runs this, so an overwrite costs one edit.
- **Landau & Vishkin (1989).** O(n + k²) given an oracle for how far two suffixes agree. The trie is that oracle: a shared run is skipped by identity.
- **Backurs & Indyk (STOC 2015).** No algorithm beats quadratic time on two unrelated sequences, so every fast bound has to be in the number of edits or in the elements sharing skips.
- **immer's `rrbtree::equals`** (`immer/detail/rbts/operations.hpp`) is the closest existing code, and it compares by index, so it reports everything after an insertion. immer's `diff` is for map and set only ([#67](https://github.com/arximboldi/immer/issues/67), [#120](https://github.com/arximboldi/immer/issues/120)), and im-rs, Scala, Clojure, Immutable.js, bifurcan, and Paguro compare vectors elementwise.

**Anchoring.**
- **Heckel (CACM 1978)** and **patience diff ([Cohen 2010](https://bramcohen.livejournal.com/73318.html)).** Strip the common ends, anchor on the lines unique to both sides, take the longest increasing subsequence of the anchors, and diff the gaps. With the chunk pointer as the unique line this is `Anchored`.
- **Hunt & Szymanski (CACM 1977).** Hash, then verify. A pointer is a perfect hash for a shared chunk, and a content hash is verified by reading.
- **git's `xdiff`** stops the search at a cost limit and splits at the furthest diagonal. The limit is kept and the split is dropped, since a replaced stretch is reported as one run.

**Content.**
- **rsync (Tridgell & Mackerras 1996).** Fixed blocks hashed on one side and a window rolled over the other recovers alignment after a shift. Pointer identity has no rolling form, so this finds a block whose chunks were re-cut, or one moved between vectors built apart.
- **Prolly trees (Noms, Dolt)** and **content-defined chunking (LBFS, FastCDC, Nakamura et al. 2020, Berger's Chonkers 2025).** A shape that depends on the content makes an insertion change O(log n) nodes and a diff cost the changed nodes. That is a different vector, since it gives up the radix index.

**Structure.**
- **hdiff (Miraldo & Swierstra, ICFP 2019)** and **truediff (PLDI 2021).** Hash every subtree, intersect, and a subtree in both is a shared reference wherever it sits. With pointer sharing the hash is free. **GumTree (ASE 2014)** aligns two different parent nodes with mostly the same children. A join of two vectors rebuilds the rightmost node of the left one and the leftmost node of the right one and keeps their children.
- **Kaneda, Arimura & Inenaga (2026).** A fingerprint per node of a path-copied tree gives equality of equal-content subtrees at different pointers. It is the one thing that would change the asymptotics here, and it costs a hash of every element on every write.
- **Compressed strings.** Two vectors sharing nodes are two start symbols over one straight-line program, and the bounds for that (Hermelin et al. 2009, Abboud et al. FOCS 2017) are in the size of the whole grammar. A trie has fixed fanout, known lengths, and equality by pointer, and a grammar has none of them. Tree edit distance is the wrong model.

**Persistent data in practice.**
- **editscript** diffs Clojure vectors with `identical?` then Wu et al. 1990 over the elements, under a one-second timeout. **JS immer's** `generateArrayPatches` shows the output of a positional diff under a shift: n replaces and one add.
- **Eg-walker (Gentle & Kleppmann, EuroSys 2025)** and the CRDTs: an identity per element, and a diff as classification. A chunk pointer is such an identity for its run.
- **Cai et al., [_A Theory of Changes_](https://arxiv.org/abs/1312.0658) (PLDI 2014).** `a ⊕ (b ⊖ a) = b`. `⊖` need only be valid, and a sequence's change type is a list of localized inserts, erases, and replaces. `Diff` is `⊖` and `Apply` is `⊕`. **DBSP (VLDB 2023)** shows why a set delta over (index, value) pairs is the wrong type: a middle insertion shifts every pair.
- **immer's [_Status of diff & patch_](https://github.com/arximboldi/immer/issues/58)** wanted a patch for undo. Persistence already covers that: two snapshots share every node they have in common, so a history is kept by keeping the snapshots. `Apply` is for the law above, and for applying a change set to a vector it did not come from.
