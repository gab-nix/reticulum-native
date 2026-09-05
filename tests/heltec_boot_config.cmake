cmake_minimum_required(VERSION 3.16)
set(guard "${CMAKE_CURRENT_LIST_DIR}/../firmware/heltec_wifi_lora_32_v3_1/main/boot_stack_guard.cmake")
function(check_config stack canary watchpoint expected)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DCONFIG_ESP_MAIN_TASK_STACK_SIZE=${stack}"
        "-DCONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=${canary}"
        "-DCONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=${watchpoint}"
        -P "${guard}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
    if(expected AND NOT result EQUAL 0)
        message(FATAL_ERROR "Safe boot configuration rejected")
    elseif(NOT expected AND result EQUAL 0)
        message(FATAL_ERROR "Unsafe boot configuration accepted: ${stack}/${canary}/${watchpoint}")
    endif()
endfunction()
check_config(12288 ON ON TRUE)
check_config(16384 ON ON TRUE)
check_config(3584 ON ON FALSE)
check_config(8192 ON ON FALSE)
check_config(12287 ON ON FALSE)
check_config(12288 OFF ON FALSE)
check_config(12288 ON OFF FALSE)
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${guard}"
    RESULT_VARIABLE missing_result OUTPUT_QUIET ERROR_QUIET)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "Missing resolved configuration accepted")
endif()
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../firmware/heltec_wifi_lora_32_v3_1/sdkconfig.defaults" defaults)
foreach(required IN ITEMS "CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288"
        "CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y"
        "CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y")
    if(NOT required IN_LIST defaults)
        message(FATAL_ERROR "Missing safe firmware default: ${required}")
    endif()
endforeach()
