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
#include "reticulum/embedded_link.h"
#include "reticulum/transport.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define PEERS 32U
#define REPLAYS 32U
#define PENDING 4U
struct lxmf_packet_node {
    rns_storage_t *storage;
    rns_interface_t *interface_value;
    rns_identity identity;
    rns_embedded_link_manager *links;
    uint64_t now_ms;
    uint64_t next_path_request, next_path_response;
    uint8_t path_destination[16];
    bool path_response_pending;
    uint8_t direct_archive[RNS_MTU];
    uint8_t ratchet_private[32], ratchet_public[32], address[16], name_hash[10];
    struct { bool used, delivery, has_ratchet, requires_stamp, metadata_valid, observed;
        uint8_t stamp_cost, address[16], ratchet[32]; rns_identity identity;
        uint64_t timestamp, delivery_timestamp; char display_name[128];
        uint8_t link_id[16]; bool unreachable; } peers[PEERS];
    rns_storage_t *peer_storage;
    lxmf_packet_peer_protected_fn protected_peer;
    void *peer_context;
    rns_status_t peer_storage_status;
    bool peer_blocked[PEERS], peer_dirty[PEERS], restoring;
    size_t restore_slot;
    size_t peer_write_cursor;
    uint64_t peer_write_after;
    struct { uint8_t raw[RNS_MTU+1U]; size_t length; } peer_records[PEERS];
    size_t next_replay;
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
    bool quarantined_outbox[4];
    struct {
        bool used, dirty, attempt_started, direct_sent, fallback_blocked, proof_pending;
        uint8_t link_id[16];
        lxmf_packet_outgoing view;
        rns_identity recipient;
        uint8_t raw[500], hash[32]; size_t length;
        uint32_t transmission_id;
        uint64_t deadline, ready;
    } outgoing[4];
};
static const char *aspects[] = {"delivery"};
static void retry_out(lxmf_packet_node_t *,size_t,rns_status_t,uint64_t);
static rns_status_t direct_data(void *,const uint8_t[16],const uint8_t *,size_t);
static void direct_state(void *,const uint8_t[16],rns_link_state,rns_status_t);
static void direct_proof(void *,const uint8_t[16],const uint8_t[32]);
static void direct_transmission(void *,const uint8_t[16],const uint8_t[32],rns_status_t);
static const uint8_t direct_magic[5]={0xff,'L','X','D',1};
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
        static const char *path_aspects[]={"path","request"};
        if(!rns_destination_hash(NULL,"rnstransport",path_aspects,2,n->path_destination)) status=RNS_ERROR_CRYPTO;
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
    if (n) { rns_embedded_link_destroy(n->links); rns_hal_secure_zero(n, sizeof(*n)); free(n); }
}
rns_status_t lxmf_packet_node_enable_links(lxmf_packet_node_t *n) {
    if(!n) return RNS_ERROR_INVALID_ARGUMENT;
    if(n->links) return RNS_OK;
    const rns_embedded_link_callbacks callbacks={.state=direct_state,.data=direct_data,
        .proof=direct_proof,.transmission=direct_transmission};
    return rns_embedded_link_create(&n->identity,n->address,n->interface_value,&callbacks,n,&n->links);
}
const uint8_t *lxmf_packet_node_address(const lxmf_packet_node_t *n) { return n ? n->address : NULL; }
void lxmf_packet_node_set_accept(lxmf_packet_node_t *n, lxmf_packet_accept_fn accept, void *context) {
    if (n) { n->accept = accept; n->accept_context = context; }
}
static rns_status_t announce_context(lxmf_packet_node_t *n, uint64_t timestamp,uint8_t context) {
    if (!n || timestamp > RNS_ANNOUNCE_MAX_TIMESTAMP) return RNS_ERROR_INVALID_ARGUMENT;
    if (n->last_announce == RNS_ANNOUNCE_MAX_TIMESTAMP) return RNS_ERROR_OVERFLOW;
    if (timestamp <= n->last_announce) timestamp = n->last_announce + 1U;
    uint8_t random[5], app[192]; size_t app_length, length;
    lxmf_announce_data_t data = {0};
    memcpy(data.display_name, "Heltec", 6); data.display_name_len = 6;
    rns_packet p = {.packet_type = 1,.context=context};
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
rns_status_t lxmf_packet_node_announce(lxmf_packet_node_t *n,uint64_t timestamp) {
    return announce_context(n,timestamp,0);
}
rns_status_t lxmf_packet_node_request_peer(lxmf_packet_node_t *n,const uint8_t destination[16],uint64_t now) {
    if(!n || !destination) return RNS_ERROR_INVALID_ARGUMENT;
    if(now<n->next_path_request) return RNS_ERROR_INVALID_STATE;
    uint8_t tag[16],body[48],raw[RNS_MTU]; size_t length=0,raw_length=0;
    if(rns_hal_random_bytes(tag,sizeof(tag))!=RNS_OK ||
        !rns_path_request_build(destination,NULL,tag,sizeof(tag),body,sizeof(body),&length)) return RNS_ERROR_CRYPTO;
    rns_packet request={.destination_type=2,.data=body,.data_length=length};
    memcpy(request.destination_hash,n->path_destination,16);
    if(!rns_packet_encode(&request,raw,sizeof(raw),&raw_length)) return RNS_ERROR_OVERFLOW;
    n->next_path_request=now+30000U;
    return rns_interface_send(n->interface_value,raw,raw_length);
}
static bool peer_protected(lxmf_packet_node_t *n,size_t slot) {
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used &&
        !memcmp(n->outgoing[i].view.destination,n->peers[slot].address,16)) return true;
    return n->protected_peer && n->protected_peer(n->peer_context,n->peers[slot].address);
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
    bool is_delivery=!memcmp(p->destination_hash,delivery,16);
    if(n->restoring) {
        if(slot!=PEERS || !is_delivery) return;
        slot=n->restore_slot;
    }
    if(slot==PEERS) {
        for(size_t i=0;i<PEERS;++i) if(!n->peer_blocked[i] && !n->peers[i].used) { slot=i; break; }
        if(slot==PEERS && is_delivery) for(size_t i=0;i<PEERS;++i)
            if(!n->peer_blocked[i] && !n->peer_dirty[i] && !peer_protected(n,i) &&
                (slot==PEERS || n->peers[i].timestamp<n->peers[slot].timestamp)) slot=i;
        if(slot==PEERS) return;
        memset(&n->peers[slot],0,sizeof(n->peers[slot]));
    }
    if(is_delivery && n->peers[slot].delivery) {
        if(a.timestamp<n->peers[slot].delivery_timestamp) return;
        rns_packet previous;
        if(n->peer_records[slot].length>1 &&
            rns_packet_decode(&previous,n->peer_records[slot].raw+1,n->peer_records[slot].length-1) &&
            previous.data_length==p->data_length && !memcmp(previous.data,p->data,p->data_length)) {
            if(!n->restoring) n->peers[slot].observed=true;
            return;
        }
    }
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
        n->peers[slot].metadata_valid=!a.app_data_length ||
            lxmf_announce_parse(a.app_data,a.app_data_length,&data)==LXMF_OK;
        n->peers[slot].stamp_cost=n->peers[slot].metadata_valid && data.has_stamp_cost ? data.stamp_cost : 0;
        n->peers[slot].requires_stamp=!n->peers[slot].metadata_valid || n->peers[slot].stamp_cost;
        memset(n->peers[slot].display_name,0,sizeof(n->peers[slot].display_name));
        if(n->peers[slot].metadata_valid) memcpy(n->peers[slot].display_name,data.display_name,data.display_name_len);
        n->peers[slot].observed=!n->restoring;
        if(!n->restoring) {
            size_t length=0; n->peer_records[slot].raw[0]=1;
            if(rns_packet_encode(p,n->peer_records[slot].raw+1,RNS_MTU,&length)) {
                n->peer_records[slot].length=length+1; n->peer_dirty[slot]=n->peer_storage!=NULL;
            }
        }
    }
    if(n->restoring) return;
    ++n->stats.learned_announces;
    for (size_t i = 0; i < PENDING; ++i) {
        if (!n->pending[i].used || memcmp(n->pending[i].source, delivery, 16)) continue;
        n->pending[i].used = false; --n->stats.pending_senders;
        (void)lxmf_packet_node_receive(n, n->pending[i].raw, n->pending[i].length);
        rns_hal_secure_zero(&n->pending[i], sizeof(n->pending[i]));
    }
}
rns_status_t lxmf_packet_node_open_peers(lxmf_packet_node_t *n,rns_storage_t *storage,
    lxmf_packet_peer_protected_fn protected_peer_fn,void *context) {
    if(!n || !storage) return RNS_ERROR_INVALID_ARGUMENT;
    if(n->peer_storage) return RNS_ERROR_INVALID_STATE;
    for(size_t i=0;i<PEERS;++i) if(n->peers[i].used) return RNS_ERROR_INVALID_STATE;
    n->peer_storage=storage; n->protected_peer=protected_peer_fn; n->peer_context=context;
    n->restoring=true;
    for(size_t i=0;i<PEERS;++i) {
        char key[8]; (void)snprintf(key,sizeof(key),"peer%02u",(unsigned)i);
        size_t length=0; rns_packet p;
        rns_status_t status=rns_storage_read(storage,key,n->peer_records[i].raw,sizeof(n->peer_records[i].raw),&length);
        if(status==RNS_ERROR_NOT_FOUND) continue;
        if(status==RNS_OK && length>1 && n->peer_records[i].raw[0]==1 &&
            rns_packet_decode(&p,n->peer_records[i].raw+1,length-1) && p.packet_type==1 &&
            !(n->peer_records[i].raw[1]&0x80U)) {
            n->restore_slot=i; learn(n,&p);
            if(n->peers[i].used) { n->peer_records[i].length=length; continue; }
        }
        n->peer_blocked[i]=true;
        n->peer_storage_status=status==RNS_OK?RNS_ERROR_PROTOCOL:status;
    }
    n->restoring=false;
    return RNS_OK;
}
rns_status_t lxmf_packet_node_peer_storage_status(const lxmf_packet_node_t *n) {
    return n?n->peer_storage_status:RNS_ERROR_INVALID_ARGUMENT;
}
rns_status_t lxmf_packet_node_receive(lxmf_packet_node_t *n, const uint8_t *raw, size_t length) {
    rns_packet p;
    if (!n) return RNS_ERROR_INVALID_ARGUMENT;
    uint64_t now = 0;
    bool clock_ok = rns_hal_monotonic_ms(&now) == RNS_OK;
    if(clock_ok) n->now_ms=now;
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
    if(p.packet_type==0 && p.destination_type==2 && p.context==0 &&
        !memcmp(p.destination_hash,n->path_destination,16)) {
        rns_path_request request;
        if(!rns_path_request_parse(&request,p.data,p.data_length)) return RNS_ERROR_PROTOCOL;
        if(!memcmp(request.destination_hash,n->address,16) && n->now_ms>=n->next_path_response)
            n->path_response_pending=true;
        return RNS_OK;
    }
    if(n->links) {
        rns_status_t handled=rns_embedded_link_receive(n->links,raw,length,n->now_ms);
        if(handled!=RNS_ERROR_NOT_FOUND) return handled;
    }
    if(p.packet_type==3 && p.header_type==0 && p.destination_type==0 && p.context==0) {
        for(size_t i=0;i<4;++i) if(n->outgoing[i].used && !n->outgoing[i].view.direct && n->outgoing[i].view.attempts &&
            (n->outgoing[i].view.state==LXMF_PACKET_TRANSMITTING||n->outgoing[i].view.state==LXMF_PACKET_AWAITING_PROOF) &&
            !memcmp(p.destination_hash,n->outgoing[i].hash,16) &&
            rns_proof_validate(&n->outgoing[i].recipient,n->outgoing[i].hash,p.data,p.data_length)) {
            if(n->outgoing[i].view.state==LXMF_PACKET_TRANSMITTING)n->outgoing[i].proof_pending=true;
            else {n->outgoing[i].view.state=LXMF_PACKET_DELIVERED;
                n->outgoing[i].view.error=RNS_OK; n->outgoing[i].dirty=true;}
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
typedef struct { lxmf_packet_node_t *node; rns_identity identity; bool identified; } direct_verifier;
static const rns_identity *direct_resolve(void *context,const uint8_t source[16]) {
    direct_verifier *v=context;
    const rns_identity *known=resolve(v->node,source);
    if(known) return known;
    uint8_t address[16];
    if(v->identified && rns_destination_hash(&v->identity,"lxmf",aspects,1,address) &&
        !memcmp(address,source,16)) return &v->identity;
    return NULL;
}
static rns_status_t direct_data(void *context,const uint8_t link_id[16],const uint8_t *data,size_t length) {
    lxmf_packet_node_t *n=context;
    if(length>sizeof(n->direct_archive)-sizeof(direct_magic)) return RNS_ERROR_OVERFLOW;
    direct_verifier keys={.node=n};
    keys.identified=rns_embedded_link_authenticated_peer(n->links,link_id,&keys.identity)==RNS_OK;
    archive_verifier_context verify={{direct_resolve,&keys},LXMF_ERR_ARGUMENT};
    lxmf_message_t message;
    lxmf_status_t status=lxmf_unpack(data,length,archive_verify,&verify,&message);
    n->stats.last_message_status=status;
    if(status!=LXMF_OK || memcmp(message.destination,n->address,16) ||
        !isfinite(message.timestamp) || message.timestamp<0 || message.timestamp>RNS_ANNOUNCE_MAX_TIMESTAMP ||
        verify.status==LXMF_ERR_SIGNATURE) { ++n->stats.rejected; return RNS_ERROR_PROTOCOL; }
    bool verified=verify.status==LXMF_OK;
    if(!verified && verify.status!=LXMF_ERR_UNKNOWN_SIGNER) return RNS_ERROR_CRYPTO;
    if(verified) for(size_t i=0;i<REPLAYS;++i) if(n->replay_used[i] &&
        !memcmp(n->replay[i],message.message_id,32)) { ++n->stats.duplicates; return RNS_OK; }
    memcpy(n->direct_archive,direct_magic,sizeof(direct_magic));
    memcpy(n->direct_archive+sizeof(direct_magic),data,length);
    rns_status_t accepted=n->accept?n->accept(n->accept_context,&message,
        verified?LXMF_SIGNATURE_VERIFIED:LXMF_SIGNATURE_UNVERIFIED,n->direct_archive,length+sizeof(direct_magic)):RNS_OK;
    rns_hal_secure_zero(n->direct_archive,sizeof(n->direct_archive));
    if(accepted!=RNS_OK) return accepted;
    if(!verified) { ++n->stats.unknown_senders; return RNS_ERROR_NOT_FOUND; }
    memcpy(n->replay[n->next_replay],message.message_id,32);
    n->replay_used[n->next_replay]=true; n->next_replay=(n->next_replay+1U)%REPLAYS;
    ++n->stats.messages;
    if(n->callback) n->callback(n->context,&message);
    return RNS_OK;
}
static void direct_state(void *context,const uint8_t link_id[16],rns_link_state state,rns_status_t status) {
    lxmf_packet_node_t *n=context;
    if(status==RNS_ERROR_CRYPTO||status==RNS_ERROR_PROTOCOL)
        for(size_t i=0;i<4;++i) if(n->outgoing[i].used&&n->outgoing[i].view.direct&&
            !memcmp(n->outgoing[i].link_id,link_id,16)) {
            n->outgoing[i].fallback_blocked=true;n->outgoing[i].view.error=status;n->outgoing[i].dirty=true;
        }
    if(state==RNS_LINK_ACTIVE) {
        rns_identity identity; uint8_t destination[16];
        if(rns_embedded_link_authenticated_peer(n->links,link_id,&identity)==RNS_OK &&
            rns_destination_hash(&identity,"lxmf",aspects,1,destination))
            for(size_t i=0;i<PEERS;++i) if(n->peers[i].used && !memcmp(n->peers[i].address,destination,16)) {
                memcpy(n->peers[i].link_id,link_id,16); n->peers[i].unreachable=false;
            }
    }
    if(state!=RNS_LINK_CLOSED) return;
    for(size_t i=0;i<PEERS;++i) if(n->peers[i].used && !memcmp(n->peers[i].link_id,link_id,16)) {
        memset(n->peers[i].link_id,0,16); n->peers[i].unreachable=status!=RNS_OK;
    }
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && n->outgoing[i].view.direct &&
        n->outgoing[i].view.state<LXMF_PACKET_DELIVERED && !memcmp(n->outgoing[i].link_id,link_id,16)) {
        memset(n->outgoing[i].link_id,0,16);
        retry_out(n,i,status==RNS_OK?RNS_ERROR_IO:status,n->now_ms);
    }
}
static void direct_proof(void *context,const uint8_t link_id[16],const uint8_t hash[32]) {
    lxmf_packet_node_t *n=context;
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && n->outgoing[i].view.direct &&
        n->outgoing[i].view.attempts && n->outgoing[i].view.state<LXMF_PACKET_DELIVERED &&
        !memcmp(n->outgoing[i].link_id,link_id,16) && !memcmp(n->outgoing[i].hash,hash,32)) {
        n->outgoing[i].view.state=LXMF_PACKET_DELIVERED;
        n->outgoing[i].view.error=RNS_OK; n->outgoing[i].dirty=true;
    }
}
static void direct_transmission(void *context,const uint8_t link_id[16],const uint8_t hash[32],rns_status_t status) {
    lxmf_packet_node_t *n=context;
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && n->outgoing[i].view.direct &&
        n->outgoing[i].view.state==LXMF_PACKET_TRANSMITTING &&
        !memcmp(n->outgoing[i].link_id,link_id,16) && !memcmp(n->outgoing[i].hash,hash,32)) {
        if(status!=RNS_OK) retry_out(n,i,status,n->now_ms);
        else {
            n->outgoing[i].view.state=LXMF_PACKET_AWAITING_PROOF;
            n->outgoing[i].deadline=n->now_ms+120000U; n->outgoing[i].dirty=true;
        }
    }
}
rns_status_t lxmf_packet_node_check_archive(lxmf_packet_node_t *n,
    const uint8_t *raw, size_t length, const uint8_t source[16],
    const uint8_t id[32], lxmf_signature_state_t *signature) {
    if (!n || !raw || !length || length > RNS_MTU || !source || !id || !signature)
        return RNS_ERROR_INVALID_ARGUMENT;
    bool direct=length>sizeof(direct_magic) && !memcmp(raw,direct_magic,sizeof(direct_magic));
    if (!direct && (raw[0] & 0x80U)) return RNS_ERROR_PROTOCOL;
    archive_verifier_context verifier = {{resolve, n}, LXMF_ERR_ARGUMENT};
    lxmf_message_t message; size_t plain_length = 0;
    lxmf_status_t status = direct?lxmf_unpack(raw+sizeof(direct_magic),length-sizeof(direct_magic),archive_verify,&verifier,&message):lxmf_opportunistic_packet_unpack_ratchets(raw, length,
        &n->identity, n->ratchet_private, 1, 0, archive_verify, &verifier,
        n->plain, sizeof(n->plain), &plain_length, &message, NULL, NULL);
    rns_status_t result = RNS_ERROR_PROTOCOL;
    if (status == LXMF_OK &&
        !memcmp(message.destination,n->address,16) &&
        !memcmp(source, message.source, 16) && !memcmp(id, message.message_id, 32)) {
        *signature = verifier.status == LXMF_OK ? LXMF_SIGNATURE_VERIFIED :
            verifier.status == LXMF_ERR_UNKNOWN_SIGNER ? LXMF_SIGNATURE_UNVERIFIED : LXMF_SIGNATURE_FAILED;
        result = RNS_OK;
    }
    rns_hal_secure_zero(n->plain, sizeof(n->plain));
    return result;
}
rns_status_t lxmf_packet_node_replay_archive(lxmf_packet_node_t *n,const uint8_t *raw,size_t length) {
    if(!n || !raw || !length || length>RNS_MTU) return RNS_ERROR_INVALID_ARGUMENT;
    if(length>sizeof(direct_magic) && !memcmp(raw,direct_magic,sizeof(direct_magic))) {
        static const uint8_t no_link[16]={0};
        return direct_data(n,no_link,raw+sizeof(direct_magic),length-sizeof(direct_magic));
    }
    return lxmf_packet_node_receive(n,raw,length);
}

