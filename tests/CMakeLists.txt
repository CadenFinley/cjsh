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

if(NOT BUILD_TESTING)
    return()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

# Print the individual counts after CTest's suite-level summary, including when
# invoked directly or through a test preset.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/runner/CTestCustom.cmake.in"
    "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
    @ONLY
)

# Isolate both stdin and /dev/tty: job-control tests can reopen the controlling
# terminal even when stdin is redirected. Interactive tests create their own PTYs.
add_executable(cjsh_test_runner "${CMAKE_CURRENT_SOURCE_DIR}/runner/run_test.c")

function(cjsh_add_test)
    cmake_parse_arguments(PARSE_ARGV 0 test "" "NAME" "COMMAND")
    list(POP_FRONT test_COMMAND executable)
    if(TARGET "${executable}")
        set(executable "$<TARGET_FILE:${executable}>")
    endif()
    add_test(
        NAME "${test_NAME}"
        COMMAND cjsh_test_runner "${executable}" ${test_COMMAND}
    )
endfunction()

# Register shell files individually so CTest can schedule them with --parallel.
file(GLOB CJSH_SHELL_TEST_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell/test_*.sh")
foreach(test_script IN LISTS CJSH_SHELL_TEST_SCRIPTS)
    get_filename_component(test_name "${test_script}" NAME_WE)
    set(test_work_dir "${CMAKE_CURRENT_BINARY_DIR}/shell/${test_name}")
    file(MAKE_DIRECTORY "${test_work_dir}")
    cjsh_add_test(
        NAME "shell.${test_name}"
        COMMAND sh "${test_script}" $<TARGET_FILE:cjsh>
    )
    set_tests_properties(
        "shell.${test_name}"
        PROPERTIES
            LABELS "shell"
            WORKING_DIRECTORY "${test_work_dir}"
            ENVIRONMENT "CJSH=$<TARGET_FILE:cjsh>;CJSH_PATH=$<TARGET_FILE:cjsh>;SHELL_TO_TEST=$<TARGET_FILE:cjsh>"
    )

    # Timing checks need an idle runner. Process-count checks observe all system
    # processes, and test_performance also deletes a shared /tmp/cjsh_test_* glob.
    if(test_name MATCHES "performance"
       OR test_name MATCHES "^test_(sigchld_handling|zombie_processes|zombie_edge_cases)$")
        set_tests_properties("shell.${test_name}" PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

# These legacy suites both write and remove /tmp/exit_code_test.out.
set_tests_properties(
    shell.test_edge_cases shell.test_security_validation
    PROPERTIES RESOURCE_LOCK "shell_exit_code_test_output"
)

add_executable(isocline_behavior_tests "${CMAKE_CURRENT_SOURCE_DIR}/isocline/test_isocline_behavior.c")
target_link_libraries(isocline_behavior_tests PRIVATE cjsh_isocline)

add_executable(isocline_pty_driver "${CMAKE_CURRENT_SOURCE_DIR}/isocline/isocline_pty_driver.c")
target_link_libraries(isocline_pty_driver PRIVATE cjsh_isocline Threads::Threads)

if(UNIX)
    add_executable(history_concurrency_driver "${CMAKE_CURRENT_SOURCE_DIR}/isocline/history_concurrency_driver.c")
    target_link_libraries(history_concurrency_driver PRIVATE cjsh_isocline)
    add_library(startup_policy_injector SHARED "${CMAKE_CURRENT_SOURCE_DIR}/core/startup_policy_injector.c")
    set_target_properties(startup_policy_injector PROPERTIES PREFIX "")
    add_executable(lifecycle_probe "${CMAKE_CURRENT_SOURCE_DIR}/core/lifecycle_probe.c")
    add_executable(terminal_contract_driver "${CMAKE_CURRENT_SOURCE_DIR}/isocline/terminal_contract_driver.c")
    target_link_libraries(terminal_contract_driver PRIVATE cjsh_isocline)
    add_library(
        fg_terminal_race_injector
        SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/core/fg_terminal_race_injector.c"
    )
    set_target_properties(
        fg_terminal_race_injector
        PROPERTIES
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

add_executable(
    completion_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/completions/test_completions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/completions/test_completion_globals.cpp"
)
target_link_libraries(completion_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(
    syntax_highlighting_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/highlighter/test_syntax_highlighting.cpp"
)
target_link_libraries(syntax_highlighting_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(
    history_expansion_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/test_history_expansion.cpp"
)
target_link_libraries(history_expansion_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(agent_mode_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_agent_mode.cpp")
target_link_libraries(agent_mode_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(function_syntax_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_function_syntax.cpp")
target_link_libraries(function_syntax_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(loop_syntax_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_loop_syntax.cpp")
target_link_libraries(loop_syntax_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(system_paths_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_system_paths.cpp")
target_link_libraries(system_paths_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(command_lookup_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_command_lookup.cpp")
target_link_libraries(command_lookup_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(variable_lookup_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_variable_lookup.cpp")
target_link_libraries(variable_lookup_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(validation_token_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_validation_tokens.cpp")
target_link_libraries(validation_token_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(pattern_endpoint_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_pattern_endpoints.cpp")
target_link_libraries(pattern_endpoint_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(parser_dispatch_tests "${CMAKE_CURRENT_SOURCE_DIR}/core/test_parser_dispatch.cpp")
target_link_libraries(parser_dispatch_tests PRIVATE cjsh_core cjsh_isocline)

add_executable(syntax_validation_probe "${CMAKE_CURRENT_SOURCE_DIR}/core/syntax_validation_probe.cpp")
target_link_libraries(syntax_validation_probe PRIVATE cjsh_core cjsh_isocline)

cjsh_add_test(NAME isocline_behavior_tests COMMAND isocline_behavior_tests)
cjsh_add_test(NAME completion_tests COMMAND completion_tests)
cjsh_add_test(NAME syntax_highlighting_tests COMMAND syntax_highlighting_tests)
cjsh_add_test(NAME history_expansion_tests COMMAND history_expansion_tests)
cjsh_add_test(NAME agent_mode_tests COMMAND agent_mode_tests)
cjsh_add_test(NAME function_syntax_tests COMMAND function_syntax_tests)
cjsh_add_test(NAME loop_syntax_tests COMMAND loop_syntax_tests)
cjsh_add_test(NAME system_paths_tests COMMAND system_paths_tests)
cjsh_add_test(NAME command_lookup_tests COMMAND command_lookup_tests)
cjsh_add_test(NAME variable_lookup_tests COMMAND variable_lookup_tests)
cjsh_add_test(NAME validation_token_tests COMMAND validation_token_tests)
cjsh_add_test(NAME parser_dispatch_tests COMMAND parser_dispatch_tests)
cjsh_add_test(NAME pattern_endpoint_tests COMMAND pattern_endpoint_tests)

if(Python3_Interpreter_FOUND)
    cjsh_add_test(NAME syntax_regression_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_syntax_regressions.py"
                $<TARGET_FILE:cjsh> $<TARGET_FILE:syntax_validation_probe>)
    if(UNIX)
        cjsh_add_test(NAME directory_history_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_directory_history.py"
                    $<TARGET_FILE:cjsh>)
        cjsh_add_test(NAME history_concurrency_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/isocline/test_history_concurrency.py"
                    $<TARGET_FILE:history_concurrency_driver>)
        cjsh_add_test(NAME batch2_interactive_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_batch2_interactive.py"
                    $<TARGET_FILE:cjsh>)
        cjsh_add_test(NAME batch2_startup_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_batch2_startup.py"
                    $<TARGET_FILE:cjsh> $<TARGET_FILE:startup_policy_injector>)
        cjsh_add_test(NAME startup_interrupt_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_startup_interrupt.py"
                    $<TARGET_FILE:cjsh>)
    endif()
    if(UNIX)
        cjsh_add_test(
            NAME terminal_selection_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_terminal_selection.py"
                    $<TARGET_FILE:cjsh>
        )
        cjsh_add_test(
            NAME shell_lifecycle_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_shell_lifecycle.py"
                    $<TARGET_FILE:cjsh> $<TARGET_FILE:lifecycle_probe>
        )
    endif()
    if(UNIX)
        cjsh_add_test(
            NAME isocline_terminal_contract_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/isocline/test_terminal_contract.py"
                    $<TARGET_FILE:terminal_contract_driver>
        )
    endif()
    cjsh_add_test(
        NAME child_signal_startup_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_child_signal_startup.py"
                $<TARGET_FILE:cjsh>
    )

    cjsh_add_test(
        NAME isocline_pty_integration_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/isocline/test_isocline_pty.py"
                $<TARGET_FILE:isocline_pty_driver>
    )

    cjsh_add_test(
        NAME build_system_configuration_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_system/test_build_system.py"
                "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(build_system_configuration_tests PROPERTIES PROCESSORS 2)

    cjsh_add_test(
        NAME interactive_shutdown_hangup_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_interactive_shutdown.py"
                $<TARGET_FILE:cjsh>
    )

    cjsh_add_test(
        NAME agent_mode_interactive_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_agent_mode_interactive.py"
                $<TARGET_FILE:cjsh>
    )

    cjsh_add_test(
        NAME browser_interactive_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_browser_interactive.py"
                $<TARGET_FILE:cjsh>
    )

    if(UNIX)
        cjsh_add_test(
            NAME idle_hook_interactive_tests
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/core/test_idle_hook_interactive.py"
                $<TARGET_FILE:cjsh>
        )
        cjsh_add_test(
            NAME job_notification_interactive_tests
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/core/test_job_notifications_interactive.py"
                $<TARGET_FILE:cjsh>
        )
    endif()

    if(UNIX)
        cjsh_add_test(
            NAME generate_completions_integration_tests
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/core/test_generate_completions.py"
                $<TARGET_FILE:cjsh>
        )
    endif()

    if(TARGET fg_terminal_race_injector)
        cjsh_add_test(
            NAME process_group_launch_tests
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/core/test_process_group_launch.py"
                $<TARGET_FILE:cjsh> $<TARGET_FILE:fg_terminal_race_injector>
        )
        cjsh_add_test(
            NAME function_pipeline_job_control_tests
            COMMAND
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/core/test_function_pipeline_job_control.py"
                $<TARGET_FILE:cjsh> $<TARGET_FILE:fg_terminal_race_injector>
        )
    endif()

    cjsh_add_test(
        NAME test_runner_terminal_safety
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runner/test_terminal_safety.py"
                $<TARGET_FILE:cjsh_test_runner> $<TARGET_FILE:function_syntax_tests>
    )
    cjsh_add_test(
        NAME ctest_summary_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runner/test_ctest_summary.py"
    )
    cjsh_add_test(
        NAME terminal_recovery_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/core/test_terminal_recovery.py"
                $<TARGET_FILE:cjsh>
    )
endif()
