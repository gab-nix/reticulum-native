set(SEMTECH_SX126X_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/semtech/sx126x_driver")

include("${SEMTECH_SX126X_ROOT}/verify.cmake")
reticulum_verify_semtech_sx126x("${SEMTECH_SX126X_ROOT}")

# Keep the upstream driver source byte-for-byte intact. The Heltec-specific
# SPI, BUSY, reset and interrupt HAL is supplied by a later component.
idf_component_register(
    SRCS
        "${SEMTECH_SX126X_ROOT}/src/sx126x.c"
        "${SEMTECH_SX126X_ROOT}/src/sx126x_driver_version.c"
    INCLUDE_DIRS
        "${SEMTECH_SX126X_ROOT}/src"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
