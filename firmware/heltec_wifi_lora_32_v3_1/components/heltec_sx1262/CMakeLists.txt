idf_component_register(SRCS "sx1262_bus.c" "sx1262_radio.c" "sx1262_semtech.c" "esp_sx1262.c"
 INCLUDE_DIRS "include" REQUIRES driver esp_timer freertos heltec_v3_1_bsp reticulum_idf semtech_sx126x)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
