idf_component_register(
    SRCS "app_main.c" "bringup.c"
    INCLUDE_DIRS "."
    REQUIRES heltec_reticulum_radio heltec_v3_1_bsp heltec_status_ui heltec_sx1262 nvs_flash reticulum_idf
)
# Run after registration: IDF's early dependency scan has no resolved config.
include("${CMAKE_CURRENT_LIST_DIR}/boot_stack_guard.cmake")
# ESP-IDF public headers intentionally use GNU extensions such as
# `#include_next`; keep ordinary warnings fatal without rejecting SDK headers.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
