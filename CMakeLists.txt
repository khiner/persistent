cmake_minimum_required(VERSION 3.16)
project(persistent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(persistent INTERFACE)
target_include_directories(persistent INTERFACE include)

option(PERSISTENT_BUILD_TESTS "Build tests" ON)

if(PERSISTENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
