cmake_minimum_required(VERSION 3.16)
project(lockfree_queue CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(lfq INTERFACE)
target_include_directories(lfq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(smoke_test src/main.cc)
target_link_libraries(smoke_test PRIVATE lfq Threads::Threads)
target_compile_options(smoke_test PRIVATE -Wall -Wextra -Wpedantic)
