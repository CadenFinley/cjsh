# CMakeLists.txt
#
# This file is part of cjsh, CJ's Shell
#
# MIT License
#
# Copyright (c) 2026 Caden Finley
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

cmake_minimum_required(VERSION 3.25)

project(cjsh VERSION 1.5.8 LANGUAGES C CXX)
option(CJSH_PRE_RELEASE "Mark builds as pre-release versions" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
    foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${_cfg}" _cfg_upper)
        set("CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper}" "${CMAKE_BINARY_DIR}")
    endforeach()
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        Release
        CACHE STRING "Build type" FORCE
    )
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

set(_cjsh_strip_binary_default ON)
if(DEFINED ENV{CJSH_STRIP_BINARY})
    string(TOUPPER "$ENV{CJSH_STRIP_BINARY}" _cjsh_strip_binary_env)
    if(_cjsh_strip_binary_env STREQUAL "0"
       OR _cjsh_strip_binary_env STREQUAL "OFF"
       OR _cjsh_strip_binary_env STREQUAL "FALSE"
       OR _cjsh_strip_binary_env STREQUAL "NO")
        set(_cjsh_strip_binary_default OFF)
    endif()
endif()

option(CJSH_GENERATE_COMPILE_COMMANDS "Generate compile_commands.json" ON)
option(CJSH_BUILD_TESTS "Build cjsh test binaries" ON)
option(CJSH_MINIMAL_BUILD "Enable the minimal size-oriented build profile" OFF)
option(CJSH_STRIP_BINARY "Strip non-Debug binaries" ${_cjsh_strip_binary_default})
option(CJSH_STATIC_LINK "Link the cjsh executable statically (Linux only)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ${CJSH_GENERATE_COMPILE_COMMANDS})

include(CTest)
if(CJSH_BUILD_TESTS AND NOT BUILD_TESTING)
    set(BUILD_TESTING
        ON
        CACHE BOOL "Build tests" FORCE
    )
endif()
if(BUILD_TESTING)
    enable_testing()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CJSHBuildMetadata)
include(CJSHBuildSettings)

find_package(Threads REQUIRED)

cjsh_compute_build_metadata(
    OUT_GIT_HASH CJSH_METADATA_GIT_HASH
    OUT_GIT_HASH_FULL CJSH_METADATA_GIT_HASH_FULL
    OUT_BUILD_ARCH CJSH_METADATA_BUILD_ARCH
    OUT_BUILD_PLATFORM CJSH_METADATA_BUILD_PLATFORM
    OUT_BUILD_TIME CJSH_METADATA_BUILD_TIME
    OUT_BUILD_COMPILER CJSH_METADATA_BUILD_COMPILER
    OUT_CXX_STANDARD CJSH_METADATA_CXX_STANDARD
    OUT_BUILD_TYPE CJSH_METADATA_BUILD_TYPE
)

add_library(cjsh_build_metadata INTERFACE)
cjsh_apply_metadata_definitions(
    TARGET cjsh_build_metadata
    GIT_HASH "${CJSH_METADATA_GIT_HASH}"
    GIT_HASH_FULL "${CJSH_METADATA_GIT_HASH_FULL}"
    BUILD_ARCH "${CJSH_METADATA_BUILD_ARCH}"
    BUILD_PLATFORM "${CJSH_METADATA_BUILD_PLATFORM}"
    BUILD_TIME "${CJSH_METADATA_BUILD_TIME}"
    BUILD_COMPILER "${CJSH_METADATA_BUILD_COMPILER}"
    CXX_STANDARD "${CJSH_METADATA_CXX_STANDARD}"
    BUILD_TYPE "${CJSH_METADATA_BUILD_TYPE}"
    VERSION_BASE "${PROJECT_VERSION}"
    PRE_RELEASE "${CJSH_PRE_RELEASE}"
)

add_library(cjsh_build_profile INTERFACE)
cjsh_apply_build_profile(
    TARGET cjsh_build_profile
    MINIMAL_BUILD ${CJSH_MINIMAL_BUILD}
)

add_library(cjsh_build_settings INTERFACE)
target_link_libraries(cjsh_build_settings INTERFACE cjsh_build_metadata cjsh_build_profile)

add_subdirectory(cjsh-isocline "${CMAKE_BINARY_DIR}/modules/cjsh-isocline")
add_subdirectory(cjsh-core "${CMAKE_BINARY_DIR}/modules/cjsh-core")

if(CJSH_BUILD_TESTS)
    add_subdirectory(tests "${CMAKE_BINARY_DIR}/modules/tests")
endif()
