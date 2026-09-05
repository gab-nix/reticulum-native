#define _POSIX_C_SOURCE 200809L
#include "reticulum/lxmf_router.h"
#include "reticulum/lxmf_fields.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    rns_identity peer;
    uint8_t destination[16];
    bool known;
    size_t received, delivered, resource_parts, resource_segments;
    bool failed, metadata_failed, resource;
    bool has_event_snapshot;
    lxmf_delivery_status_t event_state;
    lxmf_queue_reason_t event_reason;
    lxmf_status_t event_result;
    uint32_t event_attempt;
    uint8_t event_message_id[LXMF_MESSAGE_ID_LENGTH];
} state_t;
static uint64_t wall_seconds(void *context) {
    uint64_t now = 0; (void)context;
    (void)rns_hal_wallclock_ms(&now); return now / 1000u;
}
static bool stamp_cost(void *context, const uint8_t destination[16], uint8_t *cost) {
    state_t *s = context; (void)destination;
    *cost = s->received > 0 ? 1u : 0u; return true;
}
static void make_body(uint8_t *body, size_t length, bool from_python) {
    uint32_t seed = from_python ? 0x2468ace1U : 0x13579bdfU;
    for (size_t i = 0; i < length; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        body[i] = length < 100 ? (uint8_t)((from_python ? 'a' : 'A') + i % 26)
                              : (uint8_t)(33U + seed % 90U);
    }
}
static const uint8_t fields[] = {0x81, 0xcd, 0x12, 0x34, 0xc4, 0x03, 1, 2, 3};
#define SEGMENTED_FIELD_SIZE (RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE + 4096u)
static bool segmented_fields_valid(const uint8_t *data, size_t length) {
    const uint8_t prefix[9] = {0x81, 0xcd, 0x12, 0x34, 0xc6,
        (uint8_t)(SEGMENTED_FIELD_SIZE >> 24u),
        (uint8_t)(SEGMENTED_FIELD_SIZE >> 16u),
        (uint8_t)(SEGMENTED_FIELD_SIZE >> 8u), (uint8_t)SEGMENTED_FIELD_SIZE};
    if (data == NULL || length != SEGMENTED_FIELD_SIZE + sizeof prefix ||
        memcmp(data, prefix, sizeof prefix) != 0)
        return false;
    uint32_t seed = 0x6d2b79f5u;
    for (size_t i = sizeof prefix; i < length; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        if (data[i] != (uint8_t)seed) return false;
    }
    return true;
}
static void hex(const uint8_t *input, size_t length) {
    for (size_t i = 0; i < length; ++i) printf("%02x", input[i]);
}
static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    state_t *s = context;
    return s->known && memcmp(hash, s->destination, 16) == 0 ? &s->peer : NULL;
}
static void announce(rns_runtime_t *runtime, const rns_node_result *event,
                     void *context) {
    state_t *s = context; uint8_t hash[16];
    const char *aspects[] = {"delivery"}; (void)runtime;
    if (!event->has_verified_announce ||
        !rns_destination_hash(&event->announce_identity, "lxmf", aspects, 1, hash) ||
        memcmp(hash, event->destination_hash, 16) != 0) return;
    if (!s->known) puts("{\"event\":\"peer_verified\"}");
    s->peer = event->announce_identity;
    memcpy(s->destination, hash, 16); s->known = true;
}
static void received(void *context, const lxmf_store_message_t *message) {
    state_t *s = context;
    size_t expected = s->received == 0 ? 17 : s->received == 1 ? 2048 : 23;
    if (message->signature_state != LXMF_SIGNATURE_VERIFIED ||
        message->delivery.actual_method != LXMF_DELIVERY_METHOD_DIRECT ||
        message->content.len != expected ||
        memcmp(message->source, s->destination, 16) != 0) { s->failed = true; return; }
    uint8_t expected_body[2048]; make_body(expected_body, expected, true);
    if (memcmp(message->content.data, expected_body, expected) != 0) s->failed = true;
    lxmf_message_t unpacked = {0};
    uint8_t *plain_fields = malloc(message->packed.len);
    size_t plain_length = 0;
    if (lxmf_unpack(message->packed.data, message->packed.len, NULL, NULL,
        &unpacked) != LXMF_OK || plain_fields == NULL ||
        !unpacked.has_stamp || unpacked.stamp_len != LXMF_STAMP_LENGTH ||
        lxmf_fields_merge_ticket(unpacked.fields_msgpack.data, unpacked.fields_msgpack.len,
            NULL, plain_fields, message->packed.len, &plain_length) != LXMF_OK ||
        unpacked.title.len != 11 ||
        memcmp(unpacked.title.data, "python-live", 11) != 0 ||
        (s->received < 2 &&
         (plain_length != sizeof(fields) ||
          memcmp(plain_fields, fields, sizeof(fields)) != 0)) ||
        (s->received == 2 &&
         !segmented_fields_valid(plain_fields, plain_length)))
        s->metadata_failed = true;
    lxmf_ticket_field_t ticket = {0};
    if (lxmf_fields_parse_ticket(unpacked.fields_msgpack.data, unpacked.fields_msgpack.len,
        &ticket) != LXMF_OK || ticket.present != (s->received == 0)) s->metadata_failed = true;
    printf("{\"event\":\"metadata\",\"index\":%zu,\"stamp_length\":%zu,"
           "\"title_length\":%zu,\"fields_length\":%zu,\"ticket_present\":%s,"
           "\"fields_match\":%s}\n", s->received, unpacked.stamp_len,
           unpacked.title.len, plain_length, ticket.present ? "true" : "false",
           plain_fields != NULL && (s->received < 2
               ? plain_length == sizeof fields && memcmp(plain_fields, fields, sizeof fields) == 0
               : segmented_fields_valid(plain_fields, plain_length)) ? "true" : "false");
    rns_hal_secure_zero(&ticket, sizeof ticket);
    free(plain_fields);
    s->received++;
    printf("{\"event\":\"received\",\"size\":%zu,\"verified\":%s,\"id\":\"",
        expected, s->failed ? "false" : "true"); hex(message->message_id, 32); puts("\"}");
}
static void changed(void *context, const uint8_t id[32],
    lxmf_delivery_status_t status, lxmf_status_t result) {
    state_t *s = context;
    if (status == LXMF_DELIVERY_DELIVERED) s->delivered++;
    if (status == LXMF_DELIVERY_FAILED) s->failed = true;
    printf("{\"event\":\"state\",\"state\":%d,\"result\":%d,\"id\":\"",
        (int)status, (int)result); hex(id, 32); puts("\"}");
}
static void event(void *context, const lxmf_router_event_t *ev) {
    state_t *s = context;
    if (ev->queue_reason == LXMF_QUEUE_REASON_RESOURCE) s->resource = true;
    if (s->has_event_snapshot && s->event_state == ev->state &&
        s->event_reason == ev->queue_reason &&
        s->event_result == ev->result && s->event_attempt == ev->attempt &&
        memcmp(s->event_message_id, ev->message_id,
               sizeof s->event_message_id) == 0)
        return;
    s->has_event_snapshot = true;
    s->event_state = ev->state;
    s->event_reason = ev->queue_reason;
    s->event_result = ev->result;
    s->event_attempt = ev->attempt;
    memcpy(s->event_message_id, ev->message_id,
           sizeof s->event_message_id);
    printf("{\"event\":\"router\",\"state\":%d,\"result\":%d,"
           "\"reason\":%d,\"attempt\":%u,\"id\":\"",
           (int)ev->state, (int)ev->result, (int)ev->queue_reason,
           ev->attempt);
    hex(ev->message_id, 32); puts("\"}");
}
static bool queue(lxmf_store_t *store, const rns_identity *identity,
                  const uint8_t destination[16], size_t length,
                  lxmf_ticket_store_t *tickets) {
    uint8_t body[2048], packed[2400], source[16];
    const char *aspects[] = {"delivery"};
    if (!rns_destination_hash(identity, "lxmf", aspects, 1, source)) return false;
    make_body(body, length, false);
    lxmf_message_t m = {0}, decoded;
    memcpy(m.destination, destination, 16); memcpy(m.source, source, 16);
    uint64_t now; if (rns_hal_wallclock_ms(&now) != RNS_OK) return false;
    m.timestamp = (double)now / 1000.0;
    m.title = (lxmf_slice_t){(const uint8_t *)"c-live", 6};
    m.content = (lxmf_slice_t){body, length};
    m.fields_msgpack = (lxmf_slice_t){fields, sizeof(fields)};
    uint8_t ticket_fields[64];
    if (length == 17u) {
        lxmf_ticket_entry_t entry = {0};
        if (lxmf_ticket_store_issue(tickets, destination, now / 1000u, &entry, NULL) != LXMF_OK)
            return false;
        lxmf_ticket_field_t ticket = {.present = true, .expires_at = entry.expires_at};
        memcpy(ticket.ticket, entry.ticket, sizeof ticket.ticket);
        size_t field_length = 0;
        lxmf_status_t status = lxmf_fields_merge_ticket(fields, sizeof fields, &ticket,
            ticket_fields, sizeof ticket_fields, &field_length);
        rns_hal_secure_zero(&ticket, sizeof ticket); rns_hal_secure_zero(&entry, sizeof entry);
        if (status != LXMF_OK) return false;
        m.fields_msgpack = (lxmf_slice_t){ticket_fields, field_length};
    }
    size_t size;
    if (lxmf_pack(&m, lxmf_identity_signer, (void *)identity, packed,
        sizeof(packed), &size) != LXMF_OK ||
        lxmf_unpack(packed, size, NULL, NULL, &decoded) != LXMF_OK) return false;
    lxmf_store_message_t record = {0};
    memcpy(record.destination, destination, 16); memcpy(record.source, source, 16);
    memcpy(record.message_id, decoded.message_id, 32);
    record.timestamp = m.timestamp; record.status = LXMF_DELIVERY_QUEUED;
    record.signature_state = LXMF_SIGNATURE_VERIFIED;
    record.content = m.content; record.packed = (lxmf_slice_t){packed, size};
    bool inserted;
    return lxmf_store_put(store, &record, &inserted) == LXMF_OK && inserted;
}

