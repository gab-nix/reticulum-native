set(RETICULUM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
    SRCS
        "${RETICULUM_ROOT}/src/platform/interface.c"
        "${RETICULUM_ROOT}/src/interfaces/radio_framing.c"
        "${RETICULUM_ROOT}/src/interfaces/sx1262_interface.c"
    INCLUDE_DIRS "${RETICULUM_ROOT}/include"
    REQUIRES reticulum_idf
)

# ESP-IDF public headers intentionally use GNU extensions such as
# `#include_next`; keep ordinary warnings fatal without rejecting SDK headers.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
