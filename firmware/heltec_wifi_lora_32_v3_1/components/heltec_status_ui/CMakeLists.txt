idf_component_register(
    SRCS "heltec_status_ui.c" "heltec_status_ui_esp.c"
    INCLUDE_DIRS "include"
    REQUIRES heltec_v3_1_bsp
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_rom freertos
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
