/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_LIVE_VIEW_H
#define HELTEC_LIVE_VIEW_H
#include "radio_discovery.h"
#include "peer_label.h"
typedef bool (*heltec_peer_snapshot_fn)(void *context,size_t slot,uint8_t address[16],bool *observed);
typedef struct {
    char messages[8][96];
    size_t count, selected;
    uint8_t node_key[16];
    bool node_selected;
    heltec_peer_name_fn peer_name;
    heltec_peer_snapshot_fn peer_snapshot;
    void *peer_context;
    uint8_t notification_sender[16];
    char notification_preview[22];
    uint64_t notification_until;
} heltec_live_view;
/* Volatile, bounded display history. Call only for verified messages. */
void heltec_live_message(heltec_live_view *view, const uint8_t *text, size_t length);
void heltec_live_notify(heltec_live_view *view,const uint8_t sender[16],const uint8_t *text,size_t length,uint64_t now);
bool heltec_live_notification(const heltec_live_view *view,uint64_t now,char sender[22],char preview[22]);
void heltec_live_messages(heltec_live_view *view, bool next, char lines[8][22]);
void heltec_live_nodes(heltec_live_view *view, const heltec_radio_discovery *nodes,
                      uint64_t now, bool next, char lines[8][22]);
#endif
