if(NOT DEFINED RNID OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "RNID and TEST_DIR are required")
endif()

set(identity_file "${TEST_DIR}/rnid-test-identity")
file(REMOVE "${identity_file}")
execute_process(COMMAND "${RNID}" generate "${identity_file}"
                RESULT_VARIABLE generate_result OUTPUT_VARIABLE generate_output)
if(NOT generate_result EQUAL 0 OR NOT generate_output MATCHES "Identity : [0-9a-f]+")
    message(FATAL_ERROR "rnid generate failed: ${generate_output}")
endif()
execute_process(COMMAND "${RNID}" show "${identity_file}"
                RESULT_VARIABLE show_result OUTPUT_VARIABLE show_output)
if(NOT show_result EQUAL 0 OR NOT show_output STREQUAL generate_output)
    message(FATAL_ERROR "rnid show mismatch")
endif()
execute_process(COMMAND "${RNID}" generate "${identity_file}"
                RESULT_VARIABLE duplicate_result ERROR_QUIET)
if(duplicate_result EQUAL 0)
    message(FATAL_ERROR "rnid overwrote an existing identity")
endif()
file(REMOVE "${identity_file}")
