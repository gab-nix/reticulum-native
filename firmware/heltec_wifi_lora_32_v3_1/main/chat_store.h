/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHAT_STORE_H
#define HELTEC_CHAT_STORE_H
#include "reticulum/storage.h"
#include <stdbool.h>
#define HELTEC_CHAT_COUNT 8U
#define HELTEC_CHAT_MESSAGES 8U
#define HELTEC_CHAT_TEXT 384U
typedef struct {
    uint8_t id[32];
    uint64_t timestamp;
    uint16_t length;
    uint8_t state; /* 0 verified inbound; 1 queued; 2 sent; 3 delivered; 4 failed. */
    uint8_t text[HELTEC_CHAT_TEXT];
} heltec_chat_message;
typedef struct {
    bool used;
    uint8_t sender[16], count;
    heltec_chat_message messages[HELTEC_CHAT_MESSAGES];
} heltec_chat;
typedef struct heltec_chat_store heltec_chat_store;
/* Storage is borrowed. Application-owned data, no radio/crypto operations.
 * Fail closed on corrupt records; never erase identity or configuration. */
rns_status_t heltec_chat_store_open(rns_storage_t *storage, heltec_chat_store **out);
void heltec_chat_store_close(heltec_chat_store *store);
const heltec_chat *heltec_chat_store_get(const heltec_chat_store *store, size_t slot);
rns_status_t heltec_chat_store_add(heltec_chat_store *store, const uint8_t sender[16],
                                  const heltec_chat_message *message);
rns_status_t heltec_chat_store_delete(heltec_chat_store *store, size_t slot);
/* Caller must supply receipt-validated transitions; this store does not
 * establish delivery authenticity. Inbound records cannot be changed. */
rns_status_t heltec_chat_store_set_state(heltec_chat_store *store, const uint8_t sender[16],
                                       const uint8_t id[32], uint8_t state);
#endif
