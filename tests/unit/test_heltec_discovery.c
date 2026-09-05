/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio_discovery.h"
#include "reticulum/announce.h"
#include "reticulum/destination.h"
#include "reticulum/packet.h"
#include <assert.h>
#include <string.h>

static rns_radio_encoded_packet_t make_peer_announce(uint64_t timestamp, bool large, bool corrupt, uint8_t seed) {
    uint8_t key[64], body[465], raw[500], name[10], random[5] = {1,2,3,4,5};
    uint8_t app[200] = {0};
    rns_identity identity;
    const char *aspects[] = {"delivery"};
    rns_packet packet = {.packet_type = 1};
    size_t length;
    rns_radio_encoded_packet_t result;
    memset(key, seed, sizeof(key));
    assert(rns_identity_from_private(&identity, key));
    assert(rns_destination_name_hash("lxmf", aspects, 1, name));
    assert(rns_destination_hash(&identity, "lxmf", aspects, 1, packet.destination_hash));
    assert(rns_announce_build(&identity, packet.destination_hash, name, random,
        timestamp, NULL, app, large ? sizeof(app) : 0U, body, sizeof(body),
        &packet.data_length, &packet.context_flag));
    if (corrupt) body[100] ^= 1U;
    packet.data = body;
    assert(rns_packet_encode(&packet, raw, sizeof(raw), &length));
    assert(rns_radio_frame_encode(raw, length, 3U, &result) == RNS_OK);
    return result;
}
static rns_radio_encoded_packet_t make_announce(uint64_t timestamp, bool large, bool corrupt) {
    return make_peer_announce(timestamp, large, corrupt, 42U);
}
static void feed(heltec_radio_discovery *s, rns_radio_encoded_packet_t *p, uint64_t now) {
    for (size_t i = 0; i < p->count; ++i)
        heltec_radio_discovery_receive(s, p->frames[i], p->lengths[i], now + i);
}
int main(void) {
    heltec_radio_discovery s;
    heltec_radio_discovery_init(&s);
    rns_radio_encoded_packet_t p = make_announce(100, false, false);
    feed(&s, &p, 0);
    assert(s.verified == 1 && s.peer_count == 1);
    feed(&s, &p, 11000);
    assert(s.verified == 1 && s.duplicates == 1);
    p = make_announce(99, false, false);
    feed(&s, &p, 22000);
    assert(s.stale == 1 && s.verified == 1);
    p = make_announce(101, false, true);
    feed(&s, &p, 33000);
    assert(s.invalid == 1 && s.verified == 1);
    p = make_announce(101, true, false);
    assert(p.count == 2);
    feed(&s, &p, 44000);
    assert(s.verified == 2 && s.peer_count == 1);
    heltec_radio_discovery_poll(&s, 3644001);
    assert(s.peer_count == 0);
    heltec_radio_discovery_init(&s);
    heltec_radio_discovery_receive(&s, p.frames[1], p.lengths[1], 0);
    assert(s.verified == 0);
    heltec_radio_discovery_init(&s);
    heltec_radio_discovery_receive(&s, p.frames[0], p.lengths[0], 0);
    heltec_radio_discovery_poll(&s, 11000);
    assert(s.frames.timed_out_packets == 1 && s.verified == 0);
    heltec_radio_discovery_init(&s);
    for (uint8_t i = 0U; i < HELTEC_DISCOVERY_PEERS + 1U; ++i) {
        p = make_peer_announce(100, false, false, i);
        feed(&s, &p, (uint64_t)i * 11000U);
    }
    assert(s.peer_count == HELTEC_DISCOVERY_PEERS);
    assert(s.verified == HELTEC_DISCOVERY_PEERS + 1U);
    assert(s.peers[0].last_seen_ms == HELTEC_DISCOVERY_PEERS * 11000U);
    heltec_radio_discovery_init(&s);
    p = make_announce(100, false, false);
    p.frames[0][1] |= 0x80U;
    feed(&s, &p, 0);
    assert(s.invalid == 1U && s.peer_count == 0U);
    p.frames[0][1] = 0U;
    p.lengths[0] = 2U;
    feed(&s, &p, 11000);
    assert(s.invalid == 2U && s.peer_count == 0U);
    return 0;
}
