# Standalone build of the Geode-free core tests:  cmake -S tests -B build-tests && cmake --build build-tests && ./build-tests/gdfwr_tests
cmake_minimum_required(VERSION 3.21)
project(GDFWRCoreTests CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GLOB CORE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/*.cpp)
add_executable(gdfwr_tests test_core.cpp ${CORE})
target_include_directories(gdfwr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
