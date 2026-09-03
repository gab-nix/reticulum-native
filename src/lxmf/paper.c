#include "reticulum/lxmf_paper.h"

#include "reticulum/destination.h"
#include "reticulum/hal.h"

#include <ctype.h>
#include <string.h>

static int delivery_hash(const rns_identity *identity, uint8_t output[16]) {
    static const char *const aspects[] = {"delivery"};
    return rns_destination_hash(identity, "lxmf", aspects, 1u, output);
}

lxmf_status_t lxmf_paper_pack(
    const lxmf_message_t *message, const rns_identity *source_identity,
    const rns_identity *destination_identity, const uint8_t ratchet_public[32],
    uint8_t *paper, size_t capacity, size_t *paper_length,
    uint8_t transient_id[32]) {
    if (!message || !source_identity || !source_identity->has_private ||
        !destination_identity || !paper || !paper_length || !transient_id)
        return LXMF_ERR_ARGUMENT;
    *paper_length = 0u;
    uint8_t source[16], destination[16];
    if (!delivery_hash(source_identity, source) ||
        !delivery_hash(destination_identity, destination))
        return LXMF_ERR_CRYPTO;
    if (memcmp(message->source, source, 16u) != 0 ||
        memcmp(message->destination, destination, 16u) != 0)
        return LXMF_ERR_ARGUMENT;
    uint8_t packed[LXMF_PAPER_MAX_SIZE];
    size_t packed_length = 0u;
    lxmf_status_t status = lxmf_pack(message, lxmf_identity_signer,
        (void *)source_identity, packed, sizeof packed, &packed_length);
    if (status != LXMF_OK) {
        rns_hal_secure_zero(packed, sizeof packed);
        return status;
    }
    if (packed_length < LXMF_DESTINATION_LENGTH) {
        rns_hal_secure_zero(packed, sizeof packed);
        return LXMF_ERR_FORMAT;
    }
    size_t bound = rns_identity_encrypt_bound(
        packed_length - LXMF_DESTINATION_LENGTH);
    if (bound == 0u || bound > LXMF_PAPER_MAX_SIZE - LXMF_DESTINATION_LENGTH ||
        capacity < LXMF_DESTINATION_LENGTH + bound) {
        rns_hal_secure_zero(packed, sizeof packed);
        return LXMF_ERR_BOUNDS;
    }
    memcpy(paper, destination, LXMF_DESTINATION_LENGTH);
    size_t encrypted_length = 0u;
    int encrypted = rns_identity_encrypt(destination_identity, ratchet_public,
        packed + LXMF_DESTINATION_LENGTH,
        packed_length - LXMF_DESTINATION_LENGTH,
        paper + LXMF_DESTINATION_LENGTH, capacity - LXMF_DESTINATION_LENGTH,
        &encrypted_length);
    rns_hal_secure_zero(packed, sizeof packed);
    if (!encrypted) return LXMF_ERR_CRYPTO;
    if (encrypted_length > LXMF_PAPER_MAX_SIZE - LXMF_DESTINATION_LENGTH)
        return LXMF_ERR_BOUNDS;
    *paper_length = LXMF_DESTINATION_LENGTH + encrypted_length;
    lxmf_sha256(paper, *paper_length, transient_id);
    return LXMF_OK;
}

lxmf_status_t lxmf_paper_unpack(
    const uint8_t *paper, size_t paper_length,
    const rns_identity *local_identity, const uint8_t *ratchet_private_keys,
    size_t ratchet_count, int enforce_ratchets, lxmf_verify_fn verifier,
    void *verify_context, uint8_t *plaintext, size_t plaintext_capacity,
    size_t *plaintext_length, lxmf_message_t *message,
    uint8_t transient_id[32], uint8_t ratchet_id[16], int *used_ratchet) {
    if (!paper || !local_identity || !local_identity->has_private ||
        (ratchet_count && !ratchet_private_keys) || !plaintext ||
        !plaintext_length || !message || !transient_id)
        return LXMF_ERR_ARGUMENT;
    *plaintext_length = 0u;
    if (paper_length <= LXMF_DESTINATION_LENGTH ||
        paper_length > LXMF_PAPER_MAX_SIZE)
        return LXMF_ERR_BOUNDS;
    if (plaintext_capacity < LXMF_DESTINATION_LENGTH) return LXMF_ERR_BOUNDS;
    uint8_t destination[16];
    if (!delivery_hash(local_identity, destination)) return LXMF_ERR_CRYPTO;
    if (memcmp(destination, paper, 16u) != 0) return LXMF_ERR_FORMAT;
    memcpy(plaintext, paper, LXMF_DESTINATION_LENGTH);
    size_t decrypted_length = 0u;
    if (!rns_identity_decrypt_with_ratchets(local_identity,
        ratchet_private_keys, ratchet_count, enforce_ratchets,
        paper + LXMF_DESTINATION_LENGTH,
        paper_length - LXMF_DESTINATION_LENGTH,
        plaintext + LXMF_DESTINATION_LENGTH,
        plaintext_capacity - LXMF_DESTINATION_LENGTH, &decrypted_length,
        ratchet_id, used_ratchet))
        return LXMF_ERR_CRYPTO;
    if (decrypted_length > plaintext_capacity - LXMF_DESTINATION_LENGTH)
        return LXMF_ERR_BOUNDS;
    *plaintext_length = LXMF_DESTINATION_LENGTH + decrypted_length;
    lxmf_sha256(paper, paper_length, transient_id);
    return lxmf_unpack(plaintext, *plaintext_length, verifier, verify_context,
                       message);
}

