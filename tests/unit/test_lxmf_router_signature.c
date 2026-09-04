/* Inbound messages whose signer is not yet known are retained and flagged,
 * forgeries from a known signer are refused, and a later announce settles the
 * retained ones either way. */
#include "reticulum/destination.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const rns_identity *known;   /* The one identity the resolver holds. */
    uint8_t known_hash[16];
} resolver_t;

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    resolver_t *state = context;
    if (state->known == NULL) return NULL;
    return memcmp(state->known_hash, hash, 16) == 0 ? state->known : NULL;
}

static lxmf_status_t no_send(void *context, const uint8_t *packet, size_t length) {
    (void)context; (void)packet; (void)length;
    return LXMF_OK;
}

typedef struct {
    size_t count;
    uint8_t id[32];
    lxmf_signature_state_t state;
} inbox_t;

static void on_message(void *context, const lxmf_store_message_t *message) {
    inbox_t *inbox = context;
    memcpy(inbox->id, message->message_id, 32);
    inbox->state = message->signature_state;
    inbox->count++;
}

typedef struct {
    size_t count;
    uint8_t id[32];
    lxmf_signature_state_t state;
} signature_log_t;

static void on_signature(void *context, const uint8_t id[32],
                         lxmf_signature_state_t state) {
    signature_log_t *log = context;
    memcpy(log->id, id, 32);
    log->state = state;
    log->count++;
}

static void delivery_hash(const rns_identity *identity, uint8_t out[16]) {
    const char *aspects[] = {"delivery"};
    assert(rns_destination_hash(identity, "lxmf", aspects, 1u, out));
}

/* Packs a message whose claimed source is `claimed` but whose signature is made
 * by `signer`, then seals it to `recipient`. With signer == the identity owning
 * `claimed` this is an ordinary message; with any other signer it is a forgery
 * that only a holder of the claimed identity can detect. */
static size_t seal(const rns_identity *signer, const uint8_t claimed[16],
                   const rns_identity *recipient, const char *body,
                   uint8_t *packet, size_t capacity) {
    uint8_t plain[RNS_MTU], sealed[RNS_MTU], destination[16];
    size_t plain_length = 0, sealed_length = 0, packet_length = 0;
    lxmf_message_t message = {0};
    rns_packet outer = {0};

    delivery_hash(recipient, destination);
    memcpy(message.destination, destination, 16);
    memcpy(message.source, claimed, 16);
    message.timestamp = 1234.5;
    message.content = (lxmf_slice_t){(const uint8_t *)body, strlen(body)};
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)signer, plain,
                     sizeof plain, &plain_length) == LXMF_OK);
    assert(plain_length >= LXMF_DESTINATION_LENGTH);
    assert(rns_identity_encrypt(recipient, NULL,
                                plain + LXMF_DESTINATION_LENGTH,
                                plain_length - LXMF_DESTINATION_LENGTH, sealed,
                                sizeof sealed, &sealed_length));
    memcpy(outer.destination_hash, destination, 16);
    outer.data = sealed;
    outer.data_length = sealed_length;
    assert(rns_packet_encode(&outer, packet, capacity, &packet_length));
    return packet_length;
}

