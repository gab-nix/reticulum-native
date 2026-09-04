idf_component_register(
    SRCS "heltec_wifi_lora_32_v3_1.c"
    INCLUDE_DIRS "include"
)
# ESP-IDF public headers intentionally use GNU extensions such as
# `#include_next`; keep ordinary warnings fatal without rejecting SDK headers.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
