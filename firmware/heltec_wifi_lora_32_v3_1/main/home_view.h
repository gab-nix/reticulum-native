/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_HOME_VIEW_H
#define HELTEC_HOME_VIEW_H
#include "reticulum/interface.h"
#include <stdbool.h>
typedef struct {
    uint64_t rx_packets, tx_packets;
    size_t heap_free, heap_minimum;
    rns_interface_stats_t radio;
    bool radio_valid;
} heltec_home_snapshot;
void heltec_home_lines(const heltec_home_snapshot *snapshot, char lines[8][22]);
#endif
