cmake_minimum_required(VERSION 3.20)

project(persistent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit build/compile_commands.json for clangd.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -stdlib=libc++")

add_subdirectory(lib)

add_library(hamt src/Hamt.cpp)
target_include_directories(hamt PUBLIC src)
target_compile_options(hamt PRIVATE -Wall -Wextra)

add_subdirectory(tests)
