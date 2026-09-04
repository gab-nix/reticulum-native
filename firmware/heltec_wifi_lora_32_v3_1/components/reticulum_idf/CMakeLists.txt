set(RETICULUM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
    SRCS
        "esp_platform.c"
        "esp_delay.c"
        "nvs_storage.c"
        "${RETICULUM_ROOT}/src/platform/platform.c"
        "${RETICULUM_ROOT}/src/platform/status.c"
        "${RETICULUM_ROOT}/src/platform/storage.c"
        "${RETICULUM_ROOT}/src/platform/storage_record.c"
    INCLUDE_DIRS
        "include"
        "${RETICULUM_ROOT}/include"
    REQUIRES
        bootloader_support
        esp_timer
        esp_system
        freertos
        heap
        nvs_flash
        semtech_sx126x
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic -Werror)