static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

lxmf_status_t lxmf_uri_encode(const uint8_t *paper, size_t paper_length,
                              char *uri, size_t capacity, size_t *uri_length) {
    if ((!paper && paper_length) || !uri || !uri_length)
        return LXMF_ERR_ARGUMENT;
    *uri_length = 0u;
    if (!paper_length || paper_length > LXMF_PAPER_MAX_SIZE)
        return LXMF_ERR_BOUNDS;
    size_t encoded = (paper_length / 3u) * 4u;
    if (paper_length % 3u) encoded += paper_length % 3u + 1u;
    if (encoded > LXMF_URI_MAX_CANONICAL_LENGTH - LXMF_URI_SCHEME_LENGTH ||
        capacity <= LXMF_URI_SCHEME_LENGTH + encoded)
        return LXMF_ERR_BOUNDS;
    memcpy(uri, LXMF_URI_SCHEME, LXMF_URI_SCHEME_LENGTH);
    size_t at = LXMF_URI_SCHEME_LENGTH;
    for (size_t i = 0; i < paper_length; i += 3u) {
        size_t left = paper_length - i;
        uint32_t value = (uint32_t)paper[i] << 16;
        if (left > 1u) value |= (uint32_t)paper[i + 1u] << 8;
        if (left > 2u) value |= paper[i + 2u];
        uri[at++] = alphabet[(value >> 18) & 63u];
        uri[at++] = alphabet[(value >> 12) & 63u];
        if (left > 1u) uri[at++] = alphabet[(value >> 6) & 63u];
        if (left > 2u) uri[at++] = alphabet[value & 63u];
    }
    uri[at] = '\0';
    *uri_length = at;
    return LXMF_OK;
}

static int b64_value(unsigned char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '-') return 62;
    if (character == '_') return 63;
    return -1;
}

lxmf_status_t lxmf_uri_decode(const char *uri, size_t uri_length,
                              uint8_t *paper, size_t capacity,
                              size_t *paper_length, uint8_t transient_id[32]) {
    if (!uri || !paper || !paper_length || !transient_id)
        return LXMF_ERR_ARGUMENT;
    *paper_length = 0u;
    if (uri_length < LXMF_URI_SCHEME_LENGTH + 2u ||
        uri_length > LXMF_URI_MAX_INPUT_LENGTH)
        return LXMF_ERR_FORMAT;
    for (size_t i = 0; i < LXMF_URI_SCHEME_LENGTH; ++i)
        if (tolower((unsigned char)uri[i]) != LXMF_URI_SCHEME[i])
            return LXMF_ERR_FORMAT;
    uint8_t quartet[4];
    size_t count = 0u, out = 0u, symbols = 0u;
    for (size_t i = LXMF_URI_SCHEME_LENGTH; i < uri_length; ++i) {
        unsigned char character = (unsigned char)uri[i];
        if (character == '/') continue;
        int value = b64_value(character);
        if (value < 0) return LXMF_ERR_FORMAT;
        quartet[count++] = (uint8_t)value;
        symbols++;
        if (symbols > LXMF_URI_MAX_CANONICAL_LENGTH - LXMF_URI_SCHEME_LENGTH)
            return LXMF_ERR_BOUNDS;
        if (count == 4u) {
            if (out > capacity || capacity - out < 3u ||
                out > LXMF_PAPER_MAX_SIZE - 3u)
                return LXMF_ERR_BOUNDS;
            paper[out++] = (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4));
            paper[out++] = (uint8_t)((quartet[1] << 4) | (quartet[2] >> 2));
            paper[out++] = (uint8_t)((quartet[2] << 6) | quartet[3]);
            count = 0u;
        }
    }
    if (count == 1u || symbols == 0u) return LXMF_ERR_FORMAT;
    size_t tail = count ? count - 1u : 0u;
    if (tail > capacity - out || out + tail > LXMF_PAPER_MAX_SIZE)
        return LXMF_ERR_BOUNDS;
    if (count >= 2u)
        paper[out++] = (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4));
    if (count == 3u)
        paper[out++] = (uint8_t)((quartet[1] << 4) | (quartet[2] >> 2));
    if (!out) return LXMF_ERR_FORMAT;
    *paper_length = out;
    lxmf_sha256(paper, out, transient_id);
    return LXMF_OK;
}
