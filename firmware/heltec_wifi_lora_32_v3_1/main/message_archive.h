/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_MESSAGE_ARCHIVE_H
#define HELTEC_MESSAGE_ARCHIVE_H
#include "reticulum/lxmf.h"
#include "reticulum/storage.h"
typedef struct {
    bool used;
    uint8_t source[16], id[32];
    lxmf_signature_state_t signature;
    uint64_t received;
    uint16_t text_length, packet_length;
    uint8_t text[384], packet[500];
} heltec_archived_message;
typedef struct heltec_message_archive heltec_message_archive;
rns_status_t heltec_message_archive_open(rns_storage_t *storage, heltec_message_archive **out);
void heltec_message_archive_close(heltec_message_archive *archive);
const heltec_archived_message *heltec_message_archive_get(const heltec_message_archive *archive, size_t slot);
/* Only library-validated events may set signature status. This module does
 * not authenticate packets. Non-OK never mutates the in-memory record. */
rns_status_t heltec_message_archive_put(heltec_message_archive *archive, const heltec_archived_message *message);
/* Removes one record transactionally; caller must confirm destructive UI actions. */
rns_status_t heltec_message_archive_remove(heltec_message_archive *archive, size_t slot);
#endif
