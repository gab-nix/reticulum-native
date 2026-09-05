idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES heltec_reticulum_radio heltec_v3_1_bsp nvs_flash reticulum_idf
)
# ESP-IDF public headers intentionally use GNU extensions such as
# `#include_next`; keep ordinary warnings fatal without rejecting SDK headers.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
