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

# 6 by default because the sweep says so -- see the first open question in docs/Literature.md.
set(HAMT_BITS 6 CACHE STRING "Trie branching factor exponent: 5 for 32 slots per node, 6 for 64.")

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
