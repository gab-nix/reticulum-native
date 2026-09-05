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
#include <stdio.h>
#define PEERS 32U
#define REPLAYS 32U
#define PENDING 4U
struct lxmf_packet_node {
    rns_storage_t *storage;
    rns_interface_t *interface_value;
    rns_identity identity;
    uint8_t ratchet_private[32], ratchet_public[32], address[16], name_hash[10];
    struct { bool used, delivery, has_ratchet, requires_stamp; uint8_t address[16], ratchet[32]; rns_identity identity; uint64_t timestamp, delivery_timestamp; } peers[PEERS];
    size_t next_peer, next_replay;
    uint64_t last_announce;
    uint8_t replay[REPLAYS][32];
    bool replay_used[REPLAYS];
    struct { bool used; uint8_t raw[RNS_MTU], source[16], id[32]; size_t length; uint64_t received_ms; } pending[PENDING];
    uint8_t plain[RNS_MTU], raw[RNS_MTU], body[RNS_MTU];
    lxmf_packet_message_fn callback;
    lxmf_packet_accept_fn accept;
    void *accept_context;
    void *context;
    lxmf_packet_node_stats_t stats;
    rns_storage_t *outbox;
    struct {
        bool used, dirty;
        lxmf_packet_outgoing view;
        rns_identity recipient;
        uint8_t raw[500], hash[32]; size_t length;
        uint32_t transmission_id;
        uint64_t deadline, ready;
    } outgoing[4];
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
void lxmf_packet_node_set_accept(lxmf_packet_node_t *n, lxmf_packet_accept_fn accept, void *context) {
    if (n) { n->accept = accept; n->accept_context = context; }
}
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
        !rns_announce_verify(p->destination_hash, p->data, p->data_length, p->context_flag)) return;
    rns_identity identity;
    uint8_t delivery[16];
    if (!rns_identity_from_public(&identity, a.public_key) ||
        !rns_destination_hash(&identity, "lxmf", aspects, 1, delivery)) return;
    size_t slot = PEERS;
    for (size_t i = 0; i < PEERS; ++i)
        if (n->peers[i].used && memcmp(n->peers[i].address, delivery, 16) == 0) {
            slot = i; break;
        }
    if (slot == PEERS) { slot = n->next_peer; n->next_peer = (slot + 1) % PEERS; memset(&n->peers[slot],0,sizeof(n->peers[slot])); }
    if(a.timestamp<n->peers[slot].timestamp && memcmp(p->destination_hash,delivery,16)) return;
    if (!rns_identity_from_public(&n->peers[slot].identity, a.public_key)) return;
    memcpy(n->peers[slot].address, delivery, 16);
    if(a.timestamp>n->peers[slot].timestamp) n->peers[slot].timestamp = a.timestamp;
    n->peers[slot].used = true;
    if(!memcmp(p->destination_hash,delivery,16) && a.timestamp>=n->peers[slot].delivery_timestamp) {
        lxmf_announce_data_t data={0};
        n->peers[slot].delivery=true;
        n->peers[slot].delivery_timestamp=a.timestamp;
        n->peers[slot].has_ratchet=a.has_ratchet;
        if(a.has_ratchet) memcpy(n->peers[slot].ratchet,a.ratchet,32);
        n->peers[slot].requires_stamp=a.app_data_length &&
            (lxmf_announce_parse(a.app_data,a.app_data_length,&data)!=LXMF_OK ||
             (data.has_stamp_cost && data.stamp_cost));
    }
    ++n->stats.learned_announces;
    for (size_t i = 0; i < PENDING; ++i) {
        if (!n->pending[i].used || memcmp(n->pending[i].source, delivery, 16)) continue;
        n->pending[i].used = false; --n->stats.pending_senders;
        (void)lxmf_packet_node_receive(n, n->pending[i].raw, n->pending[i].length);
        rns_hal_secure_zero(&n->pending[i], sizeof(n->pending[i]));
    }
}
rns_status_t lxmf_packet_node_receive(lxmf_packet_node_t *n, const uint8_t *raw, size_t length) {
    rns_packet p;
    if (!n) return RNS_ERROR_INVALID_ARGUMENT;
    uint64_t now = 0;
    bool clock_ok = rns_hal_monotonic_ms(&now) == RNS_OK;
    for (size_t i = 0; i < PENDING; ++i) {
        if (n->pending[i].used && (!clock_ok || now < n->pending[i].received_ms ||
            now - n->pending[i].received_ms >= 300000U)) {
            rns_hal_secure_zero(&n->pending[i], sizeof(n->pending[i]));
            --n->stats.pending_senders; ++n->stats.expired_pending;
        }
    }
    ++n->stats.ingress;
    if (!raw || !length) { ++n->stats.malformed; return RNS_ERROR_PROTOCOL; }
    if (raw[0] & 0x80U) { ++n->stats.ifac_rejected; return RNS_ERROR_PROTOCOL; }
    if (!rns_packet_decode(&p, raw, length)) { ++n->stats.malformed; return RNS_ERROR_PROTOCOL; }
    ++n->stats.packet_types[p.packet_type];
    if (p.packet_type == 1) { ++n->stats.announces; learn(n, &p); return RNS_OK; }
    if(p.packet_type==3 && p.header_type==0 && p.destination_type==0 && p.context==0) {
        for(size_t i=0;i<4;++i) if(n->outgoing[i].used && n->outgoing[i].view.attempts &&
            n->outgoing[i].view.state<LXMF_PACKET_DELIVERED &&
            !memcmp(p.destination_hash,n->outgoing[i].hash,16) &&
            rns_proof_validate(&n->outgoing[i].recipient,n->outgoing[i].hash,p.data,p.data_length)) {
            n->outgoing[i].view.state=LXMF_PACKET_DELIVERED;
            n->outgoing[i].view.error=RNS_OK; n->outgoing[i].dirty=true;
        }
    }
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
        if (status == LXMF_ERR_UNKNOWN_SIGNER) {
            ++n->stats.unknown_senders;
            size_t slot = PENDING;
            bool duplicate_pending = false;
            for (size_t i = 0; i < PENDING; ++i) {
                if (n->pending[i].used && !memcmp(n->pending[i].id, message.message_id, 32)) duplicate_pending = true;
                if (!n->pending[i].used && slot == PENDING) slot = i;
            }
            if (!duplicate_pending && n->accept)
                (void)n->accept(n->accept_context, &message, LXMF_SIGNATURE_UNVERIFIED, raw, length);
            /* The bounded ciphertext cache remains available even when an
             * application cannot yet archive unknown senders. No proof is
             * issued, and later verified acceptance still requires saving. */
            if (clock_ok && !duplicate_pending && slot != PENDING && length <= RNS_MTU) {
                memcpy(n->pending[slot].raw, raw, length);
                memcpy(n->pending[slot].source, message.source, 16);
                memcpy(n->pending[slot].id, message.message_id, 32);
                n->pending[slot].length = length; n->pending[slot].received_ms = now;
                n->pending[slot].used = true; ++n->stats.pending_senders;
            }
        }
        rns_hal_secure_zero(n->plain, sizeof(n->plain));
        return RNS_OK;
    }
    bool duplicate = false;
    for (size_t i = 0; i < REPLAYS; ++i)
        if (n->replay_used[i] && !memcmp(n->replay[i], message.message_id, 32)) duplicate = true;
    if (duplicate) ++n->stats.duplicates;
    else {
        if (n->accept) {
            rns_status_t accepted = n->accept(n->accept_context, &message, LXMF_SIGNATURE_VERIFIED, raw, length);
            if (accepted != RNS_OK) {
                rns_hal_secure_zero(n->plain, sizeof(n->plain));
                return accepted;
            }
        }
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
typedef struct {
    lxmf_identity_verifier_context_t verifier;
    lxmf_status_t status;
} archive_verifier_context;
void lxmf_packet_node_forget_pending(lxmf_packet_node_t *n, const uint8_t id[32]) {
    if(!n || !id) return;
    for(size_t i=0;i<PENDING;++i) if(n->pending[i].used && !memcmp(n->pending[i].id,id,32)) {
        rns_hal_secure_zero(&n->pending[i],sizeof(n->pending[i]));
        --n->stats.pending_senders;
    }
}
static lxmf_status_t archive_verify(void *context, const uint8_t source[16],
    const uint8_t *data, size_t length, const uint8_t signature[64]) {
    archive_verifier_context *v = context;
    v->status = lxmf_identity_verifier(&v->verifier, source, data, length, signature);
    /* Preserve verification separately while obtaining a fully parsed result. */
    return v->status == LXMF_ERR_SIGNATURE || v->status == LXMF_ERR_UNKNOWN_SIGNER ? LXMF_OK : v->status;
}
rns_status_t lxmf_packet_node_check_archive(lxmf_packet_node_t *n,
    const uint8_t *raw, size_t length, const uint8_t source[16],
    const uint8_t id[32], lxmf_signature_state_t *signature) {
    if (!n || !raw || !length || length > RNS_MTU || !source || !id || !signature)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (raw[0] & 0x80U) return RNS_ERROR_PROTOCOL;
    archive_verifier_context verifier = {{resolve, n}, LXMF_ERR_ARGUMENT};
    lxmf_message_t message; size_t plain_length = 0;
    lxmf_status_t status = lxmf_opportunistic_packet_unpack_ratchets(raw, length,
        &n->identity, n->ratchet_private, 1, 0, archive_verify, &verifier,
        n->plain, sizeof(n->plain), &plain_length, &message, NULL, NULL);
    rns_status_t result = RNS_ERROR_PROTOCOL;
    if (status == LXMF_OK &&
        !memcmp(source, message.source, 16) && !memcmp(id, message.message_id, 32)) {
        *signature = verifier.status == LXMF_OK ? LXMF_SIGNATURE_VERIFIED :
            verifier.status == LXMF_ERR_UNKNOWN_SIGNER ? LXMF_SIGNATURE_UNVERIFIED : LXMF_SIGNATURE_FAILED;
        result = RNS_OK;
    }
    rns_hal_secure_zero(n->plain, sizeof(n->plain));
    return result;
}

#define OUT_RECORD 680U
static void out_key(size_t slot,char key[8]) { (void)snprintf(key,8,"out%u",(unsigned)slot); }
static rns_status_t save_out(lxmf_packet_node_t *n,size_t i) {
    uint8_t wire[OUT_RECORD]={1}; char key[8];
    const lxmf_packet_outgoing *v=&n->outgoing[i].view;
    wire[1]=(uint8_t)v->state; wire[2]=(uint8_t)v->attempts; wire[3]=(uint8_t)v->error;
    memcpy(wire+4,v->destination,16); memcpy(wire+20,v->id,32);
    rns_identity_export_public(&n->outgoing[i].recipient,wire+52);
    wire[116]=(uint8_t)(n->outgoing[i].length>>8); wire[117]=(uint8_t)n->outgoing[i].length;
    wire[118]=(uint8_t)v->text_length;
    for(unsigned b=0;b<8;++b) wire[120+b]=(uint8_t)(v->timestamp>>(56U-8U*b));
    memcpy(wire+128,n->outgoing[i].raw,n->outgoing[i].length);
    memcpy(wire+628,v->text,v->text_length); memcpy(wire+660,n->address,16);
    out_key(i,key);
    rns_status_t result=rns_storage_write_atomic(n->outbox,key,wire,sizeof(wire));
    rns_hal_secure_zero(wire,sizeof(wire));
    if(result==RNS_OK) n->outgoing[i].dirty=false;
    return result;
}
rns_status_t lxmf_packet_node_open_outbox(lxmf_packet_node_t *n,rns_storage_t *storage) {
    if(!n || !storage) return RNS_ERROR_INVALID_ARGUMENT;
    if(n->outbox) return RNS_ERROR_INVALID_STATE;
    uint8_t wire[OUT_RECORD]; rns_status_t status=RNS_OK;
    for(size_t i=0;i<4;++i) {
        char key[8]; size_t length=0; out_key(i,key);
        status=rns_storage_read(storage,key,wire,sizeof(wire),&length);
        if(status==RNS_ERROR_NOT_FOUND) { status=RNS_OK; continue; }
        if(status!=RNS_OK) break;
        if(length!=sizeof(wire) || wire[0]!=1 || wire[1]<1 || wire[1]>6 ||
            wire[2]>3 || wire[3]>RNS_ERROR_OVERFLOW || !wire[118] || wire[118]>32 ||
            ((wire[1]==LXMF_PACKET_TRANSMITTING || wire[1]==LXMF_PACKET_AWAITING_PROOF || wire[1]==LXMF_PACKET_DELIVERED) && !wire[2]) ||
            memcmp(wire+660,n->address,16)) { status=RNS_ERROR_PROTOCOL; break; }
        size_t raw_length=(size_t)wire[116]*256U+wire[117];
        rns_packet packet; uint8_t address[16];
        if(!raw_length || raw_length>500 || !rns_packet_decode(&packet,wire+128,raw_length) ||
            packet.header_type || packet.packet_type || packet.context || packet.destination_type ||
            memcmp(packet.destination_hash,wire+4,16) ||
            !rns_identity_from_public(&n->outgoing[i].recipient,wire+52) ||
            !rns_destination_hash(&n->outgoing[i].recipient,"lxmf",aspects,1,address) ||
            memcmp(address,wire+4,16) || !rns_packet_hash(wire+128,raw_length,n->outgoing[i].hash)) {
            status=RNS_ERROR_PROTOCOL; break;
        }
        lxmf_packet_outgoing *v=&n->outgoing[i].view;
        v->state=(lxmf_packet_send_state)wire[1]; v->attempts=wire[2]; v->error=(rns_status_t)wire[3];
        memcpy(v->destination,wire+4,16); memcpy(v->id,wire+20,32);
        for(size_t j=0;j<i;++j) if(n->outgoing[j].used && !memcmp(n->outgoing[j].view.id,v->id,32)) status=RNS_ERROR_PROTOCOL;
        if(status!=RNS_OK) break;
        v->text_length=wire[118]; memcpy(v->text,wire+628,v->text_length);
        for(unsigned b=0;b<8;++b) v->timestamp=(v->timestamp<<8)|wire[120+b];
        if(v->timestamp>RNS_ANNOUNCE_MAX_TIMESTAMP) { status=RNS_ERROR_PROTOCOL; break; }
        n->outgoing[i].length=raw_length; memcpy(n->outgoing[i].raw,wire+128,raw_length);
        if(v->state<LXMF_PACKET_DELIVERED) {
            v->state=v->attempts<3?LXMF_PACKET_QUEUED:LXMF_PACKET_FAILED;
            if(v->attempts==3) v->error=RNS_ERROR_TIMEOUT;
        }
        n->outgoing[i].used=true; n->outgoing[i].dirty=true;
    }
    rns_hal_secure_zero(wire,sizeof(wire));
    if(status!=RNS_OK) { rns_hal_secure_zero(n->outgoing,sizeof(n->outgoing)); return status; }
    n->outbox=storage; return RNS_OK;
}
rns_status_t lxmf_packet_node_send(lxmf_packet_node_t *n,const uint8_t destination[16],
    const uint8_t *text,size_t length,uint64_t timestamp,uint8_t id[32]) {
    if(!n || !destination || !text || !length || length>32 || !id || timestamp>RNS_ANNOUNCE_MAX_TIMESTAMP)
        return RNS_ERROR_INVALID_ARGUMENT;
    if(!n->outbox) return RNS_ERROR_INVALID_STATE;
    size_t peer=PEERS,slot=4;
    for(size_t i=0;i<PEERS;++i) if(n->peers[i].used && !memcmp(n->peers[i].address,destination,16)) peer=i;
    if(peer==PEERS || !n->peers[peer].delivery) return RNS_ERROR_NOT_FOUND;
    if(!n->peers[peer].has_ratchet || n->peers[peer].requires_stamp) return RNS_ERROR_UNSUPPORTED;
    for(size_t i=0;i<4;++i) if(!n->outgoing[i].used) { slot=i; break; }
    if(slot==4) return RNS_ERROR_OVERFLOW;
    lxmf_message_t message={.timestamp=(double)timestamp,.content={text,length}}, parsed;
    memcpy(message.source,n->address,16); memcpy(message.destination,destination,16);
    uint8_t packed[500]; size_t packed_length;
    lxmf_status_t result=lxmf_pack(&message,lxmf_identity_signer,&n->identity,packed,sizeof(packed),&packed_length);
    if(result==LXMF_OK) result=lxmf_unpack(packed,packed_length,NULL,NULL,&parsed);
    if(result==LXMF_OK) memcpy(message.message_id,parsed.message_id,32);
    rns_hal_secure_zero(packed,sizeof(packed));
    if(result!=LXMF_OK) return RNS_ERROR_CRYPTO;
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && !memcmp(n->outgoing[i].view.id,message.message_id,32))
        return RNS_ERROR_INVALID_STATE;
    result=lxmf_opportunistic_packet_pack_ratchet(&message,&n->identity,&n->peers[peer].identity,
        n->peers[peer].ratchet,n->outgoing[slot].raw,500,&n->outgoing[slot].length);
    if(result!=LXMF_OK || !rns_packet_hash(n->outgoing[slot].raw,n->outgoing[slot].length,n->outgoing[slot].hash)) {
        rns_hal_secure_zero(&n->outgoing[slot],sizeof(n->outgoing[slot])); return RNS_ERROR_CRYPTO;
    }
    n->outgoing[slot].recipient=n->peers[peer].identity;
    lxmf_packet_outgoing *v=&n->outgoing[slot].view;
    memcpy(v->id,message.message_id,32); memcpy(v->destination,destination,16); memcpy(v->text,text,length);
    v->text_length=length; v->timestamp=timestamp; v->state=LXMF_PACKET_QUEUED;
    rns_status_t status=save_out(n,slot);
    if(status!=RNS_OK) { rns_hal_secure_zero(&n->outgoing[slot],sizeof(n->outgoing[slot])); return status; }
    n->outgoing[slot].used=true; memcpy(id,v->id,32); return RNS_OK;
}
static void retry_out(lxmf_packet_node_t *n,size_t i,rns_status_t status,uint64_t now) {
    n->outgoing[i].view.error=status;
    n->outgoing[i].view.state=n->outgoing[i].view.attempts<3?LXMF_PACKET_QUEUED:LXMF_PACKET_FAILED;
    n->outgoing[i].ready=now+5000U*n->outgoing[i].view.attempts;
    n->outgoing[i].dirty=true;
}
void lxmf_packet_node_poll(lxmf_packet_node_t *n,uint64_t now) {
    if(!n || !n->outbox) return;
    for(size_t i=0;i<4;++i) {
        if(!n->outgoing[i].used) continue;
        if(n->outgoing[i].dirty && save_out(n,i)!=RNS_OK) continue;
        if(n->outgoing[i].view.state==LXMF_PACKET_AWAITING_PROOF && now>=n->outgoing[i].deadline)
            retry_out(n,i,RNS_ERROR_TIMEOUT,now);
        if(n->outgoing[i].view.state!=LXMF_PACKET_QUEUED || now<n->outgoing[i].ready) continue;
        ++n->outgoing[i].view.attempts; n->outgoing[i].view.state=LXMF_PACKET_TRANSMITTING;
        n->outgoing[i].dirty=true;
        if(save_out(n,i)!=RNS_OK) {
            --n->outgoing[i].view.attempts; n->outgoing[i].view.state=LXMF_PACKET_QUEUED; continue;
        }
        rns_status_t status=rns_interface_send_with_id(n->interface_value,n->outgoing[i].raw,
            n->outgoing[i].length,&n->outgoing[i].transmission_id);
        if(status!=RNS_OK) retry_out(n,i,status,now);
    }
}
void lxmf_packet_node_tx_complete(lxmf_packet_node_t *n,uint32_t id,rns_status_t status,uint64_t now) {
    if(!n) return;
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used &&
        n->outgoing[i].view.state==LXMF_PACKET_TRANSMITTING && n->outgoing[i].transmission_id==id) {
        if(status!=RNS_OK) retry_out(n,i,status,now);
        else {
            n->outgoing[i].view.state=LXMF_PACKET_AWAITING_PROOF;
            n->outgoing[i].view.error=RNS_OK; n->outgoing[i].deadline=now+120000U;
            n->outgoing[i].dirty=true;
        }
    }
}
bool lxmf_packet_node_outgoing(const lxmf_packet_node_t *n,size_t i,lxmf_packet_outgoing *out) {
    if(!n || i>=4 || !out || !n->outgoing[i].used) return false;
    *out=n->outgoing[i].view; out->durable=!n->outgoing[i].dirty; return true;
}
rns_status_t lxmf_packet_node_cancel(lxmf_packet_node_t *n,const uint8_t id[32]) {
    if(!n || !id) return RNS_ERROR_INVALID_ARGUMENT;
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && !memcmp(id,n->outgoing[i].view.id,32)) {
        if(n->outgoing[i].view.state>=LXMF_PACKET_DELIVERED) return RNS_ERROR_INVALID_STATE;
        n->outgoing[i].view.state=LXMF_PACKET_CANCELLED; n->outgoing[i].dirty=true; return RNS_OK;
    }
    return RNS_ERROR_NOT_FOUND;
}
rns_status_t lxmf_packet_node_release(lxmf_packet_node_t *n,size_t i) {
    if(!n || i>=4) return RNS_ERROR_INVALID_ARGUMENT;
    if(!n->outgoing[i].used || n->outgoing[i].dirty || n->outgoing[i].view.state<LXMF_PACKET_DELIVERED)
        return RNS_ERROR_INVALID_STATE;
    char key[8]; out_key(i,key); rns_status_t status=rns_storage_remove(n->outbox,key);
    if(status==RNS_OK) rns_hal_secure_zero(&n->outgoing[i],sizeof(n->outgoing[i]));
    return status;
}
