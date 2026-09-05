# Validate the resolved configuration, not only sdkconfig.defaults: existing
# sdkconfig files otherwise silently retain IDF's undersized task stack.
if(NOT DEFINED CONFIG_ESP_MAIN_TASK_STACK_SIZE OR
   CONFIG_ESP_MAIN_TASK_STACK_SIZE LESS 8192)
    message(FATAL_ERROR "Heltec crypto startup requires CONFIG_ESP_MAIN_TASK_STACK_SIZE >= 8192; update the existing sdkconfig")
endif()
if(NOT CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY OR
   NOT CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK)
    message(FATAL_ERROR "Heltec startup requires stack canaries and the end-of-stack watchpoint")
endif()
