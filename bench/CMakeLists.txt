add_executable(HamtBench Bench.cpp)
target_link_libraries(HamtBench PRIVATE hamt immer_reference)
