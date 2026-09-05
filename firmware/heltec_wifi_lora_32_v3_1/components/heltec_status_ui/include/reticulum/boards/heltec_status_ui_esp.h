/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_BOARDS_HELTEC_STATUS_UI_ESP_H
#define RETICULUM_BOARDS_HELTEC_STATUS_UI_ESP_H

#include "reticulum/boards/heltec_status_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_heltec_oled_esp rns_heltec_oled_esp_t;

/* Single owner task only. Opens the board's dedicated display bus with default
 * enabled settings. On failure *output is NULL and acquired resources are freed.
 * Do not open a second instance while the first owns the display pins. */
bool rns_heltec_oled_esp_open(rns_heltec_oled_esp_t **output);
/* Borrowed pointer, valid until close. NULL-safe. */
rns_heltec_oled_t *rns_heltec_oled_esp_core(rns_heltec_oled_esp_t *handle);
/* Releases I2C resources and powers down Vext. Does not touch radio GPIOs. */
void rns_heltec_oled_esp_close(rns_heltec_oled_esp_t *handle);

#ifdef __cplusplus
}
#endif
#endif