int main(void) {
    char path[] = "/tmp/lxmf-router-signature-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);

    rns_identity alice, bob, mallory;
    uint8_t alice_hash[16], mallory_hash[16];
    assert(rns_identity_generate(&alice));
    assert(rns_identity_generate(&bob));
    assert(rns_identity_generate(&mallory));
    delivery_hash(&alice, alice_hash);
    delivery_hash(&mallory, mallory_hash);

    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    resolver_t resolver = {NULL, {0}};
    inbox_t inbox = {0};
    signature_log_t log = {0};
    lxmf_router_t router;
    lxmf_router_config_t config = {
        .identity = &bob, .store = &store, .resolve_identity = resolve,
        .resolve_context = &resolver, .send_packet = no_send,
        .send_context = NULL, .message_callback = on_message,
        .message_context = &inbox, .signature_callback = on_signature,
        .signature_context = &log};
    assert(lxmf_router_init(&router, &config) == LXMF_OK);

    /* Alice has not announced, so her message cannot be judged: it is kept and
     * flagged rather than dropped. */
    uint8_t packet[RNS_MTU];
    size_t length = seal(&alice, alice_hash, &bob, "unannounced hello", packet,
                         sizeof packet);
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_OK);
    assert(inbox.count == 1u && inbox.state == LXMF_SIGNATURE_UNVERIFIED);
    assert(lxmf_store_count(&store) == 1u && lxmf_store_unverified_count(&store) == 1u);
    uint8_t alice_message[32];
    memcpy(alice_message, inbox.id, 32);

    /* A forgery from a sender that has not announced is indistinguishable from
     * an honest one, so it is retained too. */
    uint8_t forged_packet[RNS_MTU];
    size_t forged_length = seal(&bob, mallory_hash, &bob, "forged hello",
                                forged_packet, sizeof forged_packet);
    assert(lxmf_router_receive_packet(&router, forged_packet, forged_length) == LXMF_OK);
    assert(inbox.count == 2u && inbox.state == LXMF_SIGNATURE_UNVERIFIED);
    uint8_t forged_message[32];
    memcpy(forged_message, inbox.id, 32);
    assert(lxmf_store_unverified_count(&store) == 2u);

    /* Mallory announces. Her retained message was not signed by her, so it is
     * rejected, while Alice's is left pending. */
    resolver.known = &mallory;
    memcpy(resolver.known_hash, mallory_hash, 16);
    lxmf_router_verify_result_t result;
    assert(lxmf_router_verify_pending(&router, mallory_hash, &result) == LXMF_OK);
    assert(result.examined == 1u && result.rejected == 1u && result.verified == 0u);
    assert(log.count == 1u && log.state == LXMF_SIGNATURE_FAILED &&
           memcmp(log.id, forged_message, 32) == 0);
    assert(lxmf_store_count(&store) == 1u && lxmf_store_unverified_count(&store) == 1u);
    uint8_t body[64];
    lxmf_store_message_t got;
    assert(lxmf_store_read(&store, forged_message, &got, body, sizeof body) ==
           LXMF_ERR_FORMAT);

    /* A forgery received after the announce never enters the store at all. */
    assert(lxmf_router_receive_packet(&router, forged_packet, forged_length) ==
           LXMF_ERR_SIGNATURE);
    assert(lxmf_store_count(&store) == 1u && inbox.count == 2u);

    /* Alice announces. Her retained message verifies and is promoted durably. */
    resolver.known = &alice;
    memcpy(resolver.known_hash, alice_hash, 16);
    assert(lxmf_router_verify_pending(&router, alice_hash, &result) == LXMF_OK);
    assert(result.examined == 1u && result.verified == 1u && result.rejected == 0u &&
           result.pending == 0u);
    assert(log.count == 2u && log.state == LXMF_SIGNATURE_VERIFIED &&
           memcmp(log.id, alice_message, 32) == 0);
    assert(lxmf_store_unverified_count(&store) == 0u);
    assert(lxmf_store_read(&store, alice_message, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_VERIFIED &&
           got.status == LXMF_DELIVERY_DELIVERED && got.content.len == 17u);

    /* Nothing is left to re-check, and the promotion survives a restart. */
    assert(lxmf_router_verify_pending(&router, NULL, &result) == LXMF_OK);
    assert(result.examined == 0u && log.count == 2u);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_read(&store, alice_message, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_VERIFIED);

    /* A known sender also retains full bytes for later metadata/media access,
     * even when there is no deferred signature check to perform. */
    length = seal(&alice, alice_hash, &bob, "announced hello", packet, sizeof packet);
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_OK);
    assert(inbox.count == 3u && inbox.state == LXMF_SIGNATURE_VERIFIED);
    uint8_t retained[512u];
    size_t retained_length = 0;
    assert(lxmf_store_read_packed(&store, inbox.id, retained, sizeof retained,
                                  &retained_length) == LXMF_OK);
    lxmf_message_t retained_message;
    assert(lxmf_unpack(retained, retained_length, NULL, NULL,
                       &retained_message) == LXMF_OK &&
           memcmp(retained_message.message_id, inbox.id, 32) == 0);

    lxmf_store_close(&store);
    unlink(path);
    return 0;
}
