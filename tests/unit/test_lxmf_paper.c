#include "reticulum/destination.h"
#include "reticulum/lxmf_paper.h"
#include "../fixtures/lxmf_paper_vectors.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const rns_identity *identity; uint8_t hash[16]; } resolver_t;
static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    resolver_t *resolver = context;
    return memcmp(hash, resolver->hash, 16u) == 0 ? resolver->identity : NULL;
}
static void delivery_hash(const rns_identity *identity, uint8_t hash[16]) {
    static const char *const aspects[] = {"delivery"};
    assert(rns_destination_hash(identity, "lxmf", aspects, 1u, hash));
}

static void python_uri_fixtures(void) {
    uint8_t decoded[LXMF_PAPER_MAX_SIZE], transient[32];
    char encoded[LXMF_URI_MAX_CANONICAL_LENGTH + 1u];
    for (size_t i = 0; i < sizeof paper_fixtures / sizeof paper_fixtures[0]; ++i) {
        const paper_fixture *fixture = &paper_fixtures[i];
        size_t encoded_length = 0u, decoded_length = 0u;
        assert(lxmf_uri_encode(fixture->paper, fixture->paper_len, encoded,
            sizeof encoded, &encoded_length) == LXMF_OK);
        assert(encoded_length == fixture->uri_len &&
               memcmp(encoded, fixture->uri, encoded_length + 1u) == 0);
        assert(lxmf_uri_decode(fixture->uri, fixture->uri_len, decoded,
            sizeof decoded, &decoded_length, transient) == LXMF_OK);
        assert(decoded_length == fixture->paper_len &&
               memcmp(decoded, fixture->paper, decoded_length) == 0 &&
               memcmp(transient, fixture->transient, 32u) == 0);
        assert(lxmf_uri_encode(fixture->paper, fixture->paper_len, encoded,
            fixture->uri_len, &encoded_length) == LXMF_ERR_BOUNDS);
        assert(lxmf_uri_decode(fixture->uri, fixture->uri_len, decoded,
            fixture->paper_len - 1u, &decoded_length, transient) ==
            LXMF_ERR_BOUNDS);
    }
    const paper_fixture *small = &paper_fixtures[0];
    char separated[512];
    memcpy(separated, "LXM://", 6u);
    size_t at = 6u;
    for (size_t i = 6u; i < small->uri_len; ++i) {
        if ((i - 6u) && (i - 6u) % 17u == 0u) separated[at++] = '/';
        separated[at++] = small->uri[i];
    }
    size_t decoded_length;
    assert(lxmf_uri_decode(separated, at, decoded, sizeof decoded,
        &decoded_length, transient) == LXMF_OK &&
        decoded_length == small->paper_len &&
        memcmp(decoded, small->paper, decoded_length) == 0);
}

