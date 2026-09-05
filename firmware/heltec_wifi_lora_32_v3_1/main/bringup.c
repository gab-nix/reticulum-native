/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bringup.h"
#include "radio_discovery.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "reticulum/boards/heltec_status_ui_esp.h"
#include "reticulum/heltec_sx1262.h"
#include <inttypes.h>

static const char *TAG = "bringup";
/* Keep bounded receive state off the crypto verification task stack. */
static heltec_radio_discovery discovery;

static const char *radio_state(rns_status_t status, rns_sx1262_state_t state) {
    if (status != RNS_OK) return "RADIO ERROR";
    switch (state) {
    case RNS_SX1262_RECEIVING: return "RX ONLY 868.100 SF11";
    case RNS_SX1262_STOPPED: return "RADIO STOPPED";
    case RNS_SX1262_FAULT: return "RADIO FAULT";
    case RNS_SX1262_SCANNING: return "UNEXPECTED CAD";
    case RNS_SX1262_TRANSMITTING: return "UNEXPECTED TX";
    default: return "UNKNOWN RADIO STATE";
    }
}

void heltec_bringup_run(void) {
    heltec_radio_discovery_init(&discovery);
    rns_heltec_oled_esp_t *display = NULL;
    rns_heltec_sx1262_t *radio = NULL;
    rns_sx1262_config_t config;
    /* Radio uses its own owner task and power domain. Display failure must
       not prevent receive operation, and radio failure must remain visible. */
    bool display_ready = rns_heltec_oled_esp_open(&display);
    ESP_LOGI(TAG, "OLED: %s", display_ready ? "ready" : "unavailable");
    rns_sx1262_default_config(&config);
    config.frequency_hz = 868100000U;
    config.bandwidth_hz = 250000U;
    config.spreading_factor = 11U;
    config.coding_rate_denominator = 5U;
    rns_status_t radio_status = rns_heltec_sx1262_open_with_config(&config, &radio);
    ESP_LOGI(TAG, "Radio RX initialization: %d; %lu Hz BW%lu SF%u CR4/%u preamble%u; TX disabled",
        (int)radio_status, (unsigned long)config.frequency_hz,
        (unsigned long)config.bandwidth_hz, (unsigned)config.spreading_factor,
        (unsigned)config.coding_rate_denominator, (unsigned)config.preamble_symbols);
    uint64_t next_display_ms = 0U;
    uint64_t next_log_ms = 0U;
    bool display_error_reported = !display_ready;
    for (;;) {
        uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
        rns_sx1262_stats_t stats = {0};
        heltec_radio_discovery_poll(&discovery, now_ms);
        if (radio != NULL) {
            /* Bound work; only verified announce metadata survives this pass. */
            for (size_t i = 0U; i < RNS_SX1262_RX_QUEUE_CAPACITY; ++i) {
                rns_sx1262_packet_t packet;
                if (rns_heltec_sx1262_receive(radio, &packet) != RNS_OK) break;
                heltec_radio_discovery_receive(&discovery, packet.data, packet.length, now_ms);
            }
            radio_status = rns_heltec_sx1262_get_stats(radio, &stats);
        }
        const char *state = radio_state(radio_status, stats.state);
        if (now_ms >= next_display_ms) {
            if (display_ready) {
                rns_heltec_oled_t *core = rns_heltec_oled_esp_core(display);
                rns_heltec_oled_set_discovery_count(core, (uint16_t)discovery.peer_count);
                rns_heltec_oled_set_diagnostics(core, state,
                    (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    stats.rx_packets, stats.last_rssi_dbm, stats.last_snr_db,
                    stats.rx_packets != 0U);
                if (!rns_heltec_oled_render(core)) display_ready = false;
            }
            if (!display_ready && !display_error_reported) {
                ESP_LOGE(TAG, "OLED failed; radio receive continues");
                display_error_reported = true;
            }
            next_display_ms = now_ms + 1000U;
        }
        if (now_ms >= next_log_ms) {
            ESP_LOGI(TAG, "Discovery packets=%" PRIu64 " verified=%" PRIu64
                " peers=%u invalid=%" PRIu64 " duplicate=%" PRIu64 " stale=%" PRIu64,
                discovery.packets, discovery.verified, (unsigned)discovery.peer_count,
                discovery.invalid, discovery.duplicates, discovery.stale);
            ESP_LOGI(TAG, "%s rx=%" PRIu64 " tx=%" PRIu64 " crc=%" PRIu64
                " overflow=%" PRIu64 " recovery=%" PRIu64 " error=%d api=%d heap=%lu stack=%lu oled=%s",
                state, stats.rx_packets, stats.tx_packets, stats.crc_errors,
                stats.rx_overflows, stats.recoveries, (int)stats.last_error,
                (int)radio_status, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL), display_ready ? "ready" : "offline");
            next_log_ms = now_ms + 10000U;
        }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
}
