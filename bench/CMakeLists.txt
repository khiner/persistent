add_executable(HamtBench Bench.cpp)
# Plain -I rather than SYSTEM, for the same reason as tests/CMakeLists.txt.
target_include_directories(HamtBench PRIVATE ${CMAKE_SOURCE_DIR}/immer)
target_link_libraries(HamtBench PRIVATE hamt)
target_compile_options(HamtBench PRIVATE -Wall -Wextra)
