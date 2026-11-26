# summarize_ctest.cmake
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

# The current log is not renamed to LastTest.log until after post-test commands.
# Newer CMake versions append a random suffix to the temporary filename. Read
# the newest temporary log, never the previous run's finalized LastTest.log.
file(GLOB test_logs "${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/LastTest*.log.tmp*")
set(current_log "")
foreach(test_log IN LISTS test_logs)
    if(current_log STREQUAL "" OR "${test_log}" IS_NEWER_THAN "${current_log}")
        set(current_log "${test_log}")
    endif()
endforeach()
if(current_log STREQUAL "")
    return()
endif()

# Parse the log as bytes so invalid UTF-8 in test output does not stop awk.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C
            awk -f "${CMAKE_CURRENT_LIST_DIR}/summarize_ctest.awk" "${current_log}"
    RESULT_VARIABLE summary_result
)
if(NOT summary_result EQUAL 0)
    message("Individual test counts unavailable: could not summarize CTest output.")
endif()
