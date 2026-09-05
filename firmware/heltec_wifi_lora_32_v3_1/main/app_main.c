/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "reticulum/boards/heltec_wifi_lora_32_v3_1.h"
#include "reticulum/esp_idf.h"
#include "bringup.h"

static const char *TAG = "reticulum";
static rns_storage_t *storage;

void app_main(void) {
    const rns_heltec_v3_1_board_t *board = rns_heltec_v3_1_board();
    esp_err_t status = nvs_flash_init();

    if (status == ESP_ERR_NVS_NO_FREE_PAGES ||
        status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS requires explicit recovery; refusing to erase identity storage");
        return;
    }
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialisation failed: %s", esp_err_to_name(status));
        return;
    }
    if (rns_esp_platform_install() != RNS_OK) {
        ESP_LOGE(TAG, "Reticulum ESP-IDF platform installation failed");
        return;
    }
    ESP_LOGI(TAG, "Boot stage: crypto provider installation");
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "Heap integrity failed before crypto installation");
        return;
    }
    if (rns_esp_crypto_install() != RNS_OK) {
        ESP_LOGE(TAG, "Reticulum ESP-IDF crypto provider installation failed");
        return;
    }
    ESP_LOGI(TAG, "Boot stage: crypto self-test");
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "Heap integrity failed before crypto self-test");
        return;
    }
    if (rns_esp_crypto_self_test() != RNS_OK) {
        ESP_LOGE(TAG, "Reticulum ESP-IDF crypto known-answer test failed");
        return;
    }
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "Heap integrity failed after crypto self-test");
        return;
    }
    /* ESP-IDF reports this value in bytes, unlike upstream FreeRTOS. */
    unsigned long headroom = (unsigned long)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Crypto self-test passed; heap intact; stack headroom: %lu bytes", headroom);
    if (headroom < 2048UL) {
        ESP_LOGE(TAG, "Boot stack headroom below required 2048 bytes");
        return;
    }
    ESP_LOGI(TAG, "Boot stage: storage provider opening");
    if (rns_esp_nvs_storage_open(NULL, &storage) != RNS_OK) {
        ESP_LOGE(TAG, "Reticulum NVS storage provider could not be opened");
        return;
    }

    ESP_LOGI(TAG, "Reticulum hardware diagnostics for %s", board->name);
    ESP_LOGI(TAG, "UART%d console at %lu baud on TX GPIO%d/RX GPIO%d",
             board->console_uart, (unsigned long)board->console_baud,
             board->uart_tx, board->uart_rx);
    ESP_LOGI(TAG, "ESP-IDF platform, crypto and bounded NVS storage providers are ready");
    ESP_LOGW(TAG, "Identity and LXMF services remain disabled; starting receive-only hardware diagnostics");
    heltec_bringup_run();
}
