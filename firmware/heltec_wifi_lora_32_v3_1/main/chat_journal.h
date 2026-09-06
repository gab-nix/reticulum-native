/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHAT_JOURNAL_H
#define HELTEC_CHAT_JOURNAL_H
#include "reticulum/storage.h"
/* Legacy chats occupy the first 64 KiB unchanged. New message/outbox/settings
 * records extend the region; offsets are relative
 * to an exclusively owned region, never the identity/NVS partition. */
#define HELTEC_CHAT_JOURNAL_RECORDS 109U
#define HELTEC_CHAT_JOURNAL_BYTES (HELTEC_CHAT_JOURNAL_RECORDS*8192U)
typedef struct {
    void *context;
    rns_status_t (*read)(void *, size_t, uint8_t *, size_t);
    rns_status_t (*erase)(void *, size_t, size_t);
    rns_status_t (*write)(void *, size_t, const uint8_t *, size_t);
} heltec_chat_flash_ops;
/* Ops/context borrowed for the storage lifetime. Single caller only. */
rns_status_t heltec_chat_journal_open(const heltec_chat_flash_ops *ops, rns_storage_t **out);
/* Invalid pairs are preserved and reserved when healthy local records establish
 * journal ownership. With no valid record, ambiguous nonblank flash fails closed. */
rns_status_t heltec_chat_journal_open_report(const heltec_chat_flash_ops *ops,
    rns_storage_t **out, size_t *quarantined);
#ifdef ESP_PLATFORM
rns_status_t heltec_chat_flash_open(rns_storage_t **out);
size_t heltec_chat_flash_quarantined(void);
#endif
#endif
