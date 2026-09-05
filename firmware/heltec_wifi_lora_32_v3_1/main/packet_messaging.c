/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "packet_messaging.h"
#include "announce_button.h"
#include "radio_discovery.h"
#include "reticulum/boards/heltec_reticulum_radio.h"
#include "reticulum/boards/heltec_status_ui_esp.h"
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/hal.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>
static const char *TAG = "messaging";
static heltec_radio_discovery discovery;
static lxmf_packet_node_t *node;
static rns_heltec_oled_esp_t *display;
static uint64_t tx_done, tx_failed, preview_until;
static uint64_t clock_ms(void *context) { (void)context; return (uint64_t)esp_timer_get_time() / 1000U; }
static rns_status_t entropy(void *context, uint8_t *out, size_t size) {
    (void)context; return rns_hal_random_bytes(out, size);
}
static void tx_result(void *context, uint32_t id, rns_sx1262_packet_outcome_t outcome, rns_status_t status) {
    (void)context; (void)id;
    if (outcome == RNS_SX1262_PACKET_SENT) ++tx_done; else ++tx_failed;
    ESP_LOGI(TAG, "RF completion outcome=%d status=%d (not a message delivery receipt)", (int)outcome, (int)status);
}
static void incoming_message(void *context, const lxmf_message_t *message) {
    (void)context;
    ESP_LOGI(TAG, "Verified short LXMF received; content bytes=%u", (unsigned)message->content.len);
    if (display) {
        rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
        if (rns_heltec_oled_show_preview(oled, message->content.data, message->content.len, clock_ms(NULL))) {
            rns_heltec_oled_settings_t settings = oled->settings;
            settings.screen = RNS_HELTEC_OLED_SCREEN_MESSAGE;
            rns_heltec_oled_set_settings(oled, &settings);
            preview_until = clock_ms(NULL) + 30000U;
        }
    }
}
static rns_status_t received(void *context, const uint8_t *packet, size_t length) {
    (void)context;
    heltec_radio_discovery_packet(&discovery, packet, length, clock_ms(NULL));
    /* Malformed or unsupported packets must not stop the radio poll loop. */
    (void)lxmf_packet_node_receive(node, packet, length);
    return RNS_OK;
}
void heltec_packet_messaging_run(rns_storage_t *storage) {
    rns_interface_t *radio = NULL;
    rns_heltec_reticulum_radio_config_t config;
    static const rns_sx1262_clock_ops_t clocks = {.monotonic_ms = clock_ms, .entropy = entropy};
    heltec_announce_button button = {0};
    heltec_radio_discovery_init(&discovery);
    rns_heltec_reticulum_radio_default_config(&config);
    config.frequency_hz = 868100000U;
    config.scheduler.bandwidth_hz = 250000U;
    config.scheduler.spreading_factor = 11U;
    config.scheduler.coding_rate_denominator = 5U;
    config.scheduler.preamble_symbols = 18U;
    config.scheduler.duty_cycle_ppm = 10000U;
    config.tx_power_dbm = 14;
    rns_status_t status = rns_heltec_reticulum_radio_create(&config, &clocks, NULL, tx_result, NULL, &radio);
    ESP_LOGI(TAG, "Radio provider creation: %d", (int)status);
    if (status == RNS_OK) status = lxmf_packet_node_create(storage, radio, incoming_message, NULL, &node);
    ESP_LOGI(TAG, "Packet identity opening: %d", (int)status);
    if (status == RNS_OK) status = rns_interface_start(radio);
    if (status != RNS_OK) {
        ESP_LOGE(TAG, "Packet-mode startup failed: %d; storage not erased", (int)status);
        lxmf_packet_node_destroy(node); node = NULL;
        if (radio) rns_interface_destroy(radio);
        return;
    }
    (void)rns_heltec_oled_esp_open(&display);
    if (display) {
        rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
        rns_heltec_oled_settings_t settings = oled->settings;
        settings.preview_timeout_ms = 30000U;
        rns_heltec_oled_set_settings(oled, &settings);
    }
    gpio_config_t input = {.pin_bit_mask = UINT64_C(1) << RNS_HELTEC_V3_1_GPIO_PRG,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    bool button_ready = gpio_config(&input) == ESP_OK;
    ESP_LOGI(TAG, "868.100 MHz SF11 BW250 CR4/5; packet LXMF; PRG=%s; no automatic announce",
        button_ready ? "ready" : "unavailable");
    uint64_t next_display = 0, next_log = 0;
    for (;;) {
        uint64_t now = clock_ms(NULL);
        status = rns_interface_poll(radio, received, NULL, 4U);
        heltec_radio_discovery_poll(&discovery, now);
        if (button_ready && heltec_announce_button_poll(&button,
            gpio_get_level(RNS_HELTEC_V3_1_GPIO_PRG) == 0, now)) {
            rns_status_t announced = lxmf_packet_node_announce(node, (uint64_t)HELTEC_BUILD_EPOCH + now / 1000U);
            ESP_LOGI(TAG, "PRG announce queue status=%d; airtime/CAD scheduling applies", (int)announced);
        }
        lxmf_packet_node_stats_t messages;
        lxmf_packet_node_stats(node, &messages);
        if (now >= next_display && display) {
            rns_heltec_oled_t *oled = rns_heltec_oled_esp_core(display);
            rns_heltec_oled_poll(oled, now);
            if (now >= preview_until) {
                rns_heltec_oled_set_discovery_count(oled, (uint16_t)discovery.identity_count);
                rns_heltec_oled_set_diagnostics(oled, status == RNS_OK ? "RX/TX 868.100 SF11" : "RADIO ERROR",
                    (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT), discovery.packets, 0, 0, false);
            }
            if (!rns_heltec_oled_render(oled)) {
                rns_heltec_oled_esp_close(display); display = NULL;
                ESP_LOGE(TAG, "OLED offline; radio continues");
            }
            next_display = now + 1000U;
        }
        if (now >= next_log) {
            ESP_LOGI(TAG, "Ingress=%" PRIu64 " malformed=%" PRIu64 " ifac=%" PRIu64
                " types(data,announce,link,proof)=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                " learned=%" PRIu64 " other_dest=%" PRIu64 " local_data=%" PRIu64
                " local_other=%" PRIu64 " unsupported_layout=%" PRIu64,
                messages.ingress, messages.malformed, messages.ifac_rejected,
                messages.packet_types[0], messages.packet_types[1], messages.packet_types[2], messages.packet_types[3],
                messages.learned_announces, messages.other_destinations, messages.local_data,
                messages.local_other, messages.unsupported_data_layout);
            ESP_LOGI(TAG, "RX=%" PRIu64 " TX=%" PRIu64 " TXfail=%" PRIu64
                " messages=%" PRIu64 " rejected=%" PRIu64 " unknown=%" PRIu64
                " unsupported_links=%" PRIu64
                " proofs=%" PRIu64 " last_message=%d poll=%d heap=%lu stack=%lu",
                discovery.packets, tx_done, tx_failed, messages.messages, messages.rejected,
                messages.unknown_senders, messages.unsupported_packets, messages.proofs_queued, (int)messages.last_message_status,
                (int)status, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            next_log = now + 10000U;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
}
