# The same suite twice, over the same source and the same expectations. Only the allocator underneath
# differs: HamtAuditTest links the build that returns nodes to the allocator and counts the live ones,
# which is what makes the reclamation check mean anything and what lets asan see the node traffic.
add_executable(HamtTest HamtTest.cpp)
add_executable(HamtAuditTest HamtTest.cpp)
foreach(test HamtTest HamtAuditTest)
    # Plain -I rather than SYSTEM: Apple clang searches /usr/local/include ahead of -isystem dirs,
    # which would shadow the submodule with any immer installed there.
    target_include_directories(${test} PRIVATE ${CMAKE_SOURCE_DIR}/immer)
    target_compile_options(${test} PRIVATE -Wall -Wextra)
endforeach()
target_link_libraries(HamtTest PRIVATE hamt Boost::ut)
target_link_libraries(HamtAuditTest PRIVATE hamt_audit Boost::ut)
