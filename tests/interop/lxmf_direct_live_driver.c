#define _POSIX_C_SOURCE 200809L
#include "reticulum/lxmf_router.h"
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
    size_t received, delivered, resource_parts;
    bool failed, metadata_failed, resource;
} state_t;
static void make_body(uint8_t *body, size_t length, bool from_python) {
    uint32_t seed = from_python ? 0x2468ace1U : 0x13579bdfU;
    for (size_t i = 0; i < length; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        body[i] = length < 100 ? (uint8_t)((from_python ? 'a' : 'A') + i % 26)
                              : (uint8_t)(33U + seed % 90U);
    }
}
static const uint8_t fields[] = {0x81, 0xcd, 0x12, 0x34, 0xc4, 0x03, 1, 2, 3};
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
    size_t expected = s->received == 0 ? 17 : 2048;
    if (message->signature_state != LXMF_SIGNATURE_VERIFIED ||
        message->delivery.actual_method != LXMF_DELIVERY_METHOD_DIRECT ||
        message->content.len != expected ||
        memcmp(message->source, s->destination, 16) != 0) { s->failed = true; return; }
    uint8_t expected_body[2048]; make_body(expected_body, expected, true);
    if (memcmp(message->content.data, expected_body, expected) != 0) s->failed = true;
    lxmf_message_t unpacked;
    if (lxmf_unpack(message->packed.data, message->packed.len, NULL, NULL,
        &unpacked) != LXMF_OK || unpacked.title.len != 11 ||
        memcmp(unpacked.title.data, "python-live", 11) != 0 ||
        unpacked.fields_msgpack.len != sizeof(fields) ||
        memcmp(unpacked.fields_msgpack.data, fields, sizeof(fields)) != 0)
        s->metadata_failed = true;
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
}
static bool queue(lxmf_store_t *store, const rns_identity *identity,
                  const uint8_t destination[16], size_t length) {
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
static bool parse_port(const char *text, uint16_t *port) {
    char *end; errno = 0; unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > 65535) return false;
    *port = (uint16_t)value; return true;
}
int main(int argc, char **argv) {
    uint16_t listen, forward;
    if (argc != 4 || !parse_port(argv[1], &listen) || !parse_port(argv[2], &forward))
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    state_t state = {0}; rns_identity identity;
    if (!rns_identity_generate(&identity)) return 3;
    rns_config_t config; rns_config_init(&config); config.interface_count = 1;
    rns_config_interface_t *interface = &config.interfaces[0];
    strcpy(interface->name, "python-direct-test"); interface->type = RNS_CONFIG_UDP;
    interface->type_set = true; interface->enabled = true;
    strcpy(interface->listen_ip, "127.0.0.1"); strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen; interface->forward_port = forward;
    rns_runtime_options_t ro = {0}; ro.announce_callback = announce;
    ro.callback_context = &state; rns_runtime_t *runtime = NULL;
    if (rns_runtime_create(&runtime, &config, &ro) != RNS_OK) return 4;
    const char *path = argv[3]; lxmf_store_t store = {0};
    if (lxmf_store_open(&store, path) != LXMF_OK) { rns_runtime_destroy(runtime); return 5; }
    lxmf_router_t router;
    lxmf_router_config_t options = {0};
    options.identity = &identity; options.store = &store; options.runtime = runtime;
    options.resolve_identity = resolve; options.resolve_context = &state;
    options.message_callback = received; options.message_context = &state;
    options.delivery_callback = changed; options.delivery_context = &state;
    options.event_callback = event; options.event_context = &state;
    options.preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT;
    options.accept_inbound_links = true;
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
        if (state.known && queued < 2 && state.delivered == queued) {
            if (!queue(&store, &identity, state.destination, queued == 0 ? 17 : 2048)) {
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
        }
        if (state.failed || (state.received == 2 && state.delivered == 2)) break;
        (void)rns_hal_sleep_ms(5);
    } while (now - start < 90000);
    bool success = !state.failed && !state.metadata_failed && state.received == 2 && state.delivered == 2 && state.resource && state.resource_parts > 1;
    printf("{\"event\":\"done\",\"ok\":%s,\"received\":%zu,\"proved\":%zu,\"resource\":%s,\"metadata_retained\":%s,\"resource_parts\":%zu}\n",
        success ? "true" : "false", state.received, state.delivered, state.resource ? "true" : "false",
        state.metadata_failed ? "false" : "true", state.resource_parts);
    /* Drain final Resource proof before shutting down. */
    (void)rns_hal_sleep_ms(100);
    lxmf_router_destroy(&router); lxmf_store_close(&store);
    rns_runtime_destroy(runtime); unlink(path);
    return success ? 0 : 1;
}
