/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_LXMF_PACKET_NODE_H
#define RETICULUM_LXMF_PACKET_NODE_H
#include "reticulum/lxmf.h"
#include "reticulum/interface.h"
#include "reticulum/storage.h"
typedef struct lxmf_packet_node lxmf_packet_node_t;
/* Message slices are borrowed only for the callback. No plaintext is logged.
 * Single caller ownership; callbacks must not re-enter or destroy the node. */
typedef void (*lxmf_packet_message_fn)(void *context, const lxmf_message_t *message);
typedef struct {
    uint64_t messages, duplicates, rejected, unknown_senders, proofs_queued;
    uint64_t unsupported_packets;
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
 * Unknown signers are rejected without proof; announce sender before sending.
 * Duplicate cache is bounded and volatile; this is not a durable inbox. */
rns_status_t lxmf_packet_node_receive(lxmf_packet_node_t *node,
    const uint8_t *packet, size_t length);
void lxmf_packet_node_stats(const lxmf_packet_node_t *node, lxmf_packet_node_stats_t *stats);
#endif
