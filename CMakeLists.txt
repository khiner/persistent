cmake_minimum_required(VERSION 3.20)

project(persistent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit build/compile_commands.json for clangd.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -stdlib=libc++")
# The map is a compiled library and immer is header-only. Iteration and lookup are short enough that
# a call across the translation unit boundary costs more than anything in the node layout does, so
# without this the benchmark measures the packaging rather than the design: it is worth 3x on
# iteration and closes most of the lookup gap.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_subdirectory(lib)

set(HAMT_BITS 5 CACHE STRING "Trie branching factor exponent: 5 for 32 slots per node, 6 for 64.")

add_library(hamt src/Hamt.cpp)
target_include_directories(hamt PUBLIC src)
target_compile_definitions(hamt PRIVATE HAMT_BITS=${HAMT_BITS})
target_compile_options(hamt PRIVATE -Wall -Wextra)

add_subdirectory(tests)
add_subdirectory(bench)
