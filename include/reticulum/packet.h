#ifndef RETICULUM_PACKET_H
#define RETICULUM_PACKET_H

#include <stddef.h>
#include <stdint.h>

#define RNS_MTU 500u
#define RNS_PACKET_HEADER_1 0u
#define RNS_PACKET_HEADER_2 1u

typedef struct {
    uint8_t header_type;
    uint8_t context_flag;
    uint8_t transport_type;
    uint8_t destination_type;
    uint8_t packet_type;
    uint8_t hops;
    uint8_t transport_id[16];
    uint8_t destination_hash[16];
    uint8_t context;
    const uint8_t *data;
    size_t data_length;
} rns_packet;

int rns_packet_encode(const rns_packet *packet, uint8_t *out, size_t capacity, size_t *out_length);
int rns_packet_decode(rns_packet *packet, const uint8_t *raw, size_t raw_length);
int rns_packet_hash(const uint8_t *raw, size_t raw_length, uint8_t out[32]);
int rns_packet_truncated_hash(const uint8_t *raw, size_t raw_length, uint8_t out[16]);

#endif
