idf_component_register(
    SRCS "heltec_shell_core.c"
    INCLUDE_DIRS "include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
