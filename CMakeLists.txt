cmake_minimum_required(VERSION 3.20)

project(persistent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit build/compile_commands.json for clangd.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -stdlib=libc++ -Wall -Wextra")
# The map is a compiled library and immer is header-only. Iteration and lookup are short enough that
# a call across the translation unit boundary costs more than anything in the node layout does, so
# without this the benchmark measures the packaging rather than the design: it is worth 3x on
# iteration and closes most of the lookup gap.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_subdirectory(lib/ut) # boost::ut, the test framework.

# 5 by argument, not by inheritance from immer. Occupancy at the last level is 2^(W mod B), where W is
# how many hash bits the key set varies over, so a terminal node holds the bits between the last window
# boundary below W and W itself. If our window boundaries are a subset of immer's our occupancy is at
# least theirs for every W, a window cannot exceed 6 bits because a slot map has to fit a machine word,
# and the only subset of the multiples of 5 whose gaps are all at most 6 is the whole set. So 5 is the
# only choice that cannot come out behind immer on shape. 6 has the better average and the worse tail,
# and the knob stays for a lookup-dominated workload on keys that suit it.
set(HAMT_BITS 5 CACHE STRING "Trie branching factor exponent: 5 for 32 slots per node, 6 for 64.")

# The same source twice. `hamt_audit` drops the node free list, so freed nodes go back to the
# allocator and the live ones are counted. Nothing links it but the tests: it is the only build in
# which a leak, a double release, or a use after free is visible at all -- see src/Hamt.cpp.
foreach(lib hamt hamt_audit)
    add_library(${lib} src/Hamt.cpp)
    target_include_directories(${lib} PUBLIC src)
    target_compile_definitions(${lib} PRIVATE HAMT_BITS=${HAMT_BITS})
endforeach()
target_compile_definitions(hamt_audit PRIVATE HAMT_AUDIT)

# immer, the reference implementation, for the tests and the benchmark to measure against. Plain -I
# rather than SYSTEM: Apple clang searches /usr/local/include ahead of -isystem dirs, which would
# shadow the submodule with any immer installed there. Its own warnings are silenced at the include site.
add_library(immer_reference INTERFACE)
target_include_directories(immer_reference INTERFACE immer)

add_subdirectory(tests)
add_subdirectory(bench)
