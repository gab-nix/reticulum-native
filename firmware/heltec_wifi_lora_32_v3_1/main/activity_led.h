/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_ACTIVITY_LED_H
#define HELTEC_ACTIVITY_LED_H
#include "reticulum/interface.h"
#include <stdbool.h>
typedef struct {
    uint64_t rx, tx, last_ms, until_ms;
    bool initialized;
} heltec_activity_led;
/* One board LED: short RX pulses, longer successful-TX pulses. No delays. */
static inline bool heltec_activity_led_sample(heltec_activity_led *v,
        uint64_t now, const rns_interface_stats_t *s) {
    if (!s->radio_telemetry_valid) {
        v->initialized = false; v->until_ms = 0; return false;
    }
    if (!v->initialized || now < v->last_ms ||
        s->radio_rx_frames < v->rx || s->radio_tx_frames < v->tx) {
        v->initialized = true; v->until_ms = 0;
    } else {
        unsigned duration = s->radio_tx_frames > v->tx ? 240U :
                            s->radio_rx_frames > v->rx ? 120U : 0U;
        if (duration) {
            uint64_t end = now > UINT64_MAX-duration ? UINT64_MAX : now+duration;
            if (end > v->until_ms) v->until_ms = end;
        }
    }
    v->rx = s->radio_rx_frames; v->tx = s->radio_tx_frames; v->last_ms = now;
    return now < v->until_ms;
}
#endif