static bool queue_segmented(lxmf_store_t *store, const rns_identity *identity,
                            const uint8_t destination[16]) {
    const size_t fields_length = SEGMENTED_FIELD_SIZE + 9u;
    uint8_t *large_fields = malloc(fields_length);
    uint8_t *packed = malloc(fields_length + 256u);
    if (large_fields == NULL || packed == NULL) {
        free(large_fields); free(packed); return false;
    }
    const uint8_t prefix[9] = {0x81, 0xcd, 0x12, 0x34, 0xc6,
        (uint8_t)(SEGMENTED_FIELD_SIZE >> 24u),
        (uint8_t)(SEGMENTED_FIELD_SIZE >> 16u),
        (uint8_t)(SEGMENTED_FIELD_SIZE >> 8u), (uint8_t)SEGMENTED_FIELD_SIZE};
    memcpy(large_fields, prefix, sizeof prefix);
    uint32_t seed = 0x6d2b79f5u;
    for (size_t i = sizeof prefix; i < fields_length; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        large_fields[i] = (uint8_t)seed;
    }
    uint8_t body[23], source[16]; make_body(body, sizeof body, false);
    const char *aspects[] = {"delivery"};
    if (!rns_destination_hash(identity, "lxmf", aspects, 1, source)) {
        free(large_fields); free(packed); return false;
    }
    uint64_t now; lxmf_message_t m = {0}, decoded;
    if (rns_hal_wallclock_ms(&now) != RNS_OK) {
        free(large_fields); free(packed); return false;
    }
    memcpy(m.destination, destination, 16); memcpy(m.source, source, 16);
    m.timestamp = (double)now / 1000.0;
    m.title = (lxmf_slice_t){(const uint8_t *)"c-live", 6};
    m.content = (lxmf_slice_t){body, sizeof body};
    m.fields_msgpack = (lxmf_slice_t){large_fields, fields_length};
    size_t size = 0; bool ok = lxmf_pack(&m, lxmf_identity_signer,
        (void *)identity, packed, fields_length + 256u, &size) == LXMF_OK &&
        size > RNS_RESOURCE_SINGLE_SEGMENT_MAX_SIZE &&
        lxmf_unpack(packed, size, NULL, NULL, &decoded) == LXMF_OK;
    if (ok) {
        lxmf_store_message_t record = {0}; bool inserted = false;
        memcpy(record.destination, destination, 16); memcpy(record.source, source, 16);
        memcpy(record.message_id, decoded.message_id, 32);
        record.timestamp = m.timestamp; record.status = LXMF_DELIVERY_QUEUED;
        record.signature_state = LXMF_SIGNATURE_VERIFIED;
        record.content = m.content; record.packed = (lxmf_slice_t){packed, size};
        ok = lxmf_store_put(store, &record, &inserted) == LXMF_OK && inserted;
    }
    free(large_fields); free(packed); return ok;
}
static bool parse_port(const char *text, uint16_t *port) {
    char *end; errno = 0; unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > 65535) return false;
    *port = (uint16_t)value; return true;
}
int main(int argc, char **argv) {
    uint16_t listen = 0U, forward = 0U;
    bool tcp = argc == 4 && strcmp(argv[1], "--tcp") == 0;
    if (argc != 4 || (tcp ? !parse_port(argv[2], &forward)
                          : (!parse_port(argv[1], &listen) ||
                             !parse_port(argv[2], &forward)))) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    state_t state = {0}; rns_identity identity;
    if (!rns_identity_generate(&identity)) return 3;
    rns_config_t config; rns_config_init(&config); config.interface_count = 1;
    rns_config_interface_t *interface = &config.interfaces[0];
    strcpy(interface->name, "python-direct-test");
    interface->type = tcp ? RNS_CONFIG_TCP_CLIENT : RNS_CONFIG_UDP;
    interface->type_set = true; interface->enabled = true;
    if (tcp) {
        strcpy(interface->target_host, "127.0.0.1");
        interface->target_port = forward;
    } else {
        strcpy(interface->listen_ip, "127.0.0.1");
        strcpy(interface->forward_ip, "127.0.0.1");
        interface->listen_port = listen; interface->forward_port = forward;
    }
    rns_runtime_options_t ro = {0}; ro.announce_callback = announce;
    ro.callback_context = &state; rns_runtime_t *runtime = NULL;
    if (rns_runtime_create(&runtime, &config, &ro) != RNS_OK) return 4;
    const char *path = argv[3]; lxmf_store_t store = {0};
    if (lxmf_store_open(&store, path) != LXMF_OK) { rns_runtime_destroy(runtime); return 5; }
    lxmf_router_t router;
    char ticket_path[1200]; lxmf_ticket_store_t *tickets = NULL;
    int ticket_path_size = snprintf(ticket_path, sizeof ticket_path, "%s.tickets", path);
    if (ticket_path_size < 0 || (size_t)ticket_path_size >= sizeof ticket_path ||
        lxmf_ticket_store_open(&tickets, ticket_path) != LXMF_OK) return 6;
    lxmf_router_config_t options = {0};
    options.identity = &identity; options.store = &store; options.runtime = runtime;
    options.resolve_identity = resolve; options.resolve_context = &state;
    options.message_callback = received; options.message_context = &state;
    options.delivery_callback = changed; options.delivery_context = &state;
    options.event_callback = event; options.event_context = &state;
    options.preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT;
    options.accept_inbound_links = true;
    options.ticket_store = tickets; options.wall_clock = wall_seconds;
    options.inbound_stamp_cost = 1u;
    options.resolve_stamp_cost = stamp_cost; options.stamp_cost_context = &state;
    if (lxmf_router_init(&router, &options) != LXMF_OK) return 6;
    uint8_t destination[16]; const char *aspects[] = {"delivery"};
    if (!rns_destination_hash(&identity, "lxmf", aspects, 1, destination)) return 7;
    printf("{\"event\":\"ready\",\"destination\":\""); hex(destination, 16); puts("\"}");
    uint64_t start, now, last_announce = 0; size_t queued = 0;
    if (rns_hal_monotonic_ms(&start) != RNS_OK) return 8;
    do {
        if (rns_hal_monotonic_ms(&now) != RNS_OK) break;
        if (last_announce == 0 || now - last_announce > 3000) {
            (void)rns_runtime_announce(runtime, &identity, "lxmf", aspects, 1, NULL, 0);
            last_announce = now;
        }
        if (state.known && queued < 3 && state.delivered == queued && state.received == queued) {
            bool queued_ok = queued < 2
                ? queue(&store, &identity, state.destination,
                        queued == 0 ? 17 : 2048, tickets)
                : queue_segmented(&store, &identity, state.destination);
            if (!queued_ok) {
                state.failed = true; break;
            }
            queued++;
        }
        lxmf_router_poll_result_t result;
        size_t processed;
        if (rns_runtime_poll(runtime, 32, &processed) != RNS_OK) { state.failed = true; break; }
        if (lxmf_router_poll(&router, 4, &result) != LXMF_OK) { state.failed = true; break; }
        for (size_t i = 0; i < LXMF_ROUTER_MAX_RESOURCES; ++i) {
            if (!router.resources[i].used) continue;
            size_t parts = rns_runtime_resource_transfer_total_parts(router.resources[i].transfer);
            if (parts > state.resource_parts) state.resource_parts = parts;
            rns_runtime_resource_progress_t progress;
            if (rns_runtime_resource_transfer_progress(
                    router.resources[i].transfer, &progress) == RNS_OK &&
                progress.total_segments > state.resource_segments)
                state.resource_segments = progress.total_segments;
        }
        if (state.failed || (state.received == 3 && state.delivered == 3)) break;
        (void)rns_hal_sleep_ms(5);
    } while (now - start < 300000);
    bool success = !state.failed && !state.metadata_failed && state.received == 3 && state.delivered == 3 && state.resource && state.resource_parts > 1 && state.resource_segments > 1;
    printf("{\"event\":\"done\",\"ok\":%s,\"received\":%zu,\"proved\":%zu,\"resource\":%s,\"metadata_retained\":%s,\"resource_parts\":%zu,\"resource_segments\":%zu}\n",
        success ? "true" : "false", state.received, state.delivered, state.resource ? "true" : "false",
        state.metadata_failed ? "false" : "true", state.resource_parts,
        state.resource_segments);
    /* Drain final Resource proof before shutting down. */
    (void)rns_hal_sleep_ms(100);
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    lxmf_ticket_store_close(tickets); unlink(ticket_path);
    rns_runtime_destroy(runtime); unlink(path);
    return success ? 0 : 1;
}
