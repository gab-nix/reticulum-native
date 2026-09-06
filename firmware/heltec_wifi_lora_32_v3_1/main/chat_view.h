/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHAT_VIEW_H
#define HELTEC_CHAT_VIEW_H
#include "chat_store.h"
#include "peer_label.h"
typedef rns_status_t (*heltec_reply_fn)(void *context,const uint8_t sender[16],const char *text);
typedef rns_status_t (*heltec_cancel_fn)(void *context,const uint8_t id[32]);
typedef bool (*heltec_delivery_line_fn)(void *context,const uint8_t id[32],char line[22]);
typedef struct {
    unsigned screen, action;
    uint8_t sender[16], message[32];
    bool selected, message_selected, back;
    rns_status_t error;
    char reply_error[22]; /* Optional bounded application diagnostic. */
    unsigned reply;
    heltec_reply_fn send_reply;
    void *reply_context;
    heltec_cancel_fn cancel_reply;
    heltec_delivery_line_fn delivery_line;
    bool reply_queued;
    unsigned page;
    heltec_peer_name_fn peer_name;
    bool read_attempted;
    uint8_t read_id[32];
    rns_status_t read_error;
} heltec_chat_view;
/* Pure one-button controller; true returns to the main menu. */
bool heltec_chat_view_poll(heltec_chat_view *view, heltec_chat_store *store,
                          bool next, bool select, char lines[8][22]);
bool heltec_chat_view_open_unread(heltec_chat_view *view,const heltec_chat_store *store);
#endif
