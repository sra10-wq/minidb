cmake_minimum_required(VERSION 3.20)
project(minidb CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# core engine = every .cpp in src/ except main.cpp
file(GLOB CORE_SOURCES src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(minidb_core ${CORE_SOURCES})
target_include_directories(minidb_core PUBLIC src)

add_executable(minidb src/main.cpp)
target_link_libraries(minidb minidb_core)

# one test executable per file in tests/
file(GLOB TEST_SOURCES tests/*.cpp)
foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} minidb_core)
endforeach()

# one benchmark executable per file in bench/
file(GLOB BENCH_SOURCES bench/*.cpp)
foreach(bench_src ${BENCH_SOURCES})
  get_filename_component(bench_name ${bench_src} NAME_WE)
  add_executable(${bench_name} ${bench_src})
  target_link_libraries(${bench_name} minidb_core)
endforeach()
