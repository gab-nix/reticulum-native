#ifndef RETICULUM_RRC_H
#define RETICULUM_RRC_H
#include <stddef.h>
#include <stdint.h>
#include "reticulum/status.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RNS_RRC_VERSION 1u
#define RNS_RRC_MESSAGE_ID_SIZE 8u
#define RNS_RRC_SOURCE_SIZE 16u
#define RNS_RRC_MAX_ENVELOPE_SIZE 65535u
#define RNS_RRC_MAX_ROOM_BYTES 255u
#define RNS_RRC_MAX_NICK_BYTES 255u
typedef enum { RNS_RRC_HELLO=1,RNS_RRC_WELCOME=2,RNS_RRC_JOIN=10,
RNS_RRC_JOINED=11,RNS_RRC_PART=12,RNS_RRC_PARTED=13,RNS_RRC_MESSAGE=20,
RNS_RRC_NOTICE=21,RNS_RRC_PING=30,RNS_RRC_PONG=31,RNS_RRC_ERROR=40,
RNS_RRC_RESOURCE_ENVELOPE=50 } rns_rrc_message_type_t;
typedef struct { const uint8_t *data; size_t length; } rns_rrc_slice_t;
typedef struct { uint8_t version; rns_rrc_message_type_t type;
uint8_t message_id[8]; uint64_t timestamp_ms; uint8_t source[16];
rns_rrc_slice_t room,body_cbor,nick; } rns_rrc_envelope_t;
rns_status_t rns_rrc_envelope_encode(const rns_rrc_envelope_t *envelope,
 uint8_t *output,size_t capacity,size_t *output_length);
rns_status_t rns_rrc_envelope_parse(const uint8_t *input,size_t input_length,
 rns_rrc_envelope_t *envelope);
rns_status_t rns_rrc_cbor_text(const uint8_t *text,size_t text_length,
 uint8_t *output,size_t capacity,size_t *output_length);
#ifdef __cplusplus
}
#endif
#endif
