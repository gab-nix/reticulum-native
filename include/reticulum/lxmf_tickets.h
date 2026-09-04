#ifndef RETICULUM_LXMF_TICKETS_H
#define RETICULUM_LXMF_TICKETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_TICKET_EXPIRY_SECONDS (21u * 24u * 60u * 60u)
#define LXMF_TICKET_GRACE_SECONDS (5u * 24u * 60u * 60u)
#define LXMF_TICKET_RENEW_SECONDS (14u * 24u * 60u * 60u)
#define LXMF_TICKET_INTERVAL_SECONDS (24u * 60u * 60u)
#define LXMF_TICKET_STORE_MAX_ENTRIES 1024u
#define LXMF_TICKET_STORE_PATH_MAX 1023u

typedef struct lxmf_ticket_store lxmf_ticket_store_t;

typedef struct lxmf_ticket_entry {
    uint64_t expires_at;
    uint8_t ticket[LXMF_TICKET_LENGTH];
} lxmf_ticket_entry_t;

/* Opens an existing versioned ticket store or creates an empty in-memory
 * store for a missing path. Mutations use path.tmp + fsync + atomic rename. */
lxmf_status_t lxmf_ticket_store_open(lxmf_ticket_store_t **store,
                                     const char *path);
void lxmf_ticket_store_close(lxmf_ticket_store_t *store);

/* Returns a reusable inbound ticket for peer, or securely creates one. A
 * recently delivered ticket is not repeated until the one-day interval has
 * elapsed, matching LXMF's ticket delivery throttling. */
lxmf_status_t lxmf_ticket_store_issue(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    uint64_t now, lxmf_ticket_entry_t *ticket, bool *created);

/* Remembers a ticket received from peer for stamping future outbound
 * messages. Expired tickets are rejected. */
lxmf_status_t lxmf_ticket_store_remember_outbound(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    const lxmf_ticket_entry_t *ticket, uint64_t now);
lxmf_status_t lxmf_ticket_store_get_outbound(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    uint64_t now, lxmf_ticket_entry_t *ticket);

/* Copies currently valid tickets issued to peer into caller storage. */
lxmf_status_t lxmf_ticket_store_get_inbound(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    uint64_t now, lxmf_ticket_entry_t *tickets, size_t capacity,
    size_t *ticket_count);

/* Creates or verifies the 16-byte ticket-backed stamp used by LXMF. */
lxmf_status_t lxmf_ticket_store_stamp_outbound(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    uint64_t now, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    uint8_t stamp[LXMF_STAMP_LENGTH]);
lxmf_status_t lxmf_ticket_store_validate_inbound(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    uint64_t now, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    const uint8_t stamp[LXMF_STAMP_LENGTH]);

lxmf_status_t lxmf_ticket_store_mark_delivered(
    lxmf_ticket_store_t *store, const uint8_t peer[LXMF_DESTINATION_LENGTH],
    uint64_t now);
lxmf_status_t lxmf_ticket_store_cleanup(lxmf_ticket_store_t *store,
                                        uint64_t now);
size_t lxmf_ticket_store_count(const lxmf_ticket_store_t *store);

#ifdef __cplusplus
}
#endif

#endif
