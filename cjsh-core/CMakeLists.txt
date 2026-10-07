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

file(
    GLOB_RECURSE CJSH_ALL_CPP
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(
    GLOB_RECURSE CJSH_ALL_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

set(CJSH_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/cjsh.cpp")
set(CJSH_CORE_SOURCES ${CJSH_ALL_CPP})
list(REMOVE_ITEM CJSH_CORE_SOURCES "${CJSH_MAIN_SOURCE}")

cjsh_collect_include_dirs(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" OUT_VAR CJSH_INCLUDE_DIRS)

add_library(cjsh_core STATIC)
target_sources(cjsh_core PRIVATE ${CJSH_CORE_SOURCES} ${CJSH_ALL_HEADERS})
target_include_directories(cjsh_core PUBLIC ${CJSH_INCLUDE_DIRS})
target_link_libraries(cjsh_core PUBLIC cjsh_build_settings cjsh_isocline)
cjsh_link_libatomic_if_needed(cjsh_core)
set_target_properties(
    cjsh_core
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(cjsh_core PUBLIC stdc++fs)
endif()

add_executable(cjsh "${CJSH_MAIN_SOURCE}")
target_link_libraries(cjsh PRIVATE cjsh_core)
target_include_directories(cjsh PRIVATE ${CJSH_INCLUDE_DIRS})
set_target_properties(cjsh PROPERTIES OUTPUT_NAME "cjsh")

if(CJSH_STATIC_LINK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "CJSH_STATIC_LINK is currently supported only on Linux")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR "CJSH_STATIC_LINK requires a GNU-compatible linker")
    endif()
    target_link_options(cjsh PRIVATE -static)
endif()

if(CJSH_STRIP_BINARY)
    cjsh_enable_binary_stripping(cjsh)
endif()

include(GNUInstallDirs)
install(TARGETS cjsh RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
    FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/cjsh"
    COMPONENT licenses
)
