/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include "reticulum/proof.h"
#include "reticulum/hal.h"
#include <assert.h>
#include <string.h>
static uint8_t record[105], sent[500];
static size_t record_length, sent_length;
static unsigned messages;
static unsigned unverified_events, verified_events;
static bool reject_save;
static rns_status_t accept_message(void *context, const lxmf_message_t *m,
    lxmf_signature_state_t signature, const uint8_t *raw, size_t length) {
    (void)context; assert(raw && length && m->content.len == 5);
    if (signature == LXMF_SIGNATURE_UNVERIFIED) ++unverified_events;
    else { assert(signature == LXMF_SIGNATURE_VERIFIED); ++verified_events; }
    return reject_save ? RNS_ERROR_IO : RNS_OK;
}
static uint64_t now_ms;
static rns_status_t test_clock(void *context, uint64_t *now) { (void)context; *now = now_ms; return RNS_OK; }
static rns_status_t write_status = RNS_OK;
static uint8_t out_records[4][680]; static size_t out_lengths[4];
static bool quarantined[4];
static uint8_t peer_records[32][501]; static size_t peer_lengths[32];
static unsigned peer_writes;
static bool protect_peers;
static bool protect_peer(void *context,const uint8_t destination[16]) {
    (void)context; (void)destination; return protect_peers;
}
static uint32_t tx_id; static unsigned tracked_sends;
static rns_status_t read_record(void *c, const char *key, uint8_t *out, size_t cap, size_t *len) {
    (void)c;
    if(!strncmp(key,"peer",4)) {
        unsigned i=(unsigned)(key[4]-'0')*10U+(unsigned)(key[5]-'0'); assert(i<32);
        if(!peer_lengths[i]) return RNS_ERROR_NOT_FOUND;
        assert(cap>=peer_lengths[i]); memcpy(out,peer_records[i],peer_lengths[i]); *len=peer_lengths[i]; return RNS_OK;
    }
    if(!strncmp(key,"out",3)) {
        unsigned i=(unsigned)(key[3]-'0'); assert(i<4 && !key[4]);
        if(quarantined[i]) return RNS_ERROR_QUARANTINED;
        if(!out_lengths[i]) return RNS_ERROR_NOT_FOUND;
        assert(cap>=out_lengths[i]); memcpy(out,out_records[i],out_lengths[i]); *len=out_lengths[i]; return RNS_OK;
    }
    assert(!strcmp(key, "identity"));
    if (!record_length) return RNS_ERROR_NOT_FOUND;
    assert(cap >= record_length); memcpy(out, record, record_length); *len = record_length; return RNS_OK;
}
static rns_status_t write_record(void *c, const char *key, const uint8_t *data, size_t len) {
    (void)c; (void)key; if (write_status != RNS_OK) return write_status;
    if(!strncmp(key,"peer",4)) {
        unsigned i=(unsigned)(key[4]-'0')*10U+(unsigned)(key[5]-'0'); assert(i<32 && len<=501);
        memcpy(peer_records[i],data,len); peer_lengths[i]=len; ++peer_writes; return RNS_OK;
    }
    if(!strncmp(key,"out",3)) {
        unsigned i=(unsigned)(key[3]-'0'); assert(i<4 && len==680);
        assert(!quarantined[i]);
        memcpy(out_records[i],data,len); out_lengths[i]=len; return RNS_OK;
    }
    assert(len == sizeof(record)); memcpy(record, data, len); record_length = len; return RNS_OK;
}
static rns_status_t start(void *c) { (void)c; return RNS_OK; }
static rns_status_t remove_record(void *c, const char *key) {
    (void)c; if(write_status!=RNS_OK) return write_status;
    if(!strncmp(key,"out",3)) out_lengths[(unsigned)(key[3]-'0')]=0;
    return RNS_OK;
}
static void stop(void *c) { (void)c; }
static rns_status_t poll_interface(void *c, rns_interface_receive_fn fn, void *ctx, size_t budget) {
    (void)c; (void)fn; (void)ctx; (void)budget; return RNS_OK;
}
static rns_status_t send_packet(void *c, const uint8_t *data, size_t len) {
    (void)c; assert(len <= sizeof(sent)); memcpy(sent, data, len); sent_length = len; return RNS_OK;
}
static rns_status_t tracked_send(void *c,const uint8_t *data,size_t len,uint32_t *id) {
    ++tracked_sends; *id=++tx_id; return send_packet(c,data,len);
}
static rns_status_t stats(void *c, rns_interface_stats_t *s) {
    (void)c; memset(s, 0, sizeof(*s)); s->online = 1; s->outbound = 1; s->effective_mtu = 500; return RNS_OK;
}
static void message(void *c, const lxmf_message_t *m) {
    (void)c; assert(m->content.len == 5 && !memcmp(m->content.data, "hello", 5)); ++messages;
}
static void set_message_id(lxmf_message_t *m,rns_identity *sender) {
    uint8_t packed[500]; size_t length; lxmf_message_t parsed;
    assert(lxmf_pack(m,lxmf_identity_signer,sender,packed,sizeof(packed),&length)==LXMF_OK);
    assert(lxmf_unpack(packed,length,NULL,NULL,&parsed)==LXMF_OK);
    memcpy(m->message_id,parsed.message_id,32);
}
static const rns_identity *resolve_reply_sender(void *context,const uint8_t source[16]) {
    const char *parts[]={"delivery"}; uint8_t expected[16];
    assert(rns_destination_hash(context,"lxmf",parts,1,expected));
    return memcmp(expected,source,16)?NULL:context;
}
int main(void) {
    rns_platform_ops_t platform = *rns_platform_current();
    platform.monotonic_ms = test_clock;
    assert(rns_platform_install(&platform) == RNS_OK);
    rns_storage_t *storage;
    rns_interface_t *radio;
    const rns_storage_ops_t storage_ops = {.read = read_record, .write_atomic = write_record, .remove = remove_record};
    const rns_interface_ops_t radio_ops = {.start = start, .stop = stop,
        .poll = poll_interface, .send = send_packet, .get_stats = stats, .destroy = stop,.send_with_id=tracked_send};
    assert(rns_storage_create(&storage_ops, NULL, &storage) == RNS_OK);
    assert(rns_interface_create(&radio_ops, NULL, &radio) == RNS_OK);
    assert(rns_interface_start(radio) == RNS_OK);
    lxmf_packet_node_t *n = NULL;
    write_status = RNS_ERROR_IO;
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_ERROR_IO && n == NULL);
    write_status = RNS_OK;
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_OK);
    uint8_t address[16]; memcpy(address, lxmf_packet_node_address(n), 16);
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_OK);
    assert(!memcmp(address, lxmf_packet_node_address(n), 16));
    assert(lxmf_packet_node_announce(n, 1234) == RNS_OK);
    rns_packet announce; rns_announce a;
    assert(rns_packet_decode(&announce, sent, sent_length));
    assert(rns_announce_verify(address, announce.data, announce.data_length, announce.context_flag));
    assert(rns_announce_parse(&a, announce.data, announce.data_length, announce.context_flag));
    assert(a.has_ratchet);
    assert(a.timestamp == 1234);
    rns_identity local, sender;
    uint8_t public_key[64], ratchet[32];
    memcpy(public_key, a.public_key, 64); memcpy(ratchet, a.ratchet, 32);
    assert(rns_identity_from_public(&local, public_key));
    assert(rns_identity_generate(&sender));
    const char *aspects[] = {"delivery"};
    lxmf_message_t m = {.timestamp = 1234, .content = {(const uint8_t *)"hello", 5}};
    memcpy(m.destination, address, 16);
    assert(rns_destination_hash(&sender, "lxmf", aspects, 1, m.source));
    uint8_t wire[500]; size_t wire_length;
    assert(lxmf_opportunistic_packet_pack_ratchet(&m, &sender, &local, ratchet,
        wire, sizeof(wire), &wire_length) == LXMF_OK);
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 0);
    lxmf_packet_node_stats_t info; lxmf_packet_node_stats(n, &info);
    assert(info.unknown_senders == 1 && info.proofs_queued == 0);
    assert(info.pending_senders == 1);
    lxmf_signature_state_t signature = LXMF_SIGNATURE_FAILED;
    set_message_id(&m,&sender);
    assert(lxmf_packet_node_check_archive(n, wire, wire_length, m.source, m.message_id, &signature) == RNS_OK);
    assert(signature == LXMF_SIGNATURE_UNVERIFIED);
    uint8_t body[465], name[10], prefix[5] = {0}, ann[500]; size_t ann_length;
    rns_packet p = {.packet_type = 1, .data = body};
    const char *node_aspects[] = {"node"};
    assert(rns_destination_hash(&sender, "nomadnetwork", node_aspects, 1, p.destination_hash));
    assert(rns_destination_name_hash("nomadnetwork", node_aspects, 1, name));
    assert(rns_announce_build(&sender, p.destination_hash, name, prefix, 1234, NULL, NULL, 0,
        body, sizeof(body), &p.data_length, &p.context_flag));
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_OK);
    assert(messages == 1);
    lxmf_packet_node_stats(n, &info); assert(info.pending_senders == 0 && info.proofs_queued == 1);
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 1);
    rns_packet proof; uint8_t hash[32];
    assert(rns_packet_decode(&proof, sent, sent_length) && proof.packet_type == 3);
    assert(rns_packet_hash(wire, wire_length, hash));
    assert(!memcmp(proof.destination_hash, hash, 16));
    assert(rns_proof_validate(&local, hash, proof.data, proof.data_length));
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 1);
    lxmf_packet_node_stats(n, &info); assert(info.duplicates == 2 && info.proofs_queued == 3);
    wire[wire_length - 1] ^= 1;
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 1);
    lxmf_packet_node_stats(n, &info); assert(info.rejected == 2 && info.proofs_queued == 3);
    assert(info.ingress == 6 && info.announces == 1 && info.learned_announces == 1);
    assert(info.packet_types[0] == 5 && info.local_data == 5);
    assert(lxmf_packet_node_receive(n, NULL, 0) == RNS_ERROR_PROTOCOL);
    uint8_t invalid[] = {0x80};
    assert(lxmf_packet_node_receive(n, invalid, sizeof(invalid)) == RNS_ERROR_PROTOCOL);
    invalid[0] = 0;
    assert(lxmf_packet_node_receive(n, invalid, sizeof(invalid)) == RNS_ERROR_PROTOCOL);
    p.packet_type = 2;
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_OK);
    memcpy(p.destination_hash, address, 16);
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_ERROR_UNSUPPORTED);
    p.packet_type = 0; p.context = 1;
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_OK);
    p.packet_type = 3;
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_OK);
    lxmf_packet_node_stats(n, &info);
    assert(info.ingress == 13 && info.malformed == 2 && info.ifac_rejected == 1);
    assert(info.other_destinations == 1 && info.unsupported_packets == 1);
    assert(info.unsupported_data_layout == 1 && info.local_other == 1);
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_OK);
    assert(lxmf_packet_node_announce(n, 1) == RNS_OK);
    assert(rns_packet_decode(&announce, sent, sent_length));
    assert(rns_announce_parse(&a, announce.data, announce.data_length, announce.context_flag));
    assert(a.timestamp == 1235);
    write_status = RNS_ERROR_IO;
    assert(lxmf_packet_node_announce(n, 1236) == RNS_ERROR_IO);
    write_status = RNS_OK;
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_OK);
    for (unsigned i = 0; i < 5; ++i) {
        m.timestamp = 2000 + i;
        assert(lxmf_opportunistic_packet_pack_ratchet(&m, &sender, &local, ratchet,
            wire, sizeof(wire), &wire_length) == LXMF_OK);
        assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK);
    }
    lxmf_packet_node_stats(n, &info);
    assert(info.pending_senders == 4 && info.proofs_queued == 0);
    now_ms = 300000;
    assert(lxmf_packet_node_receive(n, NULL, 0) == RNS_ERROR_PROTOCOL);
    lxmf_packet_node_stats(n, &info);
    assert(info.pending_senders == 0 && info.expired_pending == 4);
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage, radio, NULL, NULL, &n) == RNS_OK);
    lxmf_packet_node_set_accept(n, accept_message, NULL);
    reject_save = true;
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK);
    assert(unverified_events == 1 && verified_events == 0);
    lxmf_packet_node_stats(n, &info); assert(info.proofs_queued == 0 && info.pending_senders == 1);
    p.packet_type = 1; p.context = 0;
    assert(rns_destination_hash(&sender, "nomadnetwork", node_aspects, 1, p.destination_hash));
    assert(rns_announce_build(&sender, p.destination_hash, name, prefix, 3000, NULL, NULL, 0,
        body, sizeof(body), &p.data_length, &p.context_flag));
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    reject_save = true;
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_OK);
    lxmf_packet_node_stats(n, &info); assert(info.proofs_queued == 0 && info.messages == 0);
    reject_save = false;
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK);
    lxmf_packet_node_stats(n, &info); assert(info.proofs_queued == 1 && info.messages == 1);
    assert(verified_events == 2);
    set_message_id(&m,&sender);
    assert(lxmf_packet_node_check_archive(n, wire, wire_length, m.source, m.message_id, &signature) == RNS_OK);
    assert(signature == LXMF_SIGNATURE_VERIFIED);
    uint8_t wrong_id[32] = {0};
    assert(lxmf_packet_node_check_archive(n, wire, wire_length, m.source, wrong_id, &signature) == RNS_ERROR_PROTOCOL);
    assert(signature == LXMF_SIGNATURE_VERIFIED);
    lxmf_packet_node_stats(n, &info); assert(info.proofs_queued == 1 && info.messages == 1);
    uint8_t forged[500], encrypted[500], bad_packet[500]; size_t forged_len, encrypted_len, bad_len;
    assert(lxmf_pack(&m,lxmf_identity_signer,&sender,forged,sizeof(forged),&forged_len)==LXMF_OK);
    forged[32]^=1; /* Well-formed, decryptable message with an invalid signature. */
    assert(rns_identity_encrypt(&local,ratchet,forged+16,forged_len-16,encrypted,sizeof(encrypted),&encrypted_len));
    rns_packet bad={.data=encrypted,.data_length=encrypted_len};
    memcpy(bad.destination_hash,address,16);
    assert(rns_packet_encode(&bad,bad_packet,sizeof(bad_packet),&bad_len));
    assert(lxmf_packet_node_check_archive(n,bad_packet,bad_len,m.source,m.message_id,&signature)==RNS_OK);
    assert(signature==LXMF_SIGNATURE_FAILED);
    lxmf_packet_node_stats(n,&info); assert(info.proofs_queued==1 && info.messages==1);
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_receive(n,wire,wire_length)==RNS_OK);
    lxmf_packet_node_stats(n,&info); assert(info.pending_senders==1);
    lxmf_packet_node_forget_pending(n,m.message_id);
    lxmf_packet_node_stats(n,&info); assert(info.pending_senders==0);
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    lxmf_packet_node_stats(n,&info); assert(info.messages==0 && info.proofs_queued==0);
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_OK);
    uint8_t reply_id[32], recipient_private[32], recipient_ratchet[32], ratchet_id[16];
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,4000,reply_id)==RNS_ERROR_NOT_FOUND);
    assert(rns_identity_ratchet_generate(recipient_private,recipient_ratchet,ratchet_id));
    memcpy(p.destination_hash,m.source,16);
    assert(rns_destination_name_hash("lxmf",aspects,1,name));
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,4000,recipient_ratchet,NULL,0,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    write_status=RNS_ERROR_IO;
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,4000,reply_id)==RNS_ERROR_IO);
    assert(tracked_sends==0); write_status=RNS_OK;
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,4000,reply_id)==RNS_OK);
    lxmf_packet_outgoing outgoing;
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.durable && outgoing.attempts==0);
    lxmf_packet_node_poll(n,0); assert(tracked_sends==1);
    lxmf_packet_node_poll(n,900000);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_TRANSMITTING && outgoing.attempts==1);
    uint8_t plain[500]; size_t plain_length; lxmf_message_t reply;
    lxmf_identity_verifier_context_t reply_verifier={resolve_reply_sender,&local};
    assert(lxmf_opportunistic_packet_unpack_ratchets(sent,sent_length,&sender,recipient_private,1,1,lxmf_identity_verifier,&reply_verifier,
        plain,sizeof(plain),&plain_length,&reply,NULL,NULL)==LXMF_OK);
    assert(reply.content.len==3 && !memcmp(reply.content.data,"Yes",3) && !memcmp(reply.message_id,reply_id,32));
    assert(rns_packet_hash(sent,sent_length,hash));
    lxmf_packet_node_tx_complete(n,tx_id,RNS_OK,900000);
    lxmf_packet_node_poll(n,900001);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_AWAITING_PROOF);
    rns_packet proof_packet={.packet_type=3,.data=body,.data_length=96};
    memcpy(proof_packet.destination_hash,hash,16);
    assert(rns_proof_generate_explicit(&sender,hash,body)); body[95]^=1;
    assert(rns_packet_encode(&proof_packet,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_AWAITING_PROOF);
    body[95]^=1;
    assert(rns_packet_encode(&proof_packet,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_DELIVERED && !outgoing.durable);
    lxmf_packet_node_poll(n,900002);
    assert(lxmf_packet_node_release(n,0)==RNS_OK);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"No",2,4001,reply_id)==RNS_OK);
    lxmf_packet_node_poll(n,1000000);
    uint8_t original[500]; size_t original_length=sent_length; memcpy(original,sent,sent_length);
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_OK);
    lxmf_packet_node_poll(n,0);
    assert(sent_length==original_length && !memcmp(sent,original,sent_length));
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.attempts==2);
    lxmf_packet_node_tx_complete(n,tx_id,RNS_OK,10); lxmf_packet_node_poll(n,120010);
    lxmf_packet_node_poll(n,130010);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.attempts==3);
    lxmf_packet_node_tx_complete(n,tx_id,RNS_ERROR_IO,130011); lxmf_packet_node_poll(n,130012);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_FAILED && outgoing.durable);
    assert(lxmf_packet_node_release(n,0)==RNS_OK);
    lxmf_announce_data_t capabilities={.has_stamp_cost=true,.stamp_cost=5};
    uint8_t app_data[128]; size_t app_length;
    assert(lxmf_announce_encode(&capabilities,app_data,sizeof(app_data),&app_length)==LXMF_OK);
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,5000,recipient_ratchet,app_data,app_length,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,5000,reply_id)==RNS_ERROR_UNSUPPORTED);
    lxmf_packet_peer_info peer_info;
    assert(lxmf_packet_node_peer_info(n,m.source,&peer_info));
    assert(peer_info.delivery && peer_info.has_ratchet && peer_info.metadata_valid && peer_info.stamp_cost==5);
    const uint8_t invalid_metadata[]={0x91};
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,5000,recipient_ratchet,invalid_metadata,sizeof(invalid_metadata),
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_peer_info(n,m.source,&peer_info) && !peer_info.metadata_valid && !peer_info.stamp_cost);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,5000,reply_id)==RNS_ERROR_UNSUPPORTED);
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,5000,NULL,NULL,0,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_peer_info(n,m.source,&peer_info) && peer_info.metadata_valid && !peer_info.has_ratchet);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,5000,reply_id)==RNS_ERROR_UNSUPPORTED);
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,5001,recipient_ratchet,NULL,0,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"No",2,5001,reply_id)==RNS_OK);
    lxmf_packet_node_poll(n,0);
    assert(rns_packet_hash(sent,sent_length,hash));
    lxmf_packet_node_tx_complete(n,tx_id,RNS_OK,1);
    proof_packet.data_length=64; memcpy(proof_packet.destination_hash,hash,16);
    assert(rns_proof_generate_implicit(&sender,hash,body));
    assert(rns_packet_encode(&proof_packet,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    write_status=RNS_ERROR_IO; lxmf_packet_node_poll(n,2);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && !outgoing.durable && outgoing.state==LXMF_PACKET_DELIVERED);
    assert(lxmf_packet_node_release(n,0)==RNS_ERROR_INVALID_STATE);
    write_status=RNS_OK; lxmf_packet_node_poll(n,3);
    assert(lxmf_packet_node_release(n,0)==RNS_OK);
    for(unsigned i=0;i<4;++i)
        assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,5002+i,reply_id)==RNS_OK);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,5006,reply_id)==RNS_ERROR_OVERFLOW);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing));
    assert(lxmf_packet_node_cancel(n,outgoing.id)==RNS_OK);
    unsigned before=tracked_sends;
    lxmf_packet_node_poll_ready(n,1,4U); assert(tracked_sends==before+1);
    assert(lxmf_packet_node_outgoing(n,1,&outgoing) && outgoing.state==LXMF_PACKET_QUEUED && outgoing.attempts==0);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_CANCELLED && outgoing.durable);
    lxmf_packet_node_poll_ready(n,2,0U); assert(tracked_sends==before+1);
    lxmf_packet_node_poll(n,3); assert(tracked_sends==before+3);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_CANCELLED && outgoing.durable);
    lxmf_packet_node_tx_complete(n,0,RNS_OK,2);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_CANCELLED);
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_OK);
    before=tracked_sends;
    lxmf_packet_node_poll(n,0);
    assert(tracked_sends==before+3);
    assert(lxmf_packet_node_outgoing(n,0,&outgoing) && outgoing.state==LXMF_PACKET_CANCELLED);
    lxmf_packet_node_destroy(n);
    quarantined[0]=true;
    uint8_t damaged_copy[680]; memcpy(damaged_copy,out_records[0],sizeof(damaged_copy));
    out_records[1][3]=RNS_ERROR_QUARANTINED;
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_OK);
    assert(!lxmf_packet_node_outgoing(n,0,&outgoing));
    assert(lxmf_packet_node_outgoing(n,1,&outgoing));
    assert(lxmf_packet_node_cancel(n,outgoing.id)==RNS_OK);
    lxmf_packet_node_poll(n,0);
    assert(lxmf_packet_node_release(n,1)==RNS_OK);
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,6000,recipient_ratchet,NULL,0,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,6000,reply_id)==RNS_OK);
    assert(lxmf_packet_node_outgoing(n,1,&outgoing) && !lxmf_packet_node_outgoing(n,0,&outgoing));
    assert(!memcmp(damaged_copy,out_records[0],sizeof(damaged_copy)));
    lxmf_packet_node_destroy(n); quarantined[0]=false;
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    out_records[1][0]=2;
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_ERROR_PROTOCOL);
    lxmf_packet_node_destroy(n);
    /* Reboot restores signed peer capabilities without a new RF announce. */
    memset(out_lengths,0,sizeof(out_lengths));
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_peers(n,storage,NULL,NULL)==RNS_OK);
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_OK);
    lxmf_announce_data_t named={0}; memcpy(named.display_name,"Alice",5); named.display_name_len=5;
    assert(lxmf_announce_encode(&named,app_data,sizeof(app_data),&app_length)==LXMF_OK);
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,7000,recipient_ratchet,app_data,app_length,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    lxmf_packet_node_poll(n,0); assert(peer_writes==1);
    lxmf_packet_peer_info saved_peer;
    assert(lxmf_packet_node_peer_info(n,m.source,&saved_peer) && saved_peer.observed_this_boot);
    assert(!strcmp(saved_peer.display_name,"Alice"));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    lxmf_packet_node_poll(n,1000); assert(peer_writes==1);
    assert(rns_announce_build(&sender,p.destination_hash,name,prefix,6999,NULL,NULL,0,
        body,sizeof(body),&p.data_length,&p.context_flag));
    assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    lxmf_packet_node_poll(n,2000); assert(peer_writes==1);
    assert(lxmf_packet_node_peer_info(n,m.source,&saved_peer) && saved_peer.has_ratchet && !strcmp(saved_peer.display_name,"Alice"));
    lxmf_packet_node_destroy(n);
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_peers(n,storage,NULL,NULL)==RNS_OK);
    assert(lxmf_packet_node_peer_info(n,m.source,&saved_peer) && !saved_peer.observed_this_boot);
    assert(saved_peer.has_ratchet && !strcmp(saved_peer.display_name,"Alice"));
    assert(lxmf_packet_node_open_outbox(n,storage)==RNS_OK);
    assert(lxmf_packet_node_send(n,m.source,(const uint8_t *)"Yes",3,7001,reply_id)==RNS_OK);
    before=tracked_sends; lxmf_packet_node_poll(n,0); assert(tracked_sends==before+1);
    lxmf_packet_node_destroy(n);
    peer_records[0][peer_lengths[0]-1]^=1;
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_peers(n,storage,NULL,NULL)==RNS_OK);
    assert(lxmf_packet_node_peer_storage_status(n)==RNS_ERROR_PROTOCOL);
    assert(!lxmf_packet_node_peer_info(n,m.source,&saved_peer));
    lxmf_packet_node_poll(n,1000); assert(peer_writes==1);
    lxmf_packet_node_destroy(n);
    memset(peer_lengths,0,sizeof(peer_lengths));
    assert(lxmf_packet_node_create(storage,radio,NULL,NULL,&n)==RNS_OK);
    assert(lxmf_packet_node_open_peers(n,storage,protect_peer,NULL)==RNS_OK);
    uint8_t first_peer[16];
    unsigned writes_before=peer_writes;
    for(unsigned i=0;i<33;++i) {
        rns_identity other; assert(rns_identity_generate(&other));
        assert(rns_destination_hash(&other,"lxmf",aspects,1,p.destination_hash));
        if(!i) memcpy(first_peer,p.destination_hash,16);
        assert(rns_announce_build(&other,p.destination_hash,name,prefix,8000+i,NULL,NULL,0,
            body,sizeof(body),&p.data_length,&p.context_flag));
        assert(rns_packet_encode(&p,ann,sizeof(ann),&ann_length));
        protect_peers=i==32;
        assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
        lxmf_packet_node_poll(n,(uint64_t)i*1000U);
    }
    assert(peer_writes==writes_before+32);
    assert(lxmf_packet_node_peer_info(n,first_peer,&saved_peer));
    assert(!lxmf_packet_node_peer_info(n,p.destination_hash,&saved_peer));
    protect_peers=false;
    write_status=RNS_ERROR_IO;
    assert(lxmf_packet_node_receive(n,ann,ann_length)==RNS_OK);
    lxmf_packet_node_poll(n,33000);
    assert(lxmf_packet_node_peer_storage_status(n)==RNS_ERROR_IO);
    assert(lxmf_packet_node_peer_info(n,p.destination_hash,&saved_peer));
    write_status=RNS_OK;
    lxmf_packet_node_poll(n,33001); assert(peer_writes==writes_before+32);
    lxmf_packet_node_poll(n,34000); assert(peer_writes==writes_before+33);
    assert(!lxmf_packet_node_peer_info(n,first_peer,&saved_peer));
    lxmf_packet_node_destroy(n);
    record[0] = 2;
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_ERROR_PROTOCOL && n == NULL);
    rns_interface_destroy(radio); rns_storage_destroy(storage);
    rns_platform_restore_default();
    return 0;
}
