/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio_discovery.h"
#include "reticulum/announce.h"
#include "reticulum/crypto.h"
#include "reticulum/packet.h"
#include <string.h>
#define PEER_LIFETIME_MS UINT64_C(3600000)

static void count_identities(heltec_radio_discovery *s) {
    s->identity_count = 0U;
    for (size_t i = 0U; i < HELTEC_DISCOVERY_PEERS; ++i) {
        if (!s->peers[i].used) continue;
        bool seen = false;
        for (size_t j = 0U; j < i; ++j)
            if (s->peers[j].used && memcmp(s->peers[i].identity_hash,
                s->peers[j].identity_hash, 16U) == 0) { seen = true; break; }
        if (!seen) ++s->identity_count;
    }
}

static rns_status_t packet_received(const uint8_t *raw, size_t length, void *context) {
    heltec_radio_discovery *s = context;
    rns_packet packet;
    rns_announce announce;
    uint8_t hash[32];
    uint8_t identity_hash[32];
    ++s->packets;
    /* IFAC requires network credentials; never parse its bytes as plaintext. */
    if (length == 0U || (raw[0] & 0x80U) != 0U ||
        !rns_packet_decode(&packet, raw, length)) {
        ++s->invalid;
        return RNS_OK;
    }
    if (packet.packet_type != 1U) return RNS_OK;
    if (packet.destination_type != 0U ||
        !rns_announce_parse(&announce, packet.data, packet.data_length, packet.context_flag) ||
        !rns_announce_verify(packet.destination_hash, packet.data, packet.data_length, packet.context_flag) ||
        !rns_packet_hash(raw, length, hash) ||
        !rns_sha256(announce.public_key, 64U, identity_hash)) {
        ++s->invalid;
        return RNS_OK;
    }
    size_t slot = HELTEC_DISCOVERY_PEERS;
    size_t oldest = 0U;
    for (size_t i = 0U; i < HELTEC_DISCOVERY_PEERS; ++i) {
        heltec_discovered_peer *p = &s->peers[i];
        if (p->used && memcmp(p->destination, packet.destination_hash, 16U) == 0) {
            if (memcmp(p->packet_hash, hash, sizeof(hash)) == 0) {
                ++s->duplicates;
                return RNS_OK;
            }
            if (announce.timestamp < p->timestamp) {
                ++s->stale;
                return RNS_OK;
            }
            slot = i;
            break;
        }
        if (!p->used && slot == HELTEC_DISCOVERY_PEERS) slot = i;
        if (p->last_seen_ms < s->peers[oldest].last_seen_ms) oldest = i;
    }
    if (slot == HELTEC_DISCOVERY_PEERS) slot = oldest;
    heltec_discovered_peer *p = &s->peers[slot];
    if (!p->used) ++s->peer_count;
    p->used = true;
    memcpy(p->destination, packet.destination_hash, 16U);
    memcpy(p->identity_hash, identity_hash, 16U);
    memcpy(p->packet_hash, hash, sizeof(hash));
    p->timestamp = announce.timestamp;
    p->last_seen_ms = s->now_ms;
    ++s->verified;
    count_identities(s);
    return RNS_OK;
}

void heltec_radio_discovery_init(heltec_radio_discovery *s) {
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    (void)rns_radio_reassembler_init(&s->frames, s->storage, sizeof(s->storage), 10000U, 10000U);
}
void heltec_radio_discovery_poll(heltec_radio_discovery *s, uint64_t now_ms) {
    if (s == NULL) return;
    s->now_ms = now_ms;
    (void)rns_radio_reassembler_expire(&s->frames, now_ms);
    for (size_t i = 0U; i < HELTEC_DISCOVERY_PEERS; ++i) {
        heltec_discovered_peer *p = &s->peers[i];
        if (p->used && now_ms >= p->last_seen_ms &&
            now_ms - p->last_seen_ms >= PEER_LIFETIME_MS) {
            memset(p, 0, sizeof(*p));
            --s->peer_count;
        }
    }
    count_identities(s);
}
void heltec_radio_discovery_receive(heltec_radio_discovery *s,
    const uint8_t *frame, size_t length, uint64_t now_ms) {
    if (s == NULL) return;
    heltec_radio_discovery_poll(s, now_ms);
    (void)rns_radio_reassembler_feed(&s->frames, frame, length, now_ms, packet_received, s);
}
