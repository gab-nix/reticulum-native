/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HELTEC_RADIO_DISCOVERY_H
#define HELTEC_RADIO_DISCOVERY_H
#include "reticulum/radio_framing.h"
#define HELTEC_DISCOVERY_PEERS 32U
typedef struct {
    uint8_t destination[16];
    uint8_t identity_hash[16];
    uint8_t packet_hash[32];
    uint64_t timestamp, last_seen_ms;
    bool used;
} heltec_discovered_peer;
/* Application-owned, bounded diagnostics; not a transport/path table. */
typedef struct {
    rns_radio_reassembler_t frames;
    uint8_t storage[RNS_RADIO_PACKET_MTU];
    heltec_discovered_peer peers[HELTEC_DISCOVERY_PEERS];
    uint64_t now_ms, packets, verified, invalid, duplicates, stale;
    size_t peer_count;
    size_t identity_count;
} heltec_radio_discovery;
void heltec_radio_discovery_init(heltec_radio_discovery *state);
void heltec_radio_discovery_packet(heltec_radio_discovery *state,
    const uint8_t *packet, size_t length, uint64_t now_ms);
void heltec_radio_discovery_poll(heltec_radio_discovery *state, uint64_t now_ms);
void heltec_radio_discovery_receive(heltec_radio_discovery *state,
    const uint8_t *frame, size_t length, uint64_t now_ms);
#endif
