#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "reticulum/destination.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/packet.h"

typedef struct {
    const rns_identity *peer;
    uint8_t peer_hash[LXMF_SOURCE_LENGTH];
    uint64_t now;
    size_t messages;
    size_t events;
    lxmf_router_event_t event;
    uint8_t packet[RNS_MTU];
    size_t packet_length;
    bool blocked;
} state_t;

static bool source_is_blocked(void *context, const uint8_t hash[16]) {
    state_t *state = context;
    return state->blocked && memcmp(hash, state->peer_hash, 16) == 0;
}

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    state_t *state = context;
    return memcmp(state->peer_hash, hash, 16u) == 0 ? state->peer : NULL;
}

static uint64_t wall_clock(void *context) {
    return ((state_t *)context)->now;
}

static bool peer_cost(void *context, const uint8_t hash[16], uint8_t *cost) {
    (void)context;
    (void)hash;
    *cost = 20u;
    return true;
}

static lxmf_status_t no_send(void *context, const uint8_t *packet,
                             size_t packet_length) {
    (void)context;
    state_t *state = context;
    assert(packet_length <= sizeof state->packet);
    memcpy(state->packet, packet, packet_length);
    state->packet_length = packet_length;
    return LXMF_OK;
}

static void received(void *context, const lxmf_store_message_t *message) {
    state_t *state = context;
    assert(message->signature_state == LXMF_SIGNATURE_VERIFIED);
    state->messages++;
}

static void event(void *context, const lxmf_router_event_t *delivery_event) {
    state_t *state = context;
    state->events++;
    state->event = *delivery_event;
}

static void delivery_hash(const rns_identity *identity, uint8_t hash[16]) {
    static const char *const aspects[] = {"delivery"};
    assert(rns_destination_hash(identity, "lxmf", aspects, 1u, hash));
}

static void store64be(uint8_t output[8], uint64_t value) {
    for (size_t i = 0u; i < 8u; ++i)
        output[7u - i] = (uint8_t)(value >> (8u * i));
}

static size_t ticket_fields(uint64_t expiry, const uint8_t ticket[16],
                            uint8_t output[30]) {
    double as_double = (double)expiry;
    uint64_t bits;
    memcpy(&bits, &as_double, sizeof bits);
    output[0] = 0x81u;
    output[1] = LXMF_FIELD_TICKET;
    output[2] = 0x92u;
    output[3] = 0xcbu;
    store64be(output + 4u, bits);
    output[12] = 0xc4u;
    output[13] = LXMF_TICKET_LENGTH;
    memcpy(output + 14u, ticket, LXMF_TICKET_LENGTH);
    return 30u;
}

typedef enum { NO_STAMP, TICKET_STAMP, POW_STAMP } stamp_kind_t;

static size_t make_packet(const rns_identity *sender,
                          const rns_identity *recipient, double timestamp,
                          const uint8_t *fields, size_t fields_length,
                          stamp_kind_t kind, const uint8_t ticket[16],
                          uint8_t packet[RNS_MTU], uint8_t message_id[32]) {
    lxmf_message_t message = {.timestamp = timestamp};
    message.content = (lxmf_slice_t){(const uint8_t *)"hello", 5u};
    message.fields_msgpack = (lxmf_slice_t){fields, fields_length};
    delivery_hash(sender, message.source);
    delivery_hash(recipient, message.destination);

    uint8_t base[512u];
    size_t base_length = 0u;
    assert(lxmf_pack(&message, lxmf_identity_signer, (void *)sender, base,
                     sizeof base, &base_length) == LXMF_OK);
    lxmf_message_t parsed;
    assert(lxmf_unpack(base, base_length, NULL, NULL, &parsed) == LXMF_OK);
    memcpy(message_id, parsed.message_id, LXMF_MESSAGE_ID_LENGTH);
    if (kind == TICKET_STAMP) {
        assert(ticket != NULL);
        message.has_stamp = true;
        message.stamp_len = LXMF_STAMP_LENGTH;
        lxmf_ticket_stamp(ticket, message_id, message.stamp);
    } else if (kind == POW_STAMP) {
        message.has_stamp = true;
        message.stamp_len = LXMF_POW_STAMP_LENGTH;
        assert(lxmf_pow_stamp_generate(message_id, 1u, NULL, NULL,
                                       message.stamp, NULL, NULL) == LXMF_OK);
    }
    size_t packet_length = 0u;
    assert(lxmf_opportunistic_packet_pack(
               &message, sender, recipient, packet, RNS_MTU,
               &packet_length) == LXMF_OK);
    return packet_length;
}

