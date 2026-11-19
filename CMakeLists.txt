cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
# iOS is arm64 only. With two architectures CMake caches two sysroots (device + simulator) when project() runs, and the iOS build of
# the Geode SDK then sets ONE architecture: "CMAKE_APPLE_ARCH_SYSROOTS is not the same length as CMAKE_OSX_ARCHITECTURES".
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(GDFWR VERSION 0.2.1)

# core/ is plain C++ (no Geode); game/ and ui/ talk to the game.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_definitions(${PROJECT_NAME} PRIVATE GDFWR_VERSION="${PROJECT_VERSION}")

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

# Fix missing FMOD declarations in iOS generated bindings
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if (TARGET GeodeBindings)
        target_compile_options(GeodeBindings PRIVATE
            -include fmod.hpp
        )
    endif()
endif()
