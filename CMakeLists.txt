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

# Build provenance (note 004): regenerated at every build, rewritten only on change.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
add_custom_target(build_info ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          "-DFLAGS=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} -Wall -Wextra -Wpedantic -Werror"
          -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/lfipc/build_info.hpp
  COMMENT "Recording build provenance")
target_include_directories(lfipc INTERFACE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(lfipc build_info)

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
                   histogram_full.json histogram_empty.json clock_ok.json clock_refused.json load_block.json environment.json noise_block.json)
  set_tests_properties(check_histogram_json PROPERTIES FIXTURES_REQUIRED "histogram_json;clock_json;load_json;environment_json;noise_json")
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

add_executable(environment_test tests/environment_test.cpp)
target_link_libraries(environment_test PRIVATE lfipc)
add_test(NAME environment_test COMMAND environment_test)
set_tests_properties(environment_test PROPERTIES FIXTURES_SETUP environment_json)
if(Python3_FOUND)
  add_test(NAME check_environment_json
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_environment_json.py environment.json)
  set_tests_properties(check_environment_json PROPERTIES FIXTURES_REQUIRED environment_json)
endif()

add_executable(noise_test tests/noise_test.cpp)
target_link_libraries(noise_test PRIVATE lfipc Threads::Threads)
add_test(NAME noise_test COMMAND noise_test)

add_executable(noise_block_test tests/noise_block_test.cpp)
target_link_libraries(noise_block_test PRIVATE lfipc Threads::Threads)
add_test(NAME noise_block_test COMMAND noise_block_test)
set_tests_properties(noise_block_test PROPERTIES FIXTURES_SETUP noise_json)
if(Python3_FOUND)
  add_test(NAME check_noise_json
           COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/check_noise_json.py noise_block.json)
  set_tests_properties(check_noise_json PROPERTIES FIXTURES_REQUIRED noise_json)
endif()

add_executable(noise_bench bench/noise_bench.cpp)
target_link_libraries(noise_bench PRIVATE lfipc Threads::Threads)
target_compile_definitions(noise_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

add_executable(spsc_test tests/spsc_test.cpp)
target_link_libraries(spsc_test PRIVATE lfipc Threads::Threads)
add_test(NAME spsc_test COMMAND spsc_test)

# Baselines for note 006, fetched into build/_deps and pinned by commit and
# checksum. Third-party code stays out of the source tree.
option(LFIPC_BASELINES "Fetch and build the baseline queues" ON)
if(LFIPC_BASELINES)
  include(FetchContent)
  # Defined once: used to fetch, and recorded in every run's JSON.
  set(LFIPC_RWQ_VERSION "v1.0.7 (8b21766)")
  set(LFIPC_BOOST_VERSION "1.92.0")
  FetchContent_Declare(readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG 8b2176698e9bdaba653cdc20c32b54737a934b47  # v1.0.7, released 2025-04-28
    GIT_SHALLOW FALSE)
  set(BOOST_INCLUDE_LIBRARIES lockfree)
  set(BOOST_ENABLE_CMAKE ON)
  FetchContent_Declare(boost
    URL https://github.com/boostorg/boost/releases/download/boost-${LFIPC_BOOST_VERSION}/boost-${LFIPC_BOOST_VERSION}-cmake.tar.xz
    URL_HASH SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d  # released 2026-08-12
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(readerwriterqueue boost)
endif()

if(LFIPC_BASELINES)
  # Third-party headers are system headers here, so our warnings-as-errors
  # apply to our code only.
  add_library(lfipc_baselines INTERFACE)
  get_target_property(rwq_inc readerwriterqueue INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(lfipc_baselines SYSTEM INTERFACE ${rwq_inc})
  target_link_libraries(lfipc_baselines INTERFACE Boost::lockfree)
  set_target_properties(lfipc_baselines PROPERTIES SYSTEM ON)

  add_executable(queues_test tests/queues_test.cpp)
  target_link_libraries(queues_test PRIVATE lfipc lfipc_baselines Threads::Threads)
  add_test(NAME queues_test COMMAND queues_test)
endif()

if(LFIPC_BASELINES)
  add_executable(spsc_bench bench/spsc_bench.cpp)
  target_link_libraries(spsc_bench PRIVATE lfipc lfipc_baselines Threads::Threads)
  target_compile_definitions(spsc_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results"
    LFIPC_RWQ_VERSION="${LFIPC_RWQ_VERSION}" LFIPC_BOOST_VERSION="${LFIPC_BOOST_VERSION}")
endif()
