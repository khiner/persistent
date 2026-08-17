# The same suite twice, over the same source and expectations. Only the allocator underneath differs:
# HamtAuditTest links the build that returns nodes to the allocator and counts the live ones, which is
# what makes the reclamation check mean anything and what lets asan see the node traffic.
foreach(test HamtTest HamtAuditTest)
    add_executable(${test} HamtTest.cpp)
    target_link_libraries(${test} PRIVATE immer_reference Boost::ut)
endforeach()
target_link_libraries(HamtTest PRIVATE hamt)
target_link_libraries(HamtAuditTest PRIVATE hamt_audit)

foreach(test SetTest SetAuditTest)
    add_executable(${test} SetTest.cpp)
    target_link_libraries(${test} PRIVATE immer_reference Boost::ut)
endforeach()
target_link_libraries(SetTest PRIVATE hamt)
target_link_libraries(SetAuditTest PRIVATE hamt_audit)

foreach(test VectorTest VectorAuditTest)
    add_executable(${test} VectorTest.cpp)
    target_link_libraries(${test} PRIVATE immer_reference Boost::ut)
endforeach()
target_link_libraries(VectorTest PRIVATE vec)
target_link_libraries(VectorAuditTest PRIVATE vec_audit)

foreach(test FlexVectorTest FlexVectorAuditTest)
    add_executable(${test} FlexVectorTest.cpp)
    target_link_libraries(${test} PRIVATE immer_reference Boost::ut)
endforeach()
target_link_libraries(FlexVectorTest PRIVATE vec)
target_link_libraries(FlexVectorAuditTest PRIVATE vec_audit)

# One build and not two: the box has no free list for an audit build to drop -- see src/Box.h.
add_executable(BoxTest BoxTest.cpp)
target_link_libraries(BoxTest PRIVATE box immer_reference Boost::ut)