static void encrypted_roundtrip(void) {
    rns_identity source, recipient;
    assert(rns_identity_generate(&source));
    assert(rns_identity_generate(&recipient));
    static const uint8_t fields[] = {0x81, 0xcc, 0xee, 0x92, 0xc4, 2, 0, 255,
                                     0xa2, 'o', 'k'};
    lxmf_message_t message = {.timestamp = 1700000000.25,
        .title = {(const uint8_t *)"Paper title", 11u},
        .content = {(const uint8_t *)"fold me", 7u},
        .fields_msgpack = {fields, sizeof fields}, .has_stamp = true,
        .stamp_len = LXMF_STAMP_LENGTH};
    memset(message.stamp, 0xa5, LXMF_STAMP_LENGTH);
    delivery_hash(&source, message.source);
    delivery_hash(&recipient, message.destination);
    uint8_t paper[LXMF_PAPER_MAX_SIZE], plaintext[LXMF_PAPER_MAX_SIZE];
    uint8_t transient[32], decoded_transient[32];
    size_t paper_length = 0u, plaintext_length = 0u;
    assert(lxmf_paper_pack(&message, &source, &recipient, NULL, paper,
        sizeof paper, &paper_length, transient) == LXMF_OK);
    resolver_t resolver = {.identity = &source};
    memcpy(resolver.hash, message.source, 16u);
    lxmf_identity_verifier_context_t verifier = {
        .resolve = resolve, .resolve_context = &resolver};
    lxmf_message_t decoded;
    assert(lxmf_paper_unpack(paper, paper_length, &recipient, NULL, 0u, 0,
        lxmf_identity_verifier, &verifier, plaintext, sizeof plaintext,
        &plaintext_length, &decoded, decoded_transient, NULL, NULL) == LXMF_OK);
    assert(memcmp(transient, decoded_transient, 32u) == 0 &&
           decoded.title.len == message.title.len &&
           memcmp(decoded.title.data, message.title.data, message.title.len) == 0 &&
           decoded.fields_msgpack.len == sizeof fields &&
           memcmp(decoded.fields_msgpack.data, fields, sizeof fields) == 0 &&
           decoded.has_stamp && decoded.stamp_len == LXMF_STAMP_LENGTH &&
           memcmp(decoded.stamp, message.stamp, LXMF_STAMP_LENGTH) == 0);
    char uri[LXMF_URI_MAX_CANONICAL_LENGTH + 1u];
    size_t uri_length, uri_paper_length;
    assert(lxmf_uri_encode(paper, paper_length, uri, sizeof uri, &uri_length) ==
           LXMF_OK);
    uint8_t uri_paper[LXMF_PAPER_MAX_SIZE], uri_transient[32];
    assert(lxmf_uri_decode(uri, uri_length, uri_paper, sizeof uri_paper,
        &uri_paper_length, uri_transient) == LXMF_OK &&
        uri_paper_length == paper_length &&
        memcmp(uri_paper, paper, paper_length) == 0 &&
        memcmp(uri_transient, transient, 32u) == 0);
    assert(lxmf_paper_pack(&message, &source, &recipient, NULL, paper,
        paper_length - 1u, &paper_length, transient) == LXMF_ERR_BOUNDS);
    uint8_t oversized_content[2048];
    memset(oversized_content, 0x5a, sizeof oversized_content);
    lxmf_slice_t original_content = message.content;
    message.content = (lxmf_slice_t){oversized_content,
                                      sizeof oversized_content};
    assert(lxmf_paper_pack(&message, &source, &recipient, NULL, paper,
        sizeof paper, &paper_length, transient) == LXMF_ERR_BOUNDS);
    message.content = original_content;

    uint8_t ratchet_private[32], ratchet_public[32], ratchet_id[16];
    assert(rns_identity_ratchet_generate(ratchet_private, ratchet_public,
                                         ratchet_id));
    assert(lxmf_paper_pack(&message, &source, &recipient, ratchet_public,
        paper, sizeof paper, &paper_length, transient) == LXMF_OK);
    int used_ratchet = 0;
    uint8_t used_id[16];
    assert(lxmf_paper_unpack(paper, paper_length, &recipient, ratchet_private,
        1u, 1, lxmf_identity_verifier, &verifier, plaintext, sizeof plaintext,
        &plaintext_length, &decoded, decoded_transient, used_id,
        &used_ratchet) == LXMF_OK && used_ratchet &&
        memcmp(used_id, ratchet_id, 16u) == 0);
    ratchet_private[15] ^= 1u;
    assert(lxmf_paper_unpack(paper, paper_length, &recipient, ratchet_private,
        1u, 1, NULL, NULL, plaintext, sizeof plaintext, &plaintext_length,
        &decoded, decoded_transient, used_id, &used_ratchet) == LXMF_ERR_CRYPTO);
}

static void malformed(void) {
    uint8_t paper[LXMF_PAPER_MAX_SIZE], transient[32];
    size_t length;
    static const char *const invalid[] = {
        "http://AAAA", "lxmf://AAAA", "lxm://A", "lxm://AA=A",
        "lxm://AA A", "lxm://++++", "lxm://"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        assert(lxmf_uri_decode(invalid[i], strlen(invalid[i]), paper,
            sizeof paper, &length, transient) != LXMF_OK);
    char huge[LXMF_URI_MAX_INPUT_LENGTH + 1u];
    memset(huge, 'A', sizeof huge);
    memcpy(huge, "lxm://", 6u);
    assert(lxmf_uri_decode(huge, sizeof huge, paper, sizeof paper, &length,
                           transient) == LXMF_ERR_FORMAT);
    uint8_t one = 0;
    char uri[16];
    assert(lxmf_uri_encode(&one, 0u, uri, sizeof uri, &length) ==
           LXMF_ERR_BOUNDS);
    assert(lxmf_uri_encode(paper, LXMF_PAPER_MAX_SIZE + 1u, uri, sizeof uri,
                           &length) == LXMF_ERR_BOUNDS);
}

int main(void) {
    assert(LXMF_PAPER_MAX_SIZE == 2210u &&
           LXMF_URI_MAX_CANONICAL_LENGTH == 2953u);
    python_uri_fixtures();
    encrypted_roundtrip();
    malformed();
    return 0;
}
