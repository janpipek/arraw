# Runs a command and fails unless it exits with EXPECTED_EXIT and, when given,
# writes EXPECTED_ERR somewhere in its stderr. CTest itself can only tell zero
# from non-zero, and a usage error (2) is a different promise from a failure (1).
#
#   cmake -DEXPECTED_EXIT=2 [-DEXPECTED_ERR=text] -P ExpectExit.cmake -- <command> [arguments...]

set(command)
set(collecting OFF)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last})
    if(collecting)
        list(APPEND command "${CMAKE_ARGV${index}}")
    elseif("${CMAKE_ARGV${index}}" STREQUAL "--")
        set(collecting ON)
    endif()
endforeach()
if(NOT command)
    message(FATAL_ERROR "No command given after --")
endif()

execute_process(COMMAND ${command} RESULT_VARIABLE code OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT "${code}" STREQUAL "${EXPECTED_EXIT}")
    message(FATAL_ERROR "Expected exit ${EXPECTED_EXIT}, got '${code}'. stderr:\n${err}")
endif()
if(DEFINED EXPECTED_ERR)
    string(FIND "${err}" "${EXPECTED_ERR}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Expected stderr to contain '${EXPECTED_ERR}'. stderr:\n${err}")
    endif()
endif()
