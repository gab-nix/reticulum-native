#ifndef RETICULUM_LXMF_PEER_STORE_H
#define RETICULUM_LXMF_PEER_STORE_H

#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_PEER_STORE_VERSION 1u
#define LXMF_PEER_STORE_MAX_PEERS 1024u
#define LXMF_PEER_NAME_MAX 128u
#define LXMF_PEER_NOTE_MAX 1024u
#define LXMF_PEER_DRAFT_MAX 4096u
#define LXMF_PEER_STORE_PATH_MAX 1023u

typedef enum {
    LXMF_PEER_TRUST_UNKNOWN = 0,
    LXMF_PEER_TRUST_TRUSTED = 1,
    LXMF_PEER_TRUST_UNTRUSTED = 2
} lxmf_peer_trust_t;

typedef enum {
    LXMF_PEER_PROPAGATION_AUTOMATIC = 0,
    LXMF_PEER_PROPAGATION_DIRECT_ONLY = 1,
    LXMF_PEER_PROPAGATION_PREFERRED = 2
} lxmf_peer_propagation_t;

typedef struct {
    uint8_t address[LXMF_DESTINATION_LENGTH];
    char display_name[LXMF_PEER_NAME_MAX + 1u];
    size_t display_name_len;
    lxmf_peer_trust_t trust;
    bool blocked;
    bool pinned;
    char note[LXMF_PEER_NOTE_MAX + 1u];
    size_t note_len;
    lxmf_peer_propagation_t propagation;
    bool has_propagation_node;
    uint8_t propagation_node[LXMF_DESTINATION_LENGTH];
    uint64_t last_seen_ms;
    uint64_t last_announce_ms;
    uint32_t unread_count;
    char draft[LXMF_PEER_DRAFT_MAX + 1u];
    size_t draft_len;
} lxmf_peer_t;

typedef struct { void *implementation; } lxmf_peer_store_t;
typedef bool (*lxmf_peer_store_list_fn)(void *context,
                                        const lxmf_peer_t *peer);

/* Opens path and recovers a valid path.tmp left by an interrupted save. */
lxmf_status_t lxmf_peer_store_open(lxmf_peer_store_t *store, const char *path);
void lxmf_peer_store_close(lxmf_peer_store_t *store);
size_t lxmf_peer_store_count(const lxmf_peer_store_t *store);
lxmf_status_t lxmf_peer_store_get(const lxmf_peer_store_t *store,
    const uint8_t address[LXMF_DESTINATION_LENGTH], lxmf_peer_t *peer);
/* Validates and inserts or replaces one peer in memory. */
lxmf_status_t lxmf_peer_store_put(lxmf_peer_store_t *store,
                                  const lxmf_peer_t *peer, bool *inserted);
lxmf_status_t lxmf_peer_store_remove(lxmf_peer_store_t *store,
    const uint8_t address[LXMF_DESTINATION_LENGTH], bool *removed);
lxmf_status_t lxmf_peer_store_list(const lxmf_peer_store_t *store,
    lxmf_peer_store_list_fn callback, void *context);
/* Atomically writes the complete versioned, checksummed snapshot. */
lxmf_status_t lxmf_peer_store_save(lxmf_peer_store_t *store);

#ifdef __cplusplus
}
#endif
#endif
