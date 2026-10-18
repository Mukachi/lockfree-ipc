cmake_minimum_required(VERSION 3.28)
project(lockfree-ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(lfipc INTERFACE)
target_include_directories(lfipc INTERFACE include)