int main(void) {
    char message_path[] = "/tmp/lxmf-router-stamps-msg-XXXXXX";
    char ticket_path[] = "/tmp/lxmf-router-stamps-ticket-XXXXXX";
    int descriptor = mkstemp(message_path);
    assert(descriptor >= 0);
    close(descriptor);
    unlink(message_path);
    descriptor = mkstemp(ticket_path);
    assert(descriptor >= 0);
    close(descriptor);
    unlink(ticket_path);

    uint8_t alice_private[64], bob_private[64];
    for (size_t i = 0u; i < sizeof alice_private; ++i) {
        alice_private[i] = (uint8_t)(i + 1u);
        bob_private[i] = (uint8_t)(i + 65u);
    }
    rns_identity alice, bob;
    assert(rns_identity_from_private(&alice, alice_private));
    assert(rns_identity_from_private(&bob, bob_private));

    lxmf_store_t messages = {0};
    lxmf_ticket_store_t *tickets = NULL;
    assert(lxmf_store_open(&messages, message_path) == LXMF_OK);
    assert(lxmf_ticket_store_open(&tickets, ticket_path) == LXMF_OK);
    state_t state = {.peer = &alice, .now = 1000u};
    delivery_hash(&alice, state.peer_hash);

    lxmf_router_t router;
    lxmf_router_config_t config = {
        .identity = &bob,
        .store = &messages,
        .ticket_store = tickets,
        .wall_clock = wall_clock,
        .wall_clock_context = &state,
        .inbound_stamp_cost = 1u,
        .is_source_blocked = source_is_blocked,
        .source_policy_context = &state,
        .resolve_identity = resolve,
        .resolve_context = &state,
        .send_packet = no_send,
        .send_context = &state,
        .message_callback = received,
        .message_context = &state,
        .event_callback = event,
        .event_context = &state};
    assert(lxmf_router_init(&router, &config) == LXMF_OK);

    lxmf_ticket_entry_t issued;
    bool created = false;
    assert(lxmf_ticket_store_issue(tickets, state.peer_hash, state.now,
                                   &issued, &created) == LXMF_OK && created);

    uint8_t packet[RNS_MTU], message_id[32];
    size_t length = make_packet(&alice, &bob, 1.0, NULL, 0u, NO_STAMP,
                                NULL, packet, message_id);
    assert(lxmf_router_receive_packet(&router, packet, length) ==
           LXMF_ERR_STAMP);
    assert(state.messages == 0u && lxmf_store_count(&messages) == 0u);
    assert(state.events == 1u && state.event.result == LXMF_ERR_STAMP &&
           state.event.queue_reason == LXMF_QUEUE_REASON_STAMP &&
           memcmp(state.event.message_id, message_id, 32u) == 0);

    length = make_packet(&alice, &bob, 2.0, NULL, 0u, TICKET_STAMP,
                         issued.ticket, packet, message_id);
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_OK);
    assert(state.messages == 1u && lxmf_store_count(&messages) == 1u);

    uint8_t wrong_ticket[16];
    memcpy(wrong_ticket, issued.ticket, sizeof wrong_ticket);
    wrong_ticket[0] ^= 1u;
    length = make_packet(&alice, &bob, 3.0, NULL, 0u, TICKET_STAMP,
                         wrong_ticket, packet, message_id);
    assert(lxmf_router_receive_packet(&router, packet, length) ==
           LXMF_ERR_STAMP);
    assert(state.messages == 1u && state.event.result == LXMF_ERR_STAMP &&
           state.event.queue_reason == LXMF_QUEUE_REASON_STAMP);

    length = make_packet(&alice, &bob, 4.0, NULL, 0u, POW_STAMP, NULL,
                         packet, message_id);
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_OK);
    assert(state.messages == 2u);

    uint8_t remote_ticket[16];
    memset(remote_ticket, 0xa5, sizeof remote_ticket);
    uint8_t fields[30];
    size_t fields_length = ticket_fields(state.now + 5000u, remote_ticket,
                                         fields);
    lxmf_ticket_field_t parsed_field;
    assert(lxmf_fields_parse_ticket(fields, fields_length, &parsed_field) ==
           LXMF_OK);
    assert(parsed_field.present && parsed_field.expires_at == state.now + 5000u &&
           memcmp(parsed_field.ticket, remote_ticket, 16u) == 0);
    length = make_packet(&alice, &bob, 5.0, fields, fields_length,
                         TICKET_STAMP, issued.ticket, packet, message_id);
    lxmf_ticket_entry_t remembered;
    state.blocked = true;
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_ERR_BLOCKED);
    assert(lxmf_ticket_store_get_outbound(tickets, state.peer_hash, state.now,
                                          &remembered) == LXMF_ERR_PENDING);
    state.blocked = false;
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_OK);
    assert(lxmf_ticket_store_get_outbound(tickets, state.peer_hash, state.now,
                                          &remembered) == LXMF_OK);
    assert(remembered.expires_at == state.now + 5000u &&
           memcmp(remembered.ticket, remote_ticket, 16u) == 0);

    /* A malformed optional ticket field is ignored, but cannot poison or
     * replace the valid ticket already retained for this peer. */
    const uint8_t malformed[] = {0x81u, LXMF_FIELD_TICKET, 0x92u, 0x01u,
                                 0xc4u, 0x0fu, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0};
    assert(lxmf_fields_parse_ticket(malformed, sizeof malformed,
                                    &parsed_field) == LXMF_ERR_FORMAT);
    length = make_packet(&alice, &bob, 6.0, malformed, sizeof malformed,
                         TICKET_STAMP, issued.ticket, packet, message_id);
    assert(lxmf_router_receive_packet(&router, packet, length) == LXMF_OK);
    assert(state.messages == 4u);
    assert(lxmf_ticket_store_get_outbound(tickets, state.peer_hash, state.now,
                                          &remembered) == LXMF_OK &&
           memcmp(remembered.ticket, remote_ticket, 16u) == 0);

    /* A ticket learned from a verified FIELD_TICKET is reused on a later
     * outbound message without losing title or unknown fields. */
    lxmf_router_destroy(&router);
    static const uint8_t outbound_fields[] = {0x81u, 0x2au, 0xa1u, 'x'};
    lxmf_message_t outbound = {.timestamp = 7.0};
    delivery_hash(&bob, outbound.source);
    delivery_hash(&alice, outbound.destination);
    outbound.title = (lxmf_slice_t){(const uint8_t *)"title", 5u};
    outbound.content = (lxmf_slice_t){(const uint8_t *)"reply", 5u};
    outbound.fields_msgpack = (lxmf_slice_t){outbound_fields,
                                              sizeof outbound_fields};
    uint8_t retained[512u];
    size_t retained_length = 0u;
    assert(lxmf_pack(&outbound, lxmf_identity_signer, &bob, retained,
                     sizeof retained, &retained_length) == LXMF_OK);
    lxmf_message_t retained_message;
    assert(lxmf_unpack(retained, retained_length, NULL, NULL,
                       &retained_message) == LXMF_OK);
    lxmf_store_message_t queued = {.timestamp = outbound.timestamp,
                                   .status = LXMF_DELIVERY_QUEUED,
                                   .content = outbound.content,
                                   .packed = {retained, retained_length}};
    memcpy(queued.message_id, retained_message.message_id, 32u);
    memcpy(queued.destination, outbound.destination, 16u);
    memcpy(queued.source, outbound.source, 16u);
    bool inserted = false;
    assert(lxmf_store_put(&messages, &queued, &inserted) == LXMF_OK && inserted);
    state.peer = &alice;
    delivery_hash(&alice, state.peer_hash);
    config.identity = &bob;
    config.inbound_stamp_cost = 0u;
    config.message_callback = NULL;
    config.event_callback = NULL;
    config.resolve_stamp_cost = peer_cost;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    assert(lxmf_router_send_message(&router, queued.message_id) == LXMF_OK);
    assert(router.stamp_job == NULL);
    assert(state.packet_length > 0u);
    uint8_t plaintext[RNS_MTU];
    size_t plaintext_length = 0u;
    lxmf_message_t sent;
    assert(lxmf_opportunistic_packet_unpack(
               state.packet, state.packet_length, &alice, NULL, NULL,
               plaintext, sizeof plaintext, &plaintext_length, &sent) ==
           LXMF_OK);
    assert(sent.has_stamp && sent.stamp_len == LXMF_STAMP_LENGTH &&
           lxmf_ticket_stamp_valid(sent.stamp, remote_ticket,
                                   sent.message_id));
    assert(sent.title.len == 5u && memcmp(sent.title.data, "title", 5u) == 0 &&
           sent.fields_msgpack.len == sizeof outbound_fields &&
           memcmp(sent.fields_msgpack.data, outbound_fields,
                  sizeof outbound_fields) == 0);
    assert(lxmf_store_read_packed(&messages, queued.message_id, retained,
        sizeof retained, &retained_length) == LXMF_OK);
    assert(lxmf_unpack(retained, retained_length, NULL, NULL,
                       &retained_message) == LXMF_OK &&
           retained_message.has_stamp &&
           retained_message.stamp_len == LXMF_STAMP_LENGTH);

    lxmf_router_destroy(&router);
    lxmf_ticket_store_close(tickets);
    lxmf_store_close(&messages);
    unlink(ticket_path);
    unlink(message_path);
    return 0;
}
