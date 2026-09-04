#include "reticulum/crypto.h"
#include "reticulum/request.h"

#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t encoded[128], digest[32];
    size_t encoded_length = 0U;
    const char *path = "/page/index.mu";
    assert(rns_request_encode(path, 42.5, NULL, 0U, encoded, sizeof encoded,
                              &encoded_length) == RNS_OK);
    assert(encoded_length == 29U && encoded[0] == 0x93U &&
           encoded[1] == 0xcbU && encoded[10] == 0xc4U &&
           encoded[11] == 16U && encoded[28] == 0xc0U);
    assert(rns_sha256((const uint8_t *)path, strlen(path), digest));
    assert(memcmp(encoded + 12U, digest, 16U) == 0);

    rns_request_view_t request;
    assert(rns_request_decode(encoded, encoded_length, &request) == RNS_OK);
    assert(request.requested_at == 42.5);
    assert(memcmp(request.path_hash, digest, 16U) == 0);
    assert(request.data_msgpack_length == 1U && request.data_msgpack[0] == 0xc0U);

    static const uint8_t form[] = {0x81U, 0xa7U, 'f','i','e','l','d','_','q',
                                   0xa3U, 'r','e','i'};
    assert(rns_request_encode(path, 1.0, form, sizeof form, encoded,
                              sizeof encoded, &encoded_length) == RNS_OK);
    assert(rns_request_decode(encoded, encoded_length, &request) == RNS_OK);
    assert(request.data_msgpack_length == sizeof form);
    assert(rns_request_encode(path, 1.0, form, sizeof form - 1U, encoded,
                              sizeof encoded, &encoded_length) ==
           RNS_ERROR_PROTOCOL);

    uint8_t response_wire[64] = {0x92U, 0xc4U, 16U};
    memcpy(response_wire + 3U, digest, 16U);
    response_wire[19] = 0xc4U;
    response_wire[20] = 5U;
    memcpy(response_wire + 21U, "hello", 5U);
    rns_response_view_t response;
    assert(rns_response_decode(response_wire, 26U, &response) == RNS_OK);
    assert(memcmp(response.request_id, digest, 16U) == 0);
    assert(response.response_length == 5U &&
           memcmp(response.response, "hello", 5U) == 0);
    assert(response.response_msgpack_length == 7U &&
           response.response_msgpack[0] == 0xc4U);
    response_wire[20] = 60U;
    assert(rns_response_decode(response_wire, 26U, &response) ==
           RNS_ERROR_PROTOCOL);

    static const uint8_t map_response[] = {0x81U, 0xa2U, 'o', 'k', 0xc3U};
    size_t response_length = 0U;
    assert(rns_response_encode(digest, map_response, sizeof map_response,
                               response_wire, sizeof response_wire,
                               &response_length) == RNS_OK);
    assert(response_length == 19U + sizeof map_response);
    assert(rns_response_decode(response_wire, response_length, &response) ==
           RNS_OK);
    assert(response.response_msgpack_length == sizeof map_response &&
           memcmp(response.response_msgpack, map_response,
                  sizeof map_response) == 0);
    assert(response.response == response.response_msgpack);
    assert(rns_response_encode(digest, map_response,
                               sizeof map_response - 1U, response_wire,
                               sizeof response_wire, &response_length) ==
           RNS_ERROR_PROTOCOL);
    return 0;
}
