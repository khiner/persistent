add_executable(HamtTest HamtTest.cpp)
# Plain -I rather than SYSTEM: Apple clang searches /usr/local/include ahead of -isystem dirs,
# which would shadow the submodule with any immer installed there.
target_include_directories(HamtTest PRIVATE ${CMAKE_SOURCE_DIR}/immer)
target_link_libraries(HamtTest PRIVATE hamt Boost::ut)
target_compile_options(HamtTest PRIVATE -Wall -Wextra)
