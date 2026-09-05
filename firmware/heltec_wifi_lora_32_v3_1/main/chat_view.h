/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHAT_VIEW_H
#define HELTEC_CHAT_VIEW_H
#include "chat_store.h"
typedef struct {
    unsigned screen, action;
    uint8_t sender[16], message[32];
    bool selected, message_selected, back;
    rns_status_t error;
} heltec_chat_view;
/* Pure one-button controller; true returns to the main menu. */
bool heltec_chat_view_poll(heltec_chat_view *view, heltec_chat_store *store,
                          bool next, bool select, char lines[8][22]);
#endif
