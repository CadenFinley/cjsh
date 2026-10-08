# CJSHBuildSettings.cmake
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

include(CheckCXXSourceCompiles)

function(cjsh_collect_include_dirs)
    set(_options)
    set(_one_value_args ROOT_DIR OUT_VAR)
    set(_multi_value_args)
    cmake_parse_arguments(CJSH_DIRS "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if(CJSH_DIRS_ROOT_DIR STREQUAL "")
        message(FATAL_ERROR "cjsh_collect_include_dirs requires ROOT_DIR")
    endif()
    if(CJSH_DIRS_OUT_VAR STREQUAL "")
        message(FATAL_ERROR "cjsh_collect_include_dirs requires OUT_VAR")
    endif()

    file(
        GLOB_RECURSE _header_files
        CONFIGURE_DEPENDS
        "${CJSH_DIRS_ROOT_DIR}/*.h"
        "${CJSH_DIRS_ROOT_DIR}/*.hpp"
    )

    set(_include_dirs "${CJSH_DIRS_ROOT_DIR}")
    foreach(_header_file IN LISTS _header_files)
        get_filename_component(_header_dir "${_header_file}" DIRECTORY)
        list(APPEND _include_dirs "${_header_dir}")
    endforeach()
    list(REMOVE_DUPLICATES _include_dirs)

    set(${CJSH_DIRS_OUT_VAR} "${_include_dirs}" PARENT_SCOPE)
endfunction()

function(cjsh_link_libatomic_if_needed target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "cjsh_link_libatomic_if_needed expected an existing target")
    endif()

    set(
        _cjsh_atomic_test_source
        "#include <atomic>

int main() {
    std::atomic<unsigned long long> counter{0};
    return static_cast<int>(counter.fetch_add(1, std::memory_order_relaxed));
}
"
    )

    check_cxx_source_compiles(
        "${_cjsh_atomic_test_source}"
        CJSH_HAVE_64BIT_ATOMICS_WITHOUT_LIBATOMIC
    )
    if(CJSH_HAVE_64BIT_ATOMICS_WITHOUT_LIBATOMIC)
        return()
    endif()

    set(_cjsh_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_LIBRARIES atomic)
    check_cxx_source_compiles(
        "${_cjsh_atomic_test_source}"
        CJSH_HAVE_64BIT_ATOMICS_WITH_LIBATOMIC
    )
    set(CMAKE_REQUIRED_LIBRARIES "${_cjsh_saved_required_libraries}")

    if(CJSH_HAVE_64BIT_ATOMICS_WITH_LIBATOMIC)
        target_link_libraries(${target_name} PUBLIC atomic)
        return()
    endif()

    message(
        FATAL_ERROR
            "The selected C++ toolchain cannot link required 64-bit atomic operations. "
            "Install libatomic or use a toolchain with built-in support."
    )
endfunction()

function(cjsh_apply_build_profile)
    set(_options)
    set(_one_value_args TARGET MINIMAL_BUILD)
    set(_multi_value_args)
    cmake_parse_arguments(CJSH_PROFILE "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if(NOT TARGET ${CJSH_PROFILE_TARGET})
        message(FATAL_ERROR "cjsh_apply_build_profile expected an existing TARGET")
    endif()

    if(CJSH_ENABLE_UBSAN)
        if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            message(FATAL_ERROR "CJSH_ENABLE_UBSAN requires a Clang or GNU toolchain")
        endif()
        target_compile_options(${CJSH_PROFILE_TARGET} INTERFACE
            -fsanitize=undefined -fno-sanitize-recover=undefined)
        target_link_options(${CJSH_PROFILE_TARGET} INTERFACE
            -fsanitize=undefined -fno-sanitize-recover=undefined)
    endif()

    target_compile_definitions(
        ${CJSH_PROFILE_TARGET}
        INTERFACE
            "$<$<CONFIG:Debug>:CJSH_ENABLE_DEBUG=1>"
            "$<$<NOT:$<CONFIG:Debug>>:IC_NO_DEBUG_MSG=1>"
    )

    if(CJSH_PROFILE_MINIMAL_BUILD)
        target_compile_definitions(
            ${CJSH_PROFILE_TARGET}
            INTERFACE
                IC_NO_DEBUG_MSG=1
        )
    endif()

    if(MSVC)
        target_compile_options(
            ${CJSH_PROFILE_TARGET}
            INTERFACE
                "$<$<CONFIG:Debug>:/Od>"
                "$<$<CONFIG:Debug>:/Zi>"
                "$<$<CONFIG:Release>:/O2>"
                "$<$<CONFIG:RelWithDebInfo>:/O2>"
                "$<$<CONFIG:RelWithDebInfo>:/Zi>"
                "$<$<CONFIG:MinSizeRel>:/O1>"
        )
    else()
        target_compile_options(
            ${CJSH_PROFILE_TARGET}
            INTERFACE
                "$<$<CONFIG:Debug>:-O0>"
                "$<$<CONFIG:Debug>:-g>"
                "$<$<CONFIG:Debug>:-fsanitize=address>"
                "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        )
        target_link_options(
            ${CJSH_PROFILE_TARGET}
            INTERFACE
                "$<$<CONFIG:Debug>:-fsanitize=address>"
        )

        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_definitions(
                ${CJSH_PROFILE_TARGET}
                INTERFACE
                    "$<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>"
            )

            set(
                _cjsh_non_debug_c_opts
                -ffunction-sections
                -fdata-sections
                -fomit-frame-pointer
                -fmerge-all-constants
                -fvisibility=hidden
            )
            foreach(_cjsh_opt IN LISTS _cjsh_non_debug_c_opts)
                target_compile_options(
                    ${CJSH_PROFILE_TARGET}
                    INTERFACE
                        "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<COMPILE_LANGUAGE:C>>:${_cjsh_opt}>"
                )
            endforeach()

            set(
                _cjsh_non_debug_cxx_opts
                -ffunction-sections
                -fdata-sections
                -fomit-frame-pointer
                -fmerge-all-constants
                -fvisibility=hidden
                -fvisibility-inlines-hidden
            )
            # Keep RTTI enabled: libc++ exceptions thrown across the library
            # boundary may not match typed catch handlers with -fno-rtti.
            foreach(_cjsh_opt IN LISTS _cjsh_non_debug_cxx_opts)
                target_compile_options(
                    ${CJSH_PROFILE_TARGET}
                    INTERFACE
                        "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<COMPILE_LANGUAGE:CXX>>:${_cjsh_opt}>"
                )
            endforeach()

            if(APPLE)
                target_link_options(
                    ${CJSH_PROFILE_TARGET}
                    INTERFACE
                        "$<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip>"
                        "$<$<NOT:$<CONFIG:Debug>>:-Wl,-dead_strip_dylibs>"
                )
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_link_options(
                    ${CJSH_PROFILE_TARGET}
                    INTERFACE
                        "$<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>"
                        "$<$<NOT:$<CONFIG:Debug>>:-Wl,--as-needed>"
                )
            endif()
        endif()

        if(CJSH_PROFILE_MINIMAL_BUILD)
            target_compile_options(
                ${CJSH_PROFILE_TARGET}
                INTERFACE
                    "$<$<NOT:$<CONFIG:Debug>>:-Oz>"
            )

            if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
                set(
                    _cjsh_minimal_c_opts
                    -fno-unwind-tables
                    -fno-asynchronous-unwind-tables
                )
                foreach(_cjsh_opt IN LISTS _cjsh_minimal_c_opts)
                    target_compile_options(
                        ${CJSH_PROFILE_TARGET}
                        INTERFACE
                            "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<COMPILE_LANGUAGE:C>>:${_cjsh_opt}>"
                    )
                endforeach()

                set(
                    _cjsh_minimal_cxx_opts
                    -fno-unwind-tables
                    -fno-asynchronous-unwind-tables
                    -ftemplate-depth=64
                    -fno-threadsafe-statics
                )
                foreach(_cjsh_opt IN LISTS _cjsh_minimal_cxx_opts)
                    target_compile_options(
                        ${CJSH_PROFILE_TARGET}
                        INTERFACE
                            "$<$<AND:$<NOT:$<CONFIG:Debug>>,$<COMPILE_LANGUAGE:CXX>>:${_cjsh_opt}>"
                    )
                endforeach()

                if(APPLE)
                    target_link_options(
                        ${CJSH_PROFILE_TARGET}
                        INTERFACE
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,-no_compact_unwind>"
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,-no_function_starts>"
                    )
                elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                    target_link_options(
                        ${CJSH_PROFILE_TARGET}
                        INTERFACE
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,--strip-all>"
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,--discard-all>"
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,--no-undefined>"
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,--compress-debug-sections=zlib>"
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,-O2>"
                            "$<$<NOT:$<CONFIG:Debug>>:-Wl,--hash-style=gnu>"
                    )
                endif()
            endif()
        else()
            target_compile_options(
                ${CJSH_PROFILE_TARGET}
                INTERFACE
                    "$<$<CONFIG:Release>:-O3>"
                    "$<$<CONFIG:RelWithDebInfo>:-O3>"
                    "$<$<CONFIG:MinSizeRel>:-Oz>"
            )
        endif()
    endif()
endfunction()

function(cjsh_enable_binary_stripping target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "cjsh_enable_binary_stripping expected an existing target")
    endif()

    if(MSVC)
        return()
    endif()

    find_program(_cjsh_strip_tool strip)
    if(NOT _cjsh_strip_tool)
        message(WARNING "strip tool not found; CJSH_STRIP_BINARY option will be ignored")
        return()
    endif()

    if(APPLE)
        set(_cjsh_strip_flags -x)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_cjsh_strip_flags --strip-unneeded)
    else()
        set(_cjsh_strip_flags)
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(_cjsh_strip_configs)
        foreach(_cjsh_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
            if(NOT _cjsh_cfg STREQUAL "Debug")
                list(APPEND _cjsh_strip_configs ${_cjsh_cfg})
            endif()
        endforeach()
        if(_cjsh_strip_configs)
            add_custom_command(
                TARGET ${target_name}
                POST_BUILD
                COMMAND ${_cjsh_strip_tool} ${_cjsh_strip_flags} $<TARGET_FILE:${target_name}>
                COMMAND_EXPAND_LISTS
                COMMENT "Stripping symbols from ${target_name}"
                VERBATIM
                CONFIGURATIONS ${_cjsh_strip_configs}
            )
        endif()
    elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET ${target_name}
            POST_BUILD
            COMMAND ${_cjsh_strip_tool} ${_cjsh_strip_flags} $<TARGET_FILE:${target_name}>
            COMMAND_EXPAND_LISTS
            COMMENT "Stripping symbols from ${target_name}"
            VERBATIM
        )
    endif()
endfunction()
