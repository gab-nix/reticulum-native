/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_ARCHIVE_VIEW_H
#define HELTEC_ARCHIVE_VIEW_H
#include "message_archive.h"
typedef struct {
    uint8_t id[32];
    bool selected, confirm, remove, deleted;
    uint8_t deleted_id[32]; /* Caller consumes this event after each poll. */
    rns_status_t error;
} heltec_archive_view;
/* Quarantined records only. Tap advances, hold opens a Cancel-default action
 * screen. A second hold returns to chats; selecting Delete requires a new hold. */
bool heltec_archive_view_poll(heltec_archive_view *view, heltec_message_archive *archive,
    bool next, bool select, char lines[8][22]);
#endif
