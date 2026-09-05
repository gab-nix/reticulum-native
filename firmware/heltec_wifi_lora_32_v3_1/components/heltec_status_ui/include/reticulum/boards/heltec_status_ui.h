/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_BOARDS_HELTEC_STATUS_UI_H
#define RETICULUM_BOARDS_HELTEC_STATUS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/boards/heltec_wifi_lora_32_v3_1.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RNS_HELTEC_OLED_FRAME_BYTES = 1024,
    RNS_HELTEC_OLED_TEXT_COLUMNS = 31,
    RNS_HELTEC_OLED_TEXT_ROWS = 10,
    RNS_HELTEC_OLED_STATUS_TEXT_MAX = 31,
    RNS_HELTEC_OLED_PREVIEW_MAX = 95
};

typedef enum {
    RNS_HELTEC_OLED_SCREEN_STATUS = 0,
    RNS_HELTEC_OLED_SCREEN_MESSAGE = 1,
    RNS_HELTEC_OLED_SCREEN_ROUTES = 2
} rns_heltec_oled_screen_t;

typedef struct {
    bool enabled;
    bool preview_enabled;
    uint8_t brightness;
    /* Zero keeps a preview until it is replaced or previews are disabled. */
    uint32_t preview_timeout_ms;
    rns_heltec_oled_screen_t screen;
} rns_heltec_oled_settings_t;

typedef struct {
    char radio[RNS_HELTEC_OLED_STATUS_TEXT_MAX + 1];
    char address_suffix[RNS_HELTEC_OLED_STATUS_TEXT_MAX + 1];
    uint16_t peer_count;
    uint16_t route_count;
    uint16_t unread_count;
    char preview[RNS_HELTEC_OLED_PREVIEW_MAX + 1];
} rns_heltec_oled_model_t;

typedef struct {
    void *context;
    rns_heltec_v3_1_gpio_ops_t gpio;
    bool (*configure_bus)(void *context, int sda_gpio, int scl_gpio,
                          uint8_t address);
    bool (*write_command)(void *context, const uint8_t *data, size_t length);
    bool (*write_data)(void *context, const uint8_t *data, size_t length);
} rns_heltec_oled_ops_t;

typedef struct {
    rns_heltec_oled_ops_t ops;
    rns_heltec_oled_settings_t settings;
    rns_heltec_oled_model_t model;
    uint8_t frame[RNS_HELTEC_OLED_FRAME_BYTES];
    uint64_t preview_deadline_ms;
    bool ready;
    bool failed;
    bool dirty;
} rns_heltec_oled_t;

void rns_heltec_oled_settings_default(rns_heltec_oled_settings_t *settings);
bool rns_heltec_oled_init(rns_heltec_oled_t *oled,
                          const rns_heltec_oled_ops_t *ops,
                          const rns_heltec_oled_settings_t *settings);
void rns_heltec_oled_set_settings(rns_heltec_oled_t *oled,
                                  const rns_heltec_oled_settings_t *settings);
void rns_heltec_oled_set_preview_enabled(rns_heltec_oled_t *oled,
                                         bool enabled);
void rns_heltec_oled_set_status(rns_heltec_oled_t *oled,
                                const char *radio,
                                const char *address_suffix,
                                uint16_t peers,
                                uint16_t routes,
                                uint16_t unread);
bool rns_heltec_oled_show_preview(rns_heltec_oled_t *oled,
                                  const uint8_t *utf8,
                                  size_t length,
                                  uint64_t now_ms);
void rns_heltec_oled_poll(rns_heltec_oled_t *oled, uint64_t now_ms);
bool rns_heltec_oled_render(rns_heltec_oled_t *oled);
bool rns_heltec_oled_is_ready(const rns_heltec_oled_t *oled);
bool rns_heltec_oled_has_failed(const rns_heltec_oled_t *oled);

#ifdef __cplusplus
}
#endif
#endif
