#include "reticulum/packet.h"
#include <assert.h>
#include <string.h>

int main(void) {
    rns_packet source = {0}, decoded; uint8_t raw1[500], raw2[500], hash1[32], hash2[32]; size_t n1, n2;
    const uint8_t payload[] = {1,2,3};
    source.header_type = RNS_PACKET_HEADER_1; source.destination_type = 0; source.packet_type = 0; source.hops = 7;
    memset(source.destination_hash, 0x44, 16); source.context = 9; source.data = payload; source.data_length = sizeof(payload);
    assert(rns_packet_encode(&source, raw1, sizeof(raw1), &n1)); assert(n1 == 22);
    assert(rns_packet_decode(&decoded, raw1, n1)); assert(decoded.hops == 7 && decoded.context == 9 && decoded.data_length == 3);
    assert(rns_packet_hash(raw1, n1, hash1)); raw1[1] = 99; raw1[0] |= 0x30; assert(rns_packet_hash(raw1, n1, hash2)); assert(memcmp(hash1, hash2, 32) == 0);
    source.header_type = RNS_PACKET_HEADER_2; source.transport_type = 1; memset(source.transport_id, 0x55, 16);
    assert(rns_packet_encode(&source, raw1, sizeof(raw1), &n1)); memcpy(raw2, raw1, n1); n2 = n1; memset(raw2 + 2, 0xaa, 16);
    assert(rns_packet_hash(raw1, n1, hash1) && rns_packet_hash(raw2, n2, hash2)); assert(memcmp(hash1, hash2, 32) == 0);
    assert(!rns_packet_decode(&decoded, raw1, 3)); assert(!rns_packet_decode(&decoded, raw1, 0));
    return 0;
}
