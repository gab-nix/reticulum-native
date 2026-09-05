idf_component_register(
    SRCS "heltec_reticulum_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES heltec_sx1262 reticulum_idf reticulum_sx1262
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
