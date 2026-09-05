/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/crypto.h"
#include "reticulum/packet.h"
#include "reticulum/proof.h"
#include <stdlib.h>
#include <string.h>
#define PEERS 32U
#define REPLAYS 32U
struct lxmf_packet_node {
    rns_storage_t *storage;
    rns_interface_t *interface_value;
    rns_identity identity;
    uint8_t ratchet_private[32], ratchet_public[32], address[16], name_hash[10];
    struct { bool used; uint8_t address[16]; rns_identity identity; uint64_t timestamp; } peers[PEERS];
    size_t next_peer, next_replay;
    uint64_t last_announce;
    uint8_t replay[REPLAYS][32];
    bool replay_used[REPLAYS];
    uint8_t plain[RNS_MTU], raw[RNS_MTU], body[RNS_MTU];
    lxmf_packet_message_fn callback;
    void *context;
    lxmf_packet_node_stats_t stats;
};
static const char *aspects[] = {"delivery"};
static const rns_identity *resolve(void *context, const uint8_t source[16]) {
    lxmf_packet_node_t *n = context;
    for (size_t i = 0; i < PEERS; ++i)
        if (n->peers[i].used && memcmp(n->peers[i].address, source, 16) == 0)
            return &n->peers[i].identity;
    return NULL;
}
rns_status_t lxmf_packet_node_create(rns_storage_t *storage, rns_interface_t *interface_value,
    lxmf_packet_message_fn callback, void *context, lxmf_packet_node_t **output) {
    if (!storage || !interface_value || !output) return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    lxmf_packet_node_t *n = calloc(1, sizeof(*n));
    if (!n) return RNS_ERROR_NO_MEMORY;
    n->storage = storage; n->interface_value = interface_value;
    n->callback = callback; n->context = context;
    uint8_t record[105] = {1}, ratchet_id[16];
    size_t length = 0;
    rns_status_t status = rns_storage_read(storage, "identity", record, sizeof(record), &length);
    if (status == RNS_ERROR_NOT_FOUND) {
        memset(record, 0, sizeof(record)); record[0] = 1;
        if (!rns_identity_generate(&n->identity) ||
            !rns_identity_export_private(&n->identity, record + 1) ||
            !rns_identity_ratchet_generate(record + 65, n->ratchet_public, ratchet_id))
            status = RNS_ERROR_CRYPTO;
        else status = rns_storage_write_atomic(storage, "identity", record, sizeof(record));
    } else if (status == RNS_OK && (length != sizeof(record) || record[0] != 1)) {
        status = RNS_ERROR_PROTOCOL;
    }
    if (status == RNS_OK) {
        for (size_t i = 97; i < sizeof(record); ++i)
            n->last_announce = (n->last_announce << 8) | record[i];
        if (n->last_announce > RNS_ANNOUNCE_MAX_TIMESTAMP) status = RNS_ERROR_PROTOCOL;
        memcpy(n->ratchet_private, record + 65, 32);
        if (!rns_identity_from_private(&n->identity, record + 1) ||
            !rns_x25519_public_from_private(n->ratchet_private, n->ratchet_public) ||
            !rns_destination_hash(&n->identity, "lxmf", aspects, 1, n->address) ||
            !rns_destination_name_hash("lxmf", aspects, 1, n->name_hash)) status = RNS_ERROR_CRYPTO;
    }
    rns_hal_secure_zero(record, sizeof(record));
    if (status != RNS_OK) { lxmf_packet_node_destroy(n); return status; }
    *output = n;
    return RNS_OK;
}
void lxmf_packet_node_destroy(lxmf_packet_node_t *n) {
    if (n) { rns_hal_secure_zero(n, sizeof(*n)); free(n); }
}
const uint8_t *lxmf_packet_node_address(const lxmf_packet_node_t *n) { return n ? n->address : NULL; }
rns_status_t lxmf_packet_node_announce(lxmf_packet_node_t *n, uint64_t timestamp) {
    if (!n || timestamp > RNS_ANNOUNCE_MAX_TIMESTAMP) return RNS_ERROR_INVALID_ARGUMENT;
    if (n->last_announce == RNS_ANNOUNCE_MAX_TIMESTAMP) return RNS_ERROR_OVERFLOW;
    if (timestamp <= n->last_announce) timestamp = n->last_announce + 1U;
    uint8_t random[5], app[192]; size_t app_length, length;
    lxmf_announce_data_t data = {0};
    memcpy(data.display_name, "Heltec", 6); data.display_name_len = 6;
    rns_packet p = {.packet_type = 1};
    if (rns_hal_random_bytes(random, sizeof(random)) != RNS_OK ||
        lxmf_announce_encode(&data, app, sizeof(app), &app_length) != LXMF_OK ||
        !rns_announce_build(&n->identity, n->address, n->name_hash, random, timestamp,
            n->ratchet_public, app, app_length, n->body, sizeof(n->body), &p.data_length, &p.context_flag))
        return RNS_ERROR_CRYPTO;
    memcpy(p.destination_hash, n->address, 16); p.data = n->body;
    if (!rns_packet_encode(&p, n->raw, sizeof(n->raw), &length)) return RNS_ERROR_OVERFLOW;
    /* Reserve a strictly increasing timebase before transmission, including
       across resets on boards without a battery-backed clock. */
    uint8_t record[105] = {1};
    if (!rns_identity_export_private(&n->identity, record + 1)) return RNS_ERROR_CRYPTO;
    memcpy(record + 65, n->ratchet_private, 32);
    for (size_t i = 0; i < 8; ++i) record[97 + i] = (uint8_t)(timestamp >> (56U - 8U * i));
    rns_status_t saved = rns_storage_write_atomic(n->storage, "identity", record, sizeof(record));
    rns_hal_secure_zero(record, sizeof(record));
    if (saved != RNS_OK) return n->stats.last_send_status = saved;
    n->last_announce = timestamp;
    return n->stats.last_send_status = rns_interface_send(n->interface_value, n->raw, length);
}
static void learn(lxmf_packet_node_t *n, const rns_packet *p) {
    rns_announce a;
    if (p->destination_type != 0 || !rns_announce_parse(&a, p->data, p->data_length, p->context_flag) ||
        memcmp(a.name_hash, n->name_hash, 10) != 0 ||
        !rns_announce_verify(p->destination_hash, p->data, p->data_length, p->context_flag)) return;
    size_t slot = PEERS;
    for (size_t i = 0; i < PEERS; ++i)
        if (n->peers[i].used && memcmp(n->peers[i].address, p->destination_hash, 16) == 0) {
            if (a.timestamp < n->peers[i].timestamp) return;
            slot = i; break;
        }
    if (slot == PEERS) { slot = n->next_peer; n->next_peer = (slot + 1) % PEERS; }
    if (!rns_identity_from_public(&n->peers[slot].identity, a.public_key)) return;
    memcpy(n->peers[slot].address, p->destination_hash, 16);
    n->peers[slot].timestamp = a.timestamp; n->peers[slot].used = true;
    ++n->stats.learned_announces;
}
rns_status_t lxmf_packet_node_receive(lxmf_packet_node_t *n, const uint8_t *raw, size_t length) {
    rns_packet p;
    if (!n) return RNS_ERROR_INVALID_ARGUMENT;
    ++n->stats.ingress;
    if (!raw || !length) { ++n->stats.malformed; return RNS_ERROR_PROTOCOL; }
    if (raw[0] & 0x80U) { ++n->stats.ifac_rejected; return RNS_ERROR_PROTOCOL; }
    if (!rns_packet_decode(&p, raw, length)) { ++n->stats.malformed; return RNS_ERROR_PROTOCOL; }
    ++n->stats.packet_types[p.packet_type];
    if (p.packet_type == 1) { ++n->stats.announces; learn(n, &p); return RNS_OK; }
    if (memcmp(p.destination_hash, n->address, 16)) { ++n->stats.other_destinations; return RNS_OK; }
    if (p.packet_type == 2) { ++n->stats.unsupported_packets; return RNS_ERROR_UNSUPPORTED; }
    if (p.packet_type != 0) { ++n->stats.local_other; return RNS_OK; }
    ++n->stats.local_data;
    if (p.header_type != 0 || p.destination_type != 0 || p.context != 0)
        ++n->stats.unsupported_data_layout;
    lxmf_identity_verifier_context_t verifier = {resolve, n};
    lxmf_message_t message; size_t plain_length = 0;
    lxmf_status_t status = lxmf_opportunistic_packet_unpack_ratchets(raw, length, &n->identity,
        n->ratchet_private, 1, 0, lxmf_identity_verifier, &verifier,
        n->plain, sizeof(n->plain), &plain_length, &message, NULL, NULL);
    n->stats.last_message_status = status;
    if (status != LXMF_OK) {
        ++n->stats.rejected;
        if (status == LXMF_ERR_UNKNOWN_SIGNER) ++n->stats.unknown_senders;
        rns_hal_secure_zero(n->plain, sizeof(n->plain));
        return RNS_OK;
    }
    bool duplicate = false;
    for (size_t i = 0; i < REPLAYS; ++i)
        if (n->replay_used[i] && !memcmp(n->replay[i], message.message_id, 32)) duplicate = true;
    if (duplicate) ++n->stats.duplicates;
    else {
        memcpy(n->replay[n->next_replay], message.message_id, 32);
        n->replay_used[n->next_replay] = true; n->next_replay = (n->next_replay + 1) % REPLAYS;
        ++n->stats.messages;
        if (n->callback) n->callback(n->context, &message);
    }
    /* Repeat proofs for duplicates, allowing recovery from a lost proof. */
    uint8_t hash[32]; size_t proof_length;
    rns_packet proof = {.packet_type = 3, .data = n->body, .data_length = RNS_PROOF_EXPLICIT_SIZE};
    if (!rns_packet_hash(raw, length, hash) || !rns_proof_generate_explicit(&n->identity, hash, n->body))
        n->stats.last_send_status = RNS_ERROR_CRYPTO;
    else {
        memcpy(proof.destination_hash, hash, 16);
        if (!rns_packet_encode(&proof, n->raw, sizeof(n->raw), &proof_length))
            n->stats.last_send_status = RNS_ERROR_OVERFLOW;
        else {
            n->stats.last_send_status = rns_interface_send(n->interface_value, n->raw, proof_length);
            if (n->stats.last_send_status == RNS_OK) ++n->stats.proofs_queued;
        }
    }
    rns_hal_secure_zero(n->plain, sizeof(n->plain));
    return RNS_OK;
}
void lxmf_packet_node_stats(const lxmf_packet_node_t *n, lxmf_packet_node_stats_t *stats) {
    if (n && stats) *stats = n->stats;
}
