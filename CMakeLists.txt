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

add_executable(json_test tests/json_test.cpp)
target_link_libraries(json_test PRIVATE lfipc)
add_test(NAME json_test COMMAND json_test)

add_executable(clock_block_test tests/clock_block_test.cpp)
target_link_libraries(clock_block_test PRIVATE lfipc Threads::Threads)
add_test(NAME clock_block_test COMMAND clock_block_test)
set_tests_properties(clock_block_test PROPERTIES FIXTURES_SETUP clock_json)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME check_clock_json
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_clock_json.py
                   clock_ok.json clock_refused.json)
  set_tests_properties(check_clock_json PROPERTIES FIXTURES_REQUIRED clock_json)
endif()

# S6 and S8 are checked in the compiled code, so x86-64 only.
add_executable(hot_path_subject tests/hot_path_subject.cpp)
target_link_libraries(hot_path_subject PRIVATE lfipc Threads::Threads)
if(Python3_FOUND AND CMAKE_OBJDUMP AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_test(NAME check_hot_path
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_hot_path.py
                   ${CMAKE_OBJDUMP} $<TARGET_FILE:hot_path_subject>)
endif()

add_executable(clock_bench bench/clock_bench.cpp)
target_link_libraries(clock_bench PRIVATE lfipc Threads::Threads)
target_compile_definitions(clock_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

add_executable(histogram_test tests/histogram_test.cpp)
target_link_libraries(histogram_test PRIVATE lfipc)
add_test(NAME histogram_test COMMAND histogram_test)

add_executable(histogram_block_test tests/histogram_block_test.cpp)
target_link_libraries(histogram_block_test PRIVATE lfipc)
add_test(NAME histogram_block_test COMMAND histogram_block_test)
set_tests_properties(histogram_block_test PROPERTIES FIXTURES_SETUP histogram_json)
if(Python3_FOUND)
  add_test(NAME check_histogram_json
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_histogram_json.py
                   histogram_full.json histogram_empty.json clock_ok.json clock_refused.json load_block.json)
  set_tests_properties(check_histogram_json PROPERTIES FIXTURES_REQUIRED "histogram_json;clock_json;load_json")
endif()

add_executable(record_cost_test tests/record_cost_test.cpp)
target_link_libraries(record_cost_test PRIVATE lfipc Threads::Threads)
add_test(NAME record_cost_test COMMAND record_cost_test)

add_executable(histogram_bench bench/histogram_bench.cpp)
target_link_libraries(histogram_bench PRIVATE lfipc Threads::Threads)
target_compile_definitions(histogram_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

add_executable(load_test tests/load_test.cpp)
target_link_libraries(load_test PRIVATE lfipc Threads::Threads)
add_test(NAME load_test COMMAND load_test)

add_executable(load_block_test tests/load_block_test.cpp)
target_link_libraries(load_block_test PRIVATE lfipc Threads::Threads)
add_test(NAME load_block_test COMMAND load_block_test)
set_tests_properties(load_block_test PROPERTIES FIXTURES_SETUP load_json)
if(Python3_FOUND)
  add_test(NAME check_load_json
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_load_json.py load_block.json)
  set_tests_properties(check_load_json PROPERTIES FIXTURES_REQUIRED load_json)
endif()

add_executable(load_bench bench/load_bench.cpp)
target_link_libraries(load_bench PRIVATE lfipc Threads::Threads)
target_compile_definitions(load_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")
