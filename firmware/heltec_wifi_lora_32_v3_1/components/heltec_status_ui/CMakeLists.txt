idf_component_register(
    SRCS "heltec_status_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES heltec_v3_1_bsp
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
