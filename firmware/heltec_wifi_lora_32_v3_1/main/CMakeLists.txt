idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES heltec_v3_1_bsp nvs_flash reticulum_idf
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic -Werror)
