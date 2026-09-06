/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_LXMF_PACKET_NODE_H
#define RETICULUM_LXMF_PACKET_NODE_H
#include "reticulum/lxmf.h"
#include "reticulum/interface.h"
#include "reticulum/storage.h"
typedef struct lxmf_packet_node lxmf_packet_node_t;
typedef enum { LXMF_PEER_KNOWN, LXMF_PEER_CONNECTING, LXMF_PEER_LINKED, LXMF_PEER_UNREACHABLE } lxmf_packet_peer_state;
typedef struct {
    bool delivery, has_ratchet, metadata_valid;
    uint8_t stamp_cost;
    char display_name[128];
    bool observed_this_boot;
    lxmf_packet_peer_state state;
} lxmf_packet_peer_info;
typedef bool (*lxmf_packet_peer_protected_fn)(void *context, const uint8_t destination[16]);
/* Borrowed peer00..peer31 store; open before packet reception. Bad records are
 * reserved, not erased. Restored signed announces are reverified, not live paths. */
rns_status_t lxmf_packet_node_open_peers(lxmf_packet_node_t *node, rns_storage_t *storage,
    lxmf_packet_peer_protected_fn protected_peer, void *context);
/* Snapshot of peer-cache storage health; not message delivery status. */
rns_status_t lxmf_packet_node_peer_storage_status(const lxmf_packet_node_t *node);
/* Snapshot from verified announces only. No keys or borrowed private state. */
bool lxmf_packet_node_peer_info(const lxmf_packet_node_t *node,
    const uint8_t destination[16], lxmf_packet_peer_info *info);
bool lxmf_packet_node_peer_at(const lxmf_packet_node_t *node, size_t slot,
    uint8_t destination[16], lxmf_packet_peer_info *info);
/* Enable caller-polled direct links before reception/sending. Existing saved
 * opportunistic outbox records retain their original delivery representation. */
rns_status_t lxmf_packet_node_enable_links(lxmf_packet_node_t *node);
rns_status_t lxmf_packet_node_request_peer(lxmf_packet_node_t *node,const uint8_t destination[16],uint64_t now_ms);
/* Local archive replay only. Direct archives are reverified without trying to
 * prove a defunct link; a sender retransmission obtains its normal proof. */
rns_status_t lxmf_packet_node_replay_archive(lxmf_packet_node_t *node,const uint8_t *record,size_t length);
/* Message slices are borrowed only for the callback. No plaintext is logged.
 * Single caller ownership; callbacks must not re-enter or destroy the node. */
typedef void (*lxmf_packet_message_fn)(void *context, const lxmf_message_t *message);
/* Optional durable receive gate, called before replay acceptance/proofs.
 * All spans are borrowed. Unknown-signature messages are never acknowledged.
 * Return non-OK on persistence failure to permit retransmission recovery.
 * Like message callbacks, this hook must not reenter/destroy the endpoint. */
typedef rns_status_t (*lxmf_packet_accept_fn)(void *context,
    const lxmf_message_t *message, lxmf_signature_state_t signature,
    const uint8_t *packet, size_t packet_length);
void lxmf_packet_node_set_accept(lxmf_packet_node_t *node,
    lxmf_packet_accept_fn accept, void *context);
typedef struct {
    uint64_t messages, duplicates, rejected, unknown_senders, proofs_queued;
    uint64_t unsupported_packets;
    /* Aggregate ingress diagnostics only: never addresses, keys or payloads. */
    uint64_t ingress, malformed, ifac_rejected, announces, learned_announces;
    uint64_t packet_types[4], other_destinations, local_data, local_other;
    uint64_t unsupported_data_layout;
    uint64_t pending_senders, expired_pending;
    lxmf_status_t last_message_status;
    rns_status_t last_send_status;
} lxmf_packet_node_stats_t;
/* Storage/interface are borrowed. First boot persists keys before use;
 * corrupted records fail closed and are never silently replaced. */
rns_status_t lxmf_packet_node_create(rns_storage_t *storage,
    rns_interface_t *interface_value, lxmf_packet_message_fn callback,
    void *context, lxmf_packet_node_t **output);
