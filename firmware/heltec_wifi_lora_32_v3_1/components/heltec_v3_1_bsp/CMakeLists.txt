idf_component_register(
    SRCS "heltec_wifi_lora_32_v3_1.c"
    INCLUDE_DIRS "include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic -Werror)
