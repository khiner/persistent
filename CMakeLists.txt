cmake_minimum_required(VERSION 3.20)

project(persistent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit build/compile_commands.json for clangd.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -stdlib=libc++ -Wall -Wextra")
# The map is a compiled library and immer is header-only. Iteration and lookup are short enough that a
# call across the translation unit boundary costs more than the node layout does, so without this the
# benchmark measures the packaging rather than the design: 3x on iteration, and most of the lookup gap.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_subdirectory(lib/ut) # boost::ut, the test framework.

# 5 by argument, not by inheritance from immer. Occupancy at the last level is 2^(W mod B) for a key set
# varying over W hash bits, so we match immer for every W exactly when our window boundaries are a subset
# of theirs. A window cannot exceed 6 bits, a slot map having to fit a word, and the only such subset of
# the multiples of 5 is all of them -- so 5 is the only choice that cannot come out behind on shape. 6
# has the better average and the worse tail, and the knob stays for a lookup-dominated workload.
set(HAMT_BITS 5 CACHE STRING "Trie branching factor exponent: 5 for 32 slots per node, 6 for 64.")

# The same source twice. `hamt_audit` drops the node free list, so freed nodes go back to the allocator
# and the live ones are counted. Nothing but the tests links it: it is the only build in which a leak, a
# double release, or a use after free is visible at all -- see src/Hamt.cpp.
foreach(lib hamt hamt_audit)
    add_library(${lib} src/Hamt.cpp)
    target_include_directories(${lib} PUBLIC src)
    target_compile_definitions(${lib} PRIVATE HAMT_BITS=${HAMT_BITS})
endforeach()
target_compile_definitions(hamt_audit PRIVATE HAMT_AUDIT)

# The vector's branching factor. A wider node makes the trie shallower, and a level is a dependent load:
# at a hundred thousand elements 6 reaches a chunk in three loads where 5 takes four, worth 20% on a
# random index and as much again on a write, over nodes twice as wide to copy.
#
# Two rows go the other way. A cut claims every child it keeps, and a wider node has twice as many per
# level for one level fewer, so `take` runs about 1.5x slower. A push that has to leave its vector alone
# copies the tail, which is twice the bytes. Set this to 5 for a workload made of those two.
set(VECTOR_BITS 6 CACHE STRING "Trie branching factor exponent: 5 for 32 slots per node, 6 for 64.")

# The same source twice, for the same reason as the map's: `vec_audit` drops the node free list.
foreach(lib vec vec_audit)
    add_library(${lib} src/Vector.cpp)
    target_include_directories(${lib} PUBLIC src)
    target_compile_definitions(${lib} PRIVATE VECTOR_BITS=${VECTOR_BITS})
endforeach()
target_compile_definitions(vec_audit PRIVATE VECTOR_AUDIT)

# The box is header-only, so this target carries the include path and nothing else.
add_library(box INTERFACE)
target_include_directories(box INTERFACE src)

# immer, the reference implementation, for the tests and the benchmark to measure against. Plain -I
# rather than SYSTEM: Apple clang searches /usr/local/include ahead of -isystem dirs, so any immer
# installed there would shadow the submodule. Its warnings are silenced at the include site instead.
add_library(immer_reference INTERFACE)
target_include_directories(immer_reference INTERFACE immer)

add_subdirectory(tests)
add_subdirectory(bench)
