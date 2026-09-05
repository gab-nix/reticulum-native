/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/lxmf_packet_node.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include "reticulum/proof.h"
#include <assert.h>
#include <string.h>
static uint8_t record[105], sent[500];
static size_t record_length, sent_length;
static unsigned messages;
static rns_status_t write_status = RNS_OK;
static rns_status_t read_record(void *c, const char *key, uint8_t *out, size_t cap, size_t *len) {
    (void)c; assert(!strcmp(key, "identity"));
    if (!record_length) return RNS_ERROR_NOT_FOUND;
    assert(cap >= record_length); memcpy(out, record, record_length); *len = record_length; return RNS_OK;
}
static rns_status_t write_record(void *c, const char *key, const uint8_t *data, size_t len) {
    (void)c; (void)key; if (write_status != RNS_OK) return write_status;
    assert(len == sizeof(record)); memcpy(record, data, len); record_length = len; return RNS_OK;
}
static rns_status_t start(void *c) { (void)c; return RNS_OK; }
static rns_status_t remove_record(void *c, const char *key) { (void)c; (void)key; return RNS_OK; }
static void stop(void *c) { (void)c; }
static rns_status_t poll_interface(void *c, rns_interface_receive_fn fn, void *ctx, size_t budget) {
    (void)c; (void)fn; (void)ctx; (void)budget; return RNS_OK;
}
static rns_status_t send_packet(void *c, const uint8_t *data, size_t len) {
    (void)c; assert(len <= sizeof(sent)); memcpy(sent, data, len); sent_length = len; return RNS_OK;
}
static rns_status_t stats(void *c, rns_interface_stats_t *s) {
    (void)c; memset(s, 0, sizeof(*s)); s->online = 1; s->outbound = 1; s->effective_mtu = 500; return RNS_OK;
}
static void message(void *c, const lxmf_message_t *m) {
    (void)c; assert(m->content.len == 5 && !memcmp(m->content.data, "hello", 5)); ++messages;
}
int main(void) {
    rns_storage_t *storage;
    rns_interface_t *radio;
    const rns_storage_ops_t storage_ops = {.read = read_record, .write_atomic = write_record, .remove = remove_record};
    const rns_interface_ops_t radio_ops = {.start = start, .stop = stop,
        .poll = poll_interface, .send = send_packet, .get_stats = stats, .destroy = stop};
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
    uint8_t body[465], name[10], prefix[5] = {0}, ann[500]; size_t ann_length;
    rns_packet p = {.packet_type = 1, .data = body};
    memcpy(p.destination_hash, m.source, 16);
    assert(rns_destination_name_hash("lxmf", aspects, 1, name));
    assert(rns_announce_build(&sender, m.source, name, prefix, 1234, NULL, NULL, 0,
        body, sizeof(body), &p.data_length, &p.context_flag));
    assert(rns_packet_encode(&p, ann, sizeof(ann), &ann_length));
    assert(lxmf_packet_node_receive(n, ann, ann_length) == RNS_OK);
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 1);
    rns_packet proof; uint8_t hash[32];
    assert(rns_packet_decode(&proof, sent, sent_length) && proof.packet_type == 3);
    assert(rns_packet_hash(wire, wire_length, hash));
    assert(!memcmp(proof.destination_hash, hash, 16));
    assert(rns_proof_validate(&local, hash, proof.data, proof.data_length));
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 1);
    lxmf_packet_node_stats(n, &info); assert(info.duplicates == 1 && info.proofs_queued == 2);
    wire[wire_length - 1] ^= 1;
    assert(lxmf_packet_node_receive(n, wire, wire_length) == RNS_OK && messages == 1);
    lxmf_packet_node_stats(n, &info); assert(info.rejected == 2 && info.proofs_queued == 2);
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
    record[0] = 2;
    assert(lxmf_packet_node_create(storage, radio, message, NULL, &n) == RNS_ERROR_PROTOCOL && n == NULL);
    rns_interface_destroy(radio); rns_storage_destroy(storage);
    return 0;
}