void lxmf_packet_node_destroy(lxmf_packet_node_t *node);
const uint8_t *lxmf_packet_node_address(const lxmf_packet_node_t *node);
rns_status_t lxmf_packet_node_announce(lxmf_packet_node_t *node, uint64_t unix_seconds);
/* Caller dispatches complete, IFAC-free Reticulum packets, not PHY frames.
 * Up to four unknown-signer packets are retained without proof for five
 * minutes, then revalidated after a verified announce. Legacy message callbacks
 * remain verified-only; the optional acceptance hook sees unverified events.
 * Duplicate cache is bounded and volatile; this is not a durable inbox. */
rns_status_t lxmf_packet_node_receive(lxmf_packet_node_t *node,
    const uint8_t *packet, size_t length);
void lxmf_packet_node_stats(const lxmf_packet_node_t *node, lxmf_packet_node_stats_t *stats);
/* Recheck an archived packet against current verified peer keys. No callback,
 * replay-cache mutation or RF transmission. Expected source/ID bind the result
 * to the archived record. On non-OK, signature is not modified. */
rns_status_t lxmf_packet_node_check_archive(lxmf_packet_node_t *node,
    const uint8_t *packet, size_t length, const uint8_t source[16],
    const uint8_t message_id[32], lxmf_signature_state_t *signature);
/* Drop volatile deferred copies after an application-confirmed deletion.
 * Does not block future packets from the sender or erase identity data. */
void lxmf_packet_node_forget_pending(lxmf_packet_node_t *node, const uint8_t message_id[32]);
typedef enum {
    LXMF_PACKET_QUEUED=1, LXMF_PACKET_TRANSMITTING, LXMF_PACKET_AWAITING_PROOF,
    LXMF_PACKET_DELIVERED, LXMF_PACKET_FAILED, LXMF_PACKET_CANCELLED
} lxmf_packet_send_state;
typedef struct {
    uint8_t id[32], destination[16], text[32];
    size_t text_length;
    uint64_t timestamp;
    unsigned attempts;
    lxmf_packet_send_state state;
    rns_status_t error;
    bool durable;
    bool direct;
} lxmf_packet_outgoing;
/* Four transactional out0..out3 records; separate from identity storage.
 * Open before sending. All calls use the endpoint's single owner task.
 * Poll persists transitions before sending. Pending attempts survive reboot.
 * Quick text is bounded to 32 bytes; stamps/resources are not generated.
 * Direct records v3 retain anti-downgrade flags (v1/v2 remain readable).
 * Handshake-only IO/timeout may fall back to a verified zero-cost ratchet
 * peer before DATA ever queues. Invalid authentication, prior DATA, cancelled
 * sends and interrupted direct attempts restored after reboot cannot fall
 * back. Conversion preserves signed bytes/ID and persists before RF. */
rns_status_t lxmf_packet_node_open_outbox(lxmf_packet_node_t *node, rns_storage_t *storage);
rns_status_t lxmf_packet_node_send(lxmf_packet_node_t *node, const uint8_t destination[16],
    const uint8_t *text, size_t length, uint64_t timestamp, uint8_t id[32]);
void lxmf_packet_node_poll(lxmf_packet_node_t *node, uint64_t now_ms);
/* Only bits 0..3 permit a slot to transmit. Persistence and deadlines always
 * advance, including for held slots. Use when application history is pending. */
void lxmf_packet_node_poll_ready(lxmf_packet_node_t *node, uint64_t now_ms, uint8_t ready_mask);
void lxmf_packet_node_tx_complete(lxmf_packet_node_t *node, uint32_t transmission_id,
    rns_status_t status, uint64_t now_ms);
bool lxmf_packet_node_outgoing(const lxmf_packet_node_t *node, size_t slot, lxmf_packet_outgoing *out);
/* Cancels future retries; a frame already queued at the interface can still
 * transmit. A terminal record is released only after application history saves. */
rns_status_t lxmf_packet_node_cancel(lxmf_packet_node_t *node, const uint8_t id[32]);
rns_status_t lxmf_packet_node_release(lxmf_packet_node_t *node, size_t slot);
#endif
