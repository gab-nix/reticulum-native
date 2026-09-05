/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHANNEL_VIEW_H
#define HELTEC_CHANNEL_VIEW_H
#include "reticulum/interface.h"
#include <stdbool.h>
typedef struct {
    uint8_t samples[60]; /* RX=1, TX=2, CAD busy=4, unobserved=8. */
    uint64_t second, rx, tx, busy;
    bool initialized, previous_valid;
} heltec_channel_view;
void heltec_channel_sample(heltec_channel_view *v, uint64_t now_ms,
                          const rns_interface_stats_t *stats);
void heltec_channel_lines(const heltec_channel_view *v,
                         const rns_interface_stats_t *stats, char lines[8][22]);
#endif
