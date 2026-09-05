/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_LIVE_VIEW_H
#define HELTEC_LIVE_VIEW_H
#include "radio_discovery.h"
typedef struct {
    char messages[8][96];
    size_t count, selected;
    uint8_t node_key[16];
    bool node_selected;
} heltec_live_view;
/* Volatile, bounded display history. Call only for verified messages. */
void heltec_live_message(heltec_live_view *view, const uint8_t *text, size_t length);
void heltec_live_messages(heltec_live_view *view, bool next, char lines[8][22]);
void heltec_live_nodes(heltec_live_view *view, const heltec_radio_discovery *nodes,
                      uint64_t now, bool next, char lines[8][22]);
#endif
