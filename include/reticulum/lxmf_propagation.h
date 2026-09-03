#ifndef RETICULUM_LXMF_PROPAGATION_H
#define RETICULUM_LXMF_PROPAGATION_H

#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_PN_GET_PATH "/get"
#define LXMF_PN_MAX_ITEMS 128u
#define LXMF_PN_MAX_WIRE (8u * 1024u * 1024u)
#define LXMF_PN_MAX_ANNOUNCE 4096u
#define LXMF_PN_ERROR_NO_IDENTITY 0xf0u
#define LXMF_PN_ERROR_NO_ACCESS 0xf1u
#define LXMF_PN_ERROR_INVALID_STAMP 0xf5u

/* Preserves integer versus floating-point representation. Limits are decimal
 * kilobytes (1000 bytes), not bytes or KiB. Negative/non-finite values fail. */
typedef struct {
    bool is_float;
    uint64_t integer;
    double real;
} lxmf_pn_number_t;

typedef struct {
    bool legacy_support;
    uint64_t timebase;
    bool enabled;
    lxmf_pn_number_t transfer_limit_kb;
    lxmf_pn_number_t sync_limit_kb;
    uint8_t stamp_cost, stamp_flexibility, peering_cost;
    /* Exactly one map; empty encodes {}. Unknown metadata is retained. */
    lxmf_slice_t metadata_msgpack;
    /* Additional outer array elements, retained as concatenated objects. */
    lxmf_slice_t extensions_msgpack;
    size_t extension_count;
} lxmf_pn_announce_t;

/* Decoders borrow input bytes, perform no allocation, and publish output only
 * on success. Input must remain alive while any returned slice is used.
 * These codecs do not authenticate peers, decrypt messages, validate stamps,
 * manage sessions or authorize deletion of acknowledged messages. */
lxmf_status_t lxmf_pn_announce_encode(const lxmf_pn_announce_t *announce,
    uint8_t *output, size_t capacity, size_t *length);
lxmf_status_t lxmf_pn_announce_decode(const uint8_t *input, size_t length,
    lxmf_pn_announce_t *announce);

/* Upload is [timebase, [destination || encrypted_message || PN_stamp, ...]].
 * Message bytes are opaque here. The transient ID excludes the final PN stamp. */
typedef struct {
    double timebase;
    size_t count;
    lxmf_slice_t messages[LXMF_PN_MAX_ITEMS];
} lxmf_pn_upload_t;
lxmf_status_t lxmf_pn_upload_encode(const lxmf_pn_upload_t *upload,
    uint8_t *output, size_t capacity, size_t *length);
lxmf_status_t lxmf_pn_upload_decode(const uint8_t *input, size_t length,
    lxmf_pn_upload_t *upload);
/* Pinned packet upload rejection is [ERROR_INVALID_STAMP], not a /get error. */
lxmf_status_t lxmf_pn_upload_rejection_encode(uint8_t *output,
    size_t capacity, size_t *length);
lxmf_status_t lxmf_pn_upload_rejection_decode(const uint8_t *input, size_t length);

typedef struct {
    bool wants_null, haves_null;
    size_t wants_count, haves_count;
    lxmf_slice_t wants[LXMF_PN_MAX_ITEMS];
    lxmf_slice_t haves[LXMF_PN_MAX_ITEMS];
    bool has_limit;
    lxmf_pn_number_t limit_kb;
} lxmf_pn_get_request_t;
/* [nil,nil] lists IDs; [wants,haves,limit] downloads; [nil,haves] acknowledges.
 * nil and [] remain distinct. Haves may delete server copies: populate only
 * after durable successful receipt, never just after decoding the response. */
lxmf_status_t lxmf_pn_get_request_encode(const lxmf_pn_get_request_t *request,
    uint8_t *output, size_t capacity, size_t *length);
lxmf_status_t lxmf_pn_get_request_decode(const uint8_t *input, size_t length,
    lxmf_pn_get_request_t *request);

typedef enum {
    LXMF_PN_RESPONSE_ITEMS = 0,
    LXMF_PN_RESPONSE_NIL,
    LXMF_PN_RESPONSE_ERROR
} lxmf_pn_response_kind_t;
typedef struct {
    lxmf_pn_response_kind_t kind;
    uint8_t error;
    size_t count;
    lxmf_slice_t items[LXMF_PN_MAX_ITEMS];
} lxmf_pn_get_response_t;
/* ids=true validates 32-byte transient IDs (list); false accepts opaque
 * encrypted messages WITHOUT the PN stamp (download). /get error is an
 * integer, not an array; nil is retained as a distinct server failure. */
lxmf_status_t lxmf_pn_get_response_encode(const lxmf_pn_get_response_t *response,
    bool ids, uint8_t *output, size_t capacity, size_t *length);
lxmf_status_t lxmf_pn_get_response_decode(const uint8_t *input, size_t length,
    bool ids, lxmf_pn_get_response_t *response);

#ifdef __cplusplus
}
#endif
#endif
