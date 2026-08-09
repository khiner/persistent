include(FetchContent)

# -------------------------------------------------------------------------
# Catch2 – test framework
# -------------------------------------------------------------------------
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.3
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# -------------------------------------------------------------------------
# immer – reference implementation for byte-for-byte comparison
# -------------------------------------------------------------------------
set(immer_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(immer_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(immer_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(immer_BUILD_EXTRAS   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    immer
    GIT_REPOSITORY https://github.com/arximboldi/immer.git
    GIT_TAG        v0.8.1
)
FetchContent_MakeAvailable(immer)

# immer is header-only; create an interface target for it.
if(NOT TARGET immer)
    add_library(immer INTERFACE)
    target_include_directories(immer INTERFACE ${immer_SOURCE_DIR})
endif()

# -------------------------------------------------------------------------
# Test targets
# -------------------------------------------------------------------------
function(add_persistent_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE persistent Catch2::Catch2WithMain immer)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    catch_discover_tests(${name})
endfunction()

add_persistent_test(test_map)
add_persistent_test(test_set)
