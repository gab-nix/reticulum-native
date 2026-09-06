set(RETICULUM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(
    SRCS
        "esp_platform.c"
        "esp_delay.c"
        "esp_crypto.c"
        "nvs_storage.c"
        "${RETICULUM_ROOT}/src/crypto/provider.c"
        "${RETICULUM_ROOT}/src/identity/identity.c"
        "${RETICULUM_ROOT}/src/announce/announce.c"
        "${RETICULUM_ROOT}/src/packet/packet.c"
        "${RETICULUM_ROOT}/src/link/link.c"
        "${RETICULUM_ROOT}/src/link/embedded_link.c"
        "${RETICULUM_ROOT}/src/destination/destination.c"
        "${RETICULUM_ROOT}/src/transport/proof.c"
        "${RETICULUM_ROOT}/src/lxmf/lxmf.c"
        "${RETICULUM_ROOT}/src/lxmf/router.c"
        "${RETICULUM_ROOT}/src/lxmf/delivery.c"
        "${RETICULUM_ROOT}/src/lxmf/packet_node.c"
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
        espressif__libsodium
        freertos
        heap
        mbedtls
        nvs_flash
        semtech_sx126x
)

# ESP-IDF's public newlib wrappers intentionally use GCC's `#include_next`, so
# compiling an IDF component with -Wpedantic would turn SDK headers into build
# errors. Keep all ordinary component warnings fatal while using the GNU C17
# dialect selected by ESP-IDF.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
