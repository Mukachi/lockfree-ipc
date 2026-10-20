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

# S6 and S8 are checked in the compiled code, so x86-64 only, and not in a
# sanitizer build, whose instrumentation adds calls to every hot path.
add_executable(hot_path_subject tests/hot_path_subject.cpp)
target_link_libraries(hot_path_subject PRIVATE lfipc Threads::Threads)
if(Python3_FOUND AND CMAKE_OBJDUMP AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"
   AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
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
  # MPMC baselines (note 010). Header-only; SOURCE_SUBDIR names no directory,
  # so their own CMake projects (tests, install rules) are not added.
  set(LFIPC_CQ_VERSION "v1.0.5 (9afb997)")
  set(LFIPC_RIGTORP_VERSION "v1.0 (11ffb3c)")
  FetchContent_Declare(concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG 9afb99746f0f5fc94ac8aef737053ae0481ba8d1  # v1.0.5, released 2026-04-03
    GIT_SHALLOW FALSE
    SOURCE_SUBDIR none)
  FetchContent_Declare(rigtorp_mpmc
    GIT_REPOSITORY https://github.com/rigtorp/MPMCQueue.git
    GIT_TAG 11ffb3cd77998cba83c20d2853720be1625cd719  # v1.0, released 2020-04-17
    GIT_SHALLOW FALSE
    SOURCE_SUBDIR none)
  FetchContent_MakeAvailable(readerwriterqueue boost concurrentqueue rigtorp_mpmc)
endif()

if(LFIPC_BASELINES)
  # Third-party headers are system headers here, so our warnings-as-errors
  # apply to our code only.
  add_library(lfipc_baselines INTERFACE)
  get_target_property(rwq_inc readerwriterqueue INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(lfipc_baselines SYSTEM INTERFACE ${rwq_inc} ${concurrentqueue_SOURCE_DIR}
                             ${rigtorp_mpmc_SOURCE_DIR}/include)
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

# Note 007: one number everywhere. A stale RESULTS.md or README figure fails
# the test suite like any other bug.
if(Python3_FOUND)
  add_test(NAME run_py_test COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tests/run_py_test.py ${CMAKE_SOURCE_DIR})
  add_test(NAME report_check COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/run.py report --check)
endif()

add_executable(shm_test tests/shm_test.cpp)
target_link_libraries(shm_test PRIVATE lfipc Threads::Threads)
add_test(NAME shm_test COMMAND shm_test)

add_executable(shm_stress_test tests/shm_stress_test.cpp)
target_link_libraries(shm_stress_test PRIVATE lfipc Threads::Threads)
add_test(NAME shm_stress_test COMMAND shm_stress_test)

add_executable(load_xproc_test tests/load_xproc_test.cpp)
target_link_libraries(load_xproc_test PRIVATE lfipc Threads::Threads)
add_test(NAME load_xproc_test COMMAND load_xproc_test)

if(LFIPC_BASELINES)
  add_executable(shm_bench bench/shm_bench.cpp)
  target_link_libraries(shm_bench PRIVATE lfipc lfipc_baselines Threads::Threads)
  target_compile_definitions(shm_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")
endif()

add_executable(placement_test tests/placement_test.cpp)
target_link_libraries(placement_test PRIVATE lfipc Threads::Threads)
add_test(NAME placement_test COMMAND placement_test)

add_executable(mpmc_test tests/mpmc_test.cpp)
target_link_libraries(mpmc_test PRIVATE lfipc Threads::Threads)
add_test(NAME mpmc_test COMMAND mpmc_test)

if(LFIPC_BASELINES)
  add_executable(mpmc_queues_test tests/mpmc_queues_test.cpp)
  target_link_libraries(mpmc_queues_test PRIVATE lfipc lfipc_baselines Threads::Threads)
  add_test(NAME mpmc_queues_test COMMAND mpmc_queues_test)

  add_executable(mpmc_bench bench/mpmc_bench.cpp)
  target_link_libraries(mpmc_bench PRIVATE lfipc lfipc_baselines Threads::Threads)
  target_compile_definitions(mpmc_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results"
    LFIPC_CQ_VERSION="${LFIPC_CQ_VERSION}" LFIPC_BOOST_VERSION="${LFIPC_BOOST_VERSION}"
    LFIPC_RIGTORP_VERSION="${LFIPC_RIGTORP_VERSION}")
endif()

add_executable(siblings_test tests/siblings_test.cpp)
target_link_libraries(siblings_test PRIVATE lfipc Threads::Threads)
add_test(NAME siblings_test COMMAND siblings_test)
set_tests_properties(siblings_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(siblings_bench bench/siblings_bench.cpp)
target_link_libraries(siblings_bench PRIVATE lfipc Threads::Threads)
target_compile_definitions(siblings_bench PRIVATE LFIPC_RESULTS_DIR="${CMAKE_SOURCE_DIR}/results")

add_executable(load_mp_test tests/load_mp_test.cpp)
target_link_libraries(load_mp_test PRIVATE lfipc Threads::Threads)
add_test(NAME load_mp_test COMMAND load_mp_test)

# Note 009: running anywhere. Checks that need a quiet, dedicated machine run
# from the same executables started with "quiet", under the label `quiet`; CI
# runs `ctest -LE quiet`. Tests that pin CPUs never overlap under `ctest -j`.
foreach(t noise_test clock_block_test cross_cpu_test)
  add_test(NAME ${t}_quiet COMMAND ${t} quiet)
  set_tests_properties(${t}_quiet PROPERTIES LABELS quiet)
endforeach()
set_tests_properties(clock_block_test clock_block_test_quiet cross_cpu_test cross_cpu_test_quiet overhead_test
                     record_cost_test load_test load_xproc_test noise_test noise_test_quiet noise_block_test
                     placement_test load_mp_test siblings_test
                     PROPERTIES RESOURCE_LOCK cpu)
set_tests_properties(load_test load_xproc_test PROPERTIES SKIP_RETURN_CODE 77)
