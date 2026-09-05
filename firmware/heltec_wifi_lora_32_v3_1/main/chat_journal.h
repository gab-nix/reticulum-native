/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_CHAT_JOURNAL_H
#define HELTEC_CHAT_JOURNAL_H
#include "reticulum/storage.h"
/* Exactly 64 KiB reserved for eight two-sector records; offsets are relative
 * to an exclusively owned region, never the identity/NVS partition. */
typedef struct {
    void *context;
    rns_status_t (*read)(void *, size_t, uint8_t *, size_t);
    rns_status_t (*erase)(void *, size_t, size_t);
    rns_status_t (*write)(void *, size_t, const uint8_t *, size_t);
} heltec_chat_flash_ops;
/* Ops/context borrowed for the storage lifetime. Single caller only. */
rns_status_t heltec_chat_journal_open(const heltec_chat_flash_ops *ops, rns_storage_t **out);
#ifdef ESP_PLATFORM
rns_status_t heltec_chat_flash_open(rns_storage_t **out);
#endif
#endif
