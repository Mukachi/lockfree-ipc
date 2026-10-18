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

enable_testing()

add_executable(clock_test tests/clock_test.cpp)
target_link_libraries(clock_test PRIVATE lfipc)
add_test(NAME clock_test COMMAND clock_test)
