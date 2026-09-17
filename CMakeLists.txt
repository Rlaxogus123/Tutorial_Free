cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(ProcessDifficulty VERSION 1.2.2)

# Visual Studio 18 currently crashes while generating code for Geode's large
# precompiled header. Compiling the same headers normally is stable.
if (MSVC)
    set(GEODE_DISABLE_PRECOMPILED_HEADERS ON CACHE BOOL "" FORCE)
endif()

# Add all source files inside src (recursively)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Set up the mod binary
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# MSVC can crash in CloseTypeServerPDB while compiling Geode's large headers
# with program-database debug information. Keep debug records in each object
# file and serialize any remaining PDB writes instead.
if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /Z7 /FS)
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Set up dependencies, resources, and link Geode.
setup_geode_mod(${PROJECT_NAME})