#define OUT_RECORD 680U
static void out_key(size_t slot,char key[8]) { (void)snprintf(key,8,"out%u",(unsigned)slot); }
static rns_status_t save_out(lxmf_packet_node_t *n,size_t i) {
    uint8_t wire[OUT_RECORD]={1}; char key[8];
    const lxmf_packet_outgoing *v=&n->outgoing[i].view;
    wire[0]=v->direct?3:1;
    wire[119]=v->direct?(uint8_t)(1u|(n->outgoing[i].direct_sent?2u:0u)|(n->outgoing[i].fallback_blocked?4u:0u)):0;
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
static const rns_identity *local_resolve(void *context,const uint8_t source[16]) {
    lxmf_packet_node_t *n=context;
    return memcmp(source,n->address,16)?NULL:&n->identity;
}
rns_status_t lxmf_packet_node_open_outbox(lxmf_packet_node_t *n,rns_storage_t *storage) {
    if(!n || !storage) return RNS_ERROR_INVALID_ARGUMENT;
    if(n->outbox) return RNS_ERROR_INVALID_STATE;
    uint8_t wire[OUT_RECORD]; rns_status_t status=RNS_OK;
    for(size_t i=0;i<4;++i) {
        char key[8]; size_t length=0; out_key(i,key);
        status=rns_storage_read(storage,key,wire,sizeof(wire),&length);
        if(status==RNS_ERROR_NOT_FOUND) { status=RNS_OK; continue; }
        if(status==RNS_ERROR_QUARANTINED) { n->quarantined_outbox[i]=true; status=RNS_OK; continue; }
        if(status!=RNS_OK) break;
        if(length!=sizeof(wire) || (wire[0]!=1 && wire[0]!=2 && wire[0]!=3) ||
            (wire[0]==2 && wire[119]!=1) ||
            (wire[0]==3 && (!(wire[119]&1u)||(wire[119]&0xf8u))) || wire[1]<1 || wire[1]>6 ||
            wire[2]>3 || wire[3]>RNS_ERROR_QUARANTINED || !wire[118] || wire[118]>32 ||
            ((wire[1]==LXMF_PACKET_TRANSMITTING || wire[1]==LXMF_PACKET_AWAITING_PROOF || wire[1]==LXMF_PACKET_DELIVERED) && !wire[2]) ||
            memcmp(wire+660,n->address,16)) { status=RNS_ERROR_PROTOCOL; break; }
        size_t raw_length=(size_t)wire[116]*256U+wire[117];
        rns_packet packet; uint8_t address[16];
        bool direct=wire[0]>=2;
        if(!raw_length || raw_length>500 ||
            !rns_identity_from_public(&n->outgoing[i].recipient,wire+52) ||
            !rns_destination_hash(&n->outgoing[i].recipient,"lxmf",aspects,1,address) ||
            memcmp(address,wire+4,16)) {
            status=RNS_ERROR_PROTOCOL; break;
        }
        if(!direct && (!rns_packet_decode(&packet,wire+128,raw_length) ||
            packet.header_type || packet.packet_type || packet.context || packet.destination_type ||
            memcmp(packet.destination_hash,wire+4,16) || !rns_packet_hash(wire+128,raw_length,n->outgoing[i].hash))) {
            status=RNS_ERROR_PROTOCOL; break;
        }
        lxmf_packet_outgoing *v=&n->outgoing[i].view;
        v->state=(lxmf_packet_send_state)wire[1]; v->attempts=wire[2]; v->error=(rns_status_t)wire[3];
        v->direct=direct;
        n->outgoing[i].direct_sent=wire[0]==3&&(wire[119]&2u);
        /* An interrupted attempt may have seen an authentication error whose
         * journal write failed. Never downgrade an interrupted direct send. */
        n->outgoing[i].fallback_blocked=direct&&(wire[2]>0||(wire[0]==3&&(wire[119]&4u)));
        memcpy(v->destination,wire+4,16); memcpy(v->id,wire+20,32);
        for(size_t j=0;j<i;++j) if(n->outgoing[j].used && !memcmp(n->outgoing[j].view.id,v->id,32)) status=RNS_ERROR_PROTOCOL;
        if(status!=RNS_OK) break;
        v->text_length=wire[118]; memcpy(v->text,wire+628,v->text_length);
        for(unsigned b=0;b<8;++b) v->timestamp=(v->timestamp<<8)|wire[120+b];
        if(v->timestamp>RNS_ANNOUNCE_MAX_TIMESTAMP) { status=RNS_ERROR_PROTOCOL; break; }
        if(direct) {
            lxmf_identity_verifier_context_t verifier={local_resolve,n}; lxmf_message_t packed;
            if(lxmf_unpack(wire+128,raw_length,lxmf_identity_verifier,&verifier,&packed)!=LXMF_OK ||
                memcmp(packed.destination,v->destination,16) || memcmp(packed.message_id,v->id,32) ||
                packed.timestamp!=(double)v->timestamp || packed.content.len!=v->text_length ||
                memcmp(packed.content.data,v->text,v->text_length)) { status=RNS_ERROR_PROTOCOL; break; }
        }
        n->outgoing[i].length=raw_length; memcpy(n->outgoing[i].raw,wire+128,raw_length);
        if(v->state<LXMF_PACKET_DELIVERED) {
            v->state=v->attempts<3?LXMF_PACKET_QUEUED:LXMF_PACKET_FAILED;
            if(v->attempts==3) v->error=RNS_ERROR_TIMEOUT;
        }
        n->outgoing[i].used=true; n->outgoing[i].dirty=true;
    }
    rns_hal_secure_zero(wire,sizeof(wire));
    if(status!=RNS_OK) { rns_hal_secure_zero(n->outgoing,sizeof(n->outgoing)); memset(n->quarantined_outbox,0,sizeof(n->quarantined_outbox)); return status; }
    n->outbox=storage; return RNS_OK;
}
bool lxmf_packet_node_peer_info(const lxmf_packet_node_t *n,
    const uint8_t destination[16], lxmf_packet_peer_info *info) {
    if (!n || !destination || !info) return false;
    memset(info,0,sizeof(*info));
    for (size_t i=0;i<PEERS;++i) if(n->peers[i].used && !memcmp(n->peers[i].address,destination,16)) {
        info->delivery=n->peers[i].delivery; info->has_ratchet=n->peers[i].has_ratchet;
        info->metadata_valid=n->peers[i].metadata_valid; info->stamp_cost=n->peers[i].stamp_cost;
        memcpy(info->display_name,n->peers[i].display_name,sizeof(info->display_name));
        info->observed_this_boot=n->peers[i].observed;
        info->state=n->peers[i].unreachable?LXMF_PEER_UNREACHABLE:LXMF_PEER_KNOWN;
        rns_link_state state;
        if(n->links && rns_embedded_link_state(n->links,n->peers[i].link_id,&state)==RNS_OK)
            info->state=state==RNS_LINK_ACTIVE?LXMF_PEER_LINKED:LXMF_PEER_CONNECTING;
        return true;
    }
    return false;
}
bool lxmf_packet_node_peer_at(const lxmf_packet_node_t *n,size_t slot,uint8_t destination[16],lxmf_packet_peer_info *info) {
    if(!n || slot>=PEERS || !destination || !info || !n->peers[slot].used) return false;
    memcpy(destination,n->peers[slot].address,16);
    return lxmf_packet_node_peer_info(n,destination,info);
}
rns_status_t lxmf_packet_node_send(lxmf_packet_node_t *n,const uint8_t destination[16],
    const uint8_t *text,size_t length,uint64_t timestamp,uint8_t id[32]) {
    if(!n || !destination || !text || !length || length>32 || !id || timestamp>RNS_ANNOUNCE_MAX_TIMESTAMP)
        return RNS_ERROR_INVALID_ARGUMENT;
    if(!n->outbox) return RNS_ERROR_INVALID_STATE;
    size_t peer=PEERS,slot=4;
    for(size_t i=0;i<PEERS;++i) if(n->peers[i].used && !memcmp(n->peers[i].address,destination,16)) peer=i;
    if(peer==PEERS || !n->peers[peer].delivery) {
        (void)lxmf_packet_node_request_peer(n,destination,n->now_ms);
        return RNS_ERROR_NOT_FOUND;
    }
    if((!n->links && !n->peers[peer].has_ratchet) || n->peers[peer].requires_stamp) return RNS_ERROR_UNSUPPORTED;
    for(size_t i=0;i<4;++i) if(!n->quarantined_outbox[i] && !n->outgoing[i].used) { slot=i; break; }
    if(slot==4) return RNS_ERROR_OVERFLOW;
    lxmf_message_t message={.timestamp=(double)timestamp,.content={text,length}}, parsed;
    memcpy(message.source,n->address,16); memcpy(message.destination,destination,16);
    uint8_t packed[500]; size_t packed_length;
    lxmf_status_t result=lxmf_pack(&message,lxmf_identity_signer,&n->identity,packed,sizeof(packed),&packed_length);
    if(result==LXMF_OK) result=lxmf_unpack(packed,packed_length,NULL,NULL,&parsed);
    if(result==LXMF_OK) memcpy(message.message_id,parsed.message_id,32);
    if(result!=LXMF_OK) { rns_hal_secure_zero(packed,sizeof(packed)); return RNS_ERROR_CRYPTO; }
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && !memcmp(n->outgoing[i].view.id,message.message_id,32))
        { rns_hal_secure_zero(packed,sizeof(packed)); return RNS_ERROR_INVALID_STATE; }
    if(n->links) {
        memcpy(n->outgoing[slot].raw,packed,packed_length); n->outgoing[slot].length=packed_length;
    } else result=lxmf_opportunistic_packet_pack_ratchet(&message,&n->identity,&n->peers[peer].identity,
        n->peers[peer].ratchet,n->outgoing[slot].raw,500,&n->outgoing[slot].length);
    rns_hal_secure_zero(packed,sizeof(packed));
    if(result!=LXMF_OK || (!n->links && !rns_packet_hash(n->outgoing[slot].raw,n->outgoing[slot].length,n->outgoing[slot].hash))) {
        rns_hal_secure_zero(&n->outgoing[slot],sizeof(n->outgoing[slot])); return RNS_ERROR_CRYPTO;
    }
    n->outgoing[slot].recipient=n->peers[peer].identity;
    lxmf_packet_outgoing *v=&n->outgoing[slot].view;
    v->direct=n->links!=NULL;
    memcpy(v->id,message.message_id,32); memcpy(v->destination,destination,16); memcpy(v->text,text,length);
    v->text_length=length; v->timestamp=timestamp; v->state=LXMF_PACKET_QUEUED;
    rns_status_t status=save_out(n,slot);
    if(status!=RNS_OK) { rns_hal_secure_zero(&n->outgoing[slot],sizeof(n->outgoing[slot])); return status; }
    n->outgoing[slot].used=true; memcpy(id,v->id,32); return RNS_OK;
}
static void retry_out(lxmf_packet_node_t *n,size_t i,rns_status_t status,uint64_t now) {
    if(status==RNS_ERROR_CRYPTO||status==RNS_ERROR_PROTOCOL)n->outgoing[i].fallback_blocked=true;
    n->outgoing[i].attempt_started=false;
    n->outgoing[i].view.error=status;
    n->outgoing[i].view.state=n->outgoing[i].view.attempts<3?LXMF_PACKET_QUEUED:LXMF_PACKET_FAILED;
    n->outgoing[i].ready=now+5000U*n->outgoing[i].view.attempts;
    n->outgoing[i].dirty=true;
}
static bool handshake_fallback(lxmf_packet_node_t *n,size_t i) {
    lxmf_packet_outgoing *v=&n->outgoing[i].view;
    if(!v->direct||n->outgoing[i].direct_sent||n->outgoing[i].fallback_blocked||
       n->outgoing[i].attempt_started||!v->attempts||v->attempts>=3||
       (v->error!=RNS_ERROR_TIMEOUT&&v->error!=RNS_ERROR_IO))return false;
    size_t p=PEERS;
    for(size_t j=0;j<PEERS;++j)if(n->peers[j].used&&!memcmp(n->peers[j].address,v->destination,16)){p=j;break;}
    if(p==PEERS||!n->peers[p].delivery||!n->peers[p].metadata_valid||
       !n->peers[p].has_ratchet||n->peers[p].stamp_cost||n->peers[p].requires_stamp)return false;
    if(n->outgoing[i].length<16)return false;
    uint8_t encrypted[RNS_MTU],packet[RNS_MTU],hash[32];size_t encrypted_length=0,packet_length=0;
    /* Keep the original signed LXMF bytes; only replace their transport
     * representation. The outer destination supplies the omitted prefix. */
    bool ok=rns_identity_encrypt(&n->peers[p].identity,n->peers[p].ratchet,
        n->outgoing[i].raw+16,n->outgoing[i].length-16,encrypted,sizeof encrypted,&encrypted_length)!=0;
    rns_packet outer={.data=encrypted,.data_length=encrypted_length};memcpy(outer.destination_hash,v->destination,16);
    if(ok)ok=rns_packet_encode(&outer,packet,sizeof packet,&packet_length)&&rns_packet_hash(packet,packet_length,hash);
    rns_hal_secure_zero(encrypted,sizeof encrypted);
    if(!ok){rns_hal_secure_zero(packet,sizeof packet);return false;}
    memcpy(n->outgoing[i].raw,packet,packet_length);n->outgoing[i].length=packet_length;
    memcpy(n->outgoing[i].hash,hash,32);n->outgoing[i].recipient=n->peers[p].identity;
    rns_hal_secure_zero(packet,sizeof packet);memset(n->outgoing[i].link_id,0,16);
    v->direct=false;n->outgoing[i].dirty=true;
    return true;
}
void lxmf_packet_node_poll(lxmf_packet_node_t *n,uint64_t now) {
    lxmf_packet_node_poll_ready(n,now,0x0fU);
}
void lxmf_packet_node_poll_ready(lxmf_packet_node_t *n,uint64_t now,uint8_t ready_mask) {
    if(!n) return;
    n->now_ms=now;
    if(n->path_response_pending && now>=n->next_path_response) {
        n->path_response_pending=false; n->next_path_response=now+60000U;
        (void)announce_context(n,n->last_announce,0x0b);
    }
    if(n->links) rns_embedded_link_poll(n->links,now);
    if(n->peer_storage && now>=n->peer_write_after) for(size_t step=0;step<PEERS;++step) {
        size_t i=(n->peer_write_cursor+step)%PEERS;
        if(!n->peer_dirty[i]) continue;
        char key[8]; (void)snprintf(key,sizeof(key),"peer%02u",(unsigned)i);
        rns_status_t status=rns_storage_write_atomic(n->peer_storage,key,n->peer_records[i].raw,n->peer_records[i].length);
        if(status==RNS_OK) n->peer_dirty[i]=false;
        else n->peer_storage_status=status;
        n->peer_write_cursor=(i+1U)%PEERS;
        n->peer_write_after=now>UINT64_MAX-1000U?UINT64_MAX:now+1000U;
        break;
    }
    if(!n->outbox) return;
    for(size_t i=0;i<4;++i) {
        if(!n->outgoing[i].used) continue;
        if(n->outgoing[i].dirty && save_out(n,i)!=RNS_OK) continue;
        if(n->outgoing[i].view.state==LXMF_PACKET_AWAITING_PROOF && now>=n->outgoing[i].deadline)
            retry_out(n,i,RNS_ERROR_TIMEOUT,now);
        if(!(ready_mask & (1U<<i)) || n->outgoing[i].view.state!=LXMF_PACKET_QUEUED || now<n->outgoing[i].ready) continue;
        if(handshake_fallback(n,i)&&save_out(n,i)!=RNS_OK)continue;
        if(n->outgoing[i].view.direct) {
            if(!n->links) { n->outgoing[i].view.error=RNS_ERROR_UNSUPPORTED; n->outgoing[i].view.state=LXMF_PACKET_FAILED; n->outgoing[i].dirty=true; continue; }
            if(!n->outgoing[i].attempt_started) {
                ++n->outgoing[i].view.attempts; n->outgoing[i].dirty=true;
                if(save_out(n,i)!=RNS_OK) { --n->outgoing[i].view.attempts; continue; }
                n->outgoing[i].attempt_started=true;
                rns_status_t connected=rns_embedded_link_connect(n->links,n->outgoing[i].view.destination,
                    &n->outgoing[i].recipient,now,n->outgoing[i].link_id);
                if(connected!=RNS_OK) { retry_out(n,i,connected,now); continue; }
                for(size_t p=0;p<PEERS;++p) if(n->peers[p].used &&
                    !memcmp(n->peers[p].address,n->outgoing[i].view.destination,16))
                    memcpy(n->peers[p].link_id,n->outgoing[i].link_id,16);
            }
            rns_link_state state;
            if(rns_embedded_link_state(n->links,n->outgoing[i].link_id,&state)!=RNS_OK || state==RNS_LINK_CLOSED) {
                retry_out(n,i,RNS_ERROR_IO,now); continue;
            }
            if(state!=RNS_LINK_ACTIVE) continue;
            n->outgoing[i].direct_sent=true;
            n->outgoing[i].view.state=LXMF_PACKET_TRANSMITTING; n->outgoing[i].dirty=true;
            if(save_out(n,i)!=RNS_OK) { n->outgoing[i].view.state=LXMF_PACKET_QUEUED; continue; }
            rns_status_t sent=rns_embedded_link_send(n->links,n->outgoing[i].link_id,n->outgoing[i].raw,
                n->outgoing[i].length,now,n->outgoing[i].hash);
            if(sent!=RNS_OK) retry_out(n,i,sent,now);
            continue;
        }
        n->outgoing[i].proof_pending=false;
        ++n->outgoing[i].view.attempts; n->outgoing[i].view.state=LXMF_PACKET_TRANSMITTING;
        n->outgoing[i].dirty=true;
        if(save_out(n,i)!=RNS_OK) {
            --n->outgoing[i].view.attempts; n->outgoing[i].view.state=LXMF_PACKET_QUEUED; continue;
        }
        rns_status_t status=rns_interface_send_with_id(n->interface_value,n->outgoing[i].raw,
            n->outgoing[i].length,&n->outgoing[i].transmission_id);
        if(status!=RNS_OK) {n->outgoing[i].proof_pending=false;retry_out(n,i,status,now);}
    }
}
void lxmf_packet_node_tx_complete(lxmf_packet_node_t *n,uint32_t id,rns_status_t status,uint64_t now) {
    if(!n) return;
    n->now_ms=now;
    if(n->links) rns_embedded_link_tx_complete(n->links,id,status,now);
    for(size_t i=0;i<4;++i) if(n->outgoing[i].used && !n->outgoing[i].view.direct &&
        n->outgoing[i].view.state==LXMF_PACKET_TRANSMITTING && n->outgoing[i].transmission_id==id) {
        if(status!=RNS_OK) {n->outgoing[i].proof_pending=false;retry_out(n,i,status,now);}
        else {
            n->outgoing[i].view.state=n->outgoing[i].proof_pending?LXMF_PACKET_DELIVERED:LXMF_PACKET_AWAITING_PROOF;
            n->outgoing[i].proof_pending=false;
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
        n->outgoing[i].proof_pending=false;
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
