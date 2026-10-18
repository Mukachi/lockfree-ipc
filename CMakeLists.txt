cmake_minimum_required(VERSION 3.28)
project(lockfree-ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lfipc INTERFACE)
target_include_directories(lfipc INTERFACE include)
target_compile_options(lfipc INTERFACE -Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)

enable_testing()

add_executable(clock_test tests/clock_test.cpp)
target_link_libraries(clock_test PRIVATE lfipc)
add_test(NAME clock_test COMMAND clock_test)

add_executable(calibrate_test tests/calibrate_test.cpp)
target_link_libraries(calibrate_test PRIVATE lfipc)
add_test(NAME calibrate_test COMMAND calibrate_test)

add_executable(overhead_test tests/overhead_test.cpp)
target_link_libraries(overhead_test PRIVATE lfipc)
add_test(NAME overhead_test COMMAND overhead_test)

add_executable(cross_cpu_test tests/cross_cpu_test.cpp)
target_link_libraries(cross_cpu_test PRIVATE lfipc Threads::Threads)
add_test(NAME cross_cpu_test COMMAND cross_cpu_test)
