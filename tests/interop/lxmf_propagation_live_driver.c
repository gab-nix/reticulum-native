#define _POSIX_C_SOURCE 200809L
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf_propagation.h"
#include "reticulum/lxmf_router.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    rns_identity python_identity;
    uint8_t python_delivery[LXMF_DESTINATION_LENGTH];
    uint8_t propagation_destination[LXMF_DESTINATION_LENGTH];
    uint8_t propagation_cost;
    bool delivery_known;
    bool propagation_known;
    bool propagation_configured;
    bool outbound_queued;
    bool outbound_sent;
    bool inbound_received;
    bool sync_started;
    bool sync_complete;
    bool failed;
    uint8_t outbound_id[LXMF_MESSAGE_ID_LENGTH];
} live_state_t;

static const uint8_t fields[] = {
    0x81u, 0xcdu, 0x12u, 0x34u, 0xc4u, 0x03u, 0x01u, 0x02u, 0x03u};

static void make_body(uint8_t *body, size_t length, bool from_python) {
    uint32_t seed = from_python ? 0xa17e2c39u : 0x4d3b2a19u;
    for (size_t i = 0u; i < length; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        body[i] = (uint8_t)(33u + seed % 90u);
    }
}

static void print_hex(const uint8_t *input, size_t length) {
    for (size_t i = 0u; i < length; ++i) (void)printf("%02x", input[i]);
}

static bool parse_port(const char *text, uint16_t *port) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u ||
        value > 65535u)
        return false;
    *port = (uint16_t)value;
    return true;
}

static uint64_t wall_clock(void *context) {
    (void)context;
    uint64_t now = 0u;
    return rns_hal_wallclock_ms(&now) == RNS_OK ? now / 1000u : 0u;
}

static const rns_identity *resolve_identity(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH]) {
    live_state_t *state = context;
    return state->delivery_known &&
        memcmp(destination, state->python_delivery,
               LXMF_DESTINATION_LENGTH) == 0
        ? &state->python_identity : NULL;
}

static void announce_received(rns_runtime_t *runtime,
                              const rns_node_result *announce,
                              void *context) {
    live_state_t *state = context;
    static const char *const delivery[] = {"delivery"};
    static const char *const propagation[] = {"propagation"};
    uint8_t derived[LXMF_DESTINATION_LENGTH];
    (void)runtime;
    if (!announce->has_verified_announce) return;
    if (rns_destination_hash(&announce->announce_identity, "lxmf", delivery,
                             1u, derived) &&
        memcmp(derived, announce->destination_hash, sizeof derived) == 0) {
        state->python_identity = announce->announce_identity;
        memcpy(state->python_delivery, derived, sizeof derived);
        if (!state->delivery_known)
            (void)puts("{\"event\":\"python_delivery_verified\"}");
        state->delivery_known = true;
        return;
    }
    if (!rns_destination_hash(&announce->announce_identity, "lxmf",
                              propagation, 1u, derived) ||
        memcmp(derived, announce->destination_hash, sizeof derived) != 0)
        return;
    lxmf_pn_announce_t app_data;
    if (announce->announce_app_data == NULL ||
        lxmf_pn_announce_decode(announce->announce_app_data,
            announce->announce_app_data_length, &app_data) != LXMF_OK ||
        !app_data.enabled || app_data.stamp_cost == 0u ||
        app_data.stamp_cost == UINT8_MAX) {
        state->failed = true;
        (void)puts("{\"event\":\"invalid_propagation_announce\"}");
        return;
    }
    state->python_identity = announce->announce_identity;
    memcpy(state->propagation_destination, derived, sizeof derived);
    state->propagation_cost = app_data.stamp_cost;
    if (!state->propagation_known)
        (void)printf("{\"event\":\"propagation_verified\",\"cost\":%u}\n",
                     (unsigned)state->propagation_cost);
    state->propagation_known = true;
}

static void message_received(void *context,
                             const lxmf_store_message_t *message) {
    live_state_t *state = context;
    uint8_t expected[2048];
    make_body(expected, sizeof expected, true);
    lxmf_message_t unpacked;
    bool valid = message->signature_state == LXMF_SIGNATURE_VERIFIED &&
        message->delivery.actual_method == LXMF_DELIVERY_METHOD_PROPAGATED &&
        message->content.len == sizeof expected &&
        memcmp(message->content.data, expected, sizeof expected) == 0 &&
        memcmp(message->source, state->python_delivery,
               LXMF_SOURCE_LENGTH) == 0 &&
        lxmf_unpack(message->packed.data, message->packed.len, NULL, NULL,
                    &unpacked) == LXMF_OK &&
        unpacked.title.len == 9u &&
        memcmp(unpacked.title.data, "python-pn", 9u) == 0 &&
        unpacked.fields_msgpack.len == sizeof fields &&
        memcmp(unpacked.fields_msgpack.data, fields, sizeof fields) == 0;
    if (!valid) state->failed = true;
    else state->inbound_received = true;
    (void)printf("{\"event\":\"sync_received\",\"valid\":%s,\"size\":%zu,\"id\":\"",
                 valid ? "true" : "false", message->content.len);
    print_hex(message->message_id, LXMF_MESSAGE_ID_LENGTH);
    (void)puts("\"}");
}

static void delivery_changed(
    void *context, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_delivery_status_t status, lxmf_status_t result) {
    live_state_t *state = context;
    if (!state->outbound_queued ||
        memcmp(message_id, state->outbound_id, LXMF_MESSAGE_ID_LENGTH) != 0)
        return;
    if (status == LXMF_DELIVERY_SENT && result == LXMF_OK)
        state->outbound_sent = true;
    if (status == LXMF_DELIVERY_FAILED) state->failed = true;
    if (status == LXMF_DELIVERY_SENT || status == LXMF_DELIVERY_FAILED) {
        (void)printf("{\"event\":\"upload_state\",\"state\":%d,\"result\":%d,\"id\":\"",
                     (int)status, (int)result);
        print_hex(message_id, LXMF_MESSAGE_ID_LENGTH);
        (void)puts("\"}");
    }
}

static bool queue_upload(lxmf_store_t *store, const rns_identity *identity,
                         const uint8_t destination[LXMF_DESTINATION_LENGTH],
                         uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]) {
    uint8_t body[257], packed[640], source[LXMF_SOURCE_LENGTH];
    static const char *const aspects[] = {"delivery"};
    if (!rns_destination_hash(identity, "lxmf", aspects, 1u, source))
        return false;
    make_body(body, sizeof body, false);
    lxmf_message_t message = {0}, decoded;
    memcpy(message.destination, destination, LXMF_DESTINATION_LENGTH);
    memcpy(message.source, source, LXMF_SOURCE_LENGTH);
    uint64_t now = 0u;
    if (rns_hal_wallclock_ms(&now) != RNS_OK) return false;
    message.timestamp = (double)now / 1000.0;
    message.title = (lxmf_slice_t){(const uint8_t *)"c-pn", 4u};
    message.content = (lxmf_slice_t){body, sizeof body};
    message.fields_msgpack = (lxmf_slice_t){fields, sizeof fields};
    size_t packed_length = 0u;
    if (lxmf_pack(&message, lxmf_identity_signer, (void *)identity,
                  packed, sizeof packed, &packed_length) != LXMF_OK ||
        lxmf_unpack(packed, packed_length, NULL, NULL, &decoded) != LXMF_OK)
        return false;
    memcpy(message_id, decoded.message_id, LXMF_MESSAGE_ID_LENGTH);
    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, message_id, LXMF_MESSAGE_ID_LENGTH);
    memcpy(stored.destination, destination, LXMF_DESTINATION_LENGTH);
    memcpy(stored.source, source, LXMF_SOURCE_LENGTH);
    stored.timestamp = message.timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.signature_state = LXMF_SIGNATURE_VERIFIED;
    stored.content = message.content;
    stored.packed = (lxmf_slice_t){packed, packed_length};
    stored.delivery.desired_method = LXMF_DELIVERY_METHOD_PROPAGATED;
    bool inserted = false;
    return lxmf_store_put(store, &stored, &inserted) == LXMF_OK && inserted;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    uint16_t listen_port = 0u, forward_port = 0u;
    if (!parse_port(argv[1], &listen_port) ||
        !parse_port(argv[2], &forward_port))
        return 2;
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    live_state_t state = {0};
    rns_identity identity;
    if (!rns_identity_generate(&identity)) return 3;
    static const char *const delivery_aspects[] = {"delivery"};
    uint8_t local_destination[LXMF_DESTINATION_LENGTH];
    if (!rns_destination_hash(&identity, "lxmf", delivery_aspects, 1u,
                              local_destination))
        return 3;

    rns_config_t config;
    rns_config_init(&config);
    config.interface_count = 1u;
    rns_config_interface_t *interface = &config.interfaces[0];
    (void)strcpy(interface->name, "python-propagation-test");
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = true;
    interface->enabled = true;
    (void)strcpy(interface->listen_ip, "127.0.0.1");
    (void)strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = listen_port;
    interface->forward_port = forward_port;
    rns_runtime_options_t runtime_options = {0};
    runtime_options.announce_callback = announce_received;
    runtime_options.callback_context = &state;
    rns_runtime_t *runtime = NULL;
    if (rns_runtime_create(&runtime, &config, &runtime_options) != RNS_OK)
        return 4;
    lxmf_store_t store = {0};
    if (lxmf_store_open(&store, argv[3]) != LXMF_OK) {
        rns_runtime_destroy(runtime);
        return 5;
    }
    lxmf_router_config_t router_options = {0};
    router_options.identity = &identity;
    router_options.store = &store;
    router_options.runtime = runtime;
    router_options.resolve_identity = resolve_identity;
    router_options.resolve_context = &state;
    router_options.wall_clock = wall_clock;
    router_options.message_callback = message_received;
    router_options.message_context = &state;
    router_options.delivery_callback = delivery_changed;
    router_options.delivery_context = &state;
    router_options.preferred_delivery_method = LXMF_DELIVERY_METHOD_PROPAGATED;
    router_options.propagation_retry_limit = 2u;
    router_options.propagation_retry_base_ms = 100u;
    lxmf_router_t router;
    if (lxmf_router_init(&router, &router_options) != LXMF_OK) return 6;

    lxmf_announce_data_t announce_data = {0};
    (void)memcpy(announce_data.display_name, "C propagation test", 18u);
    announce_data.display_name_len = 18u;
    uint8_t app_data[256];
    size_t app_data_length = 0u;
    if (lxmf_announce_encode(&announce_data, app_data, sizeof app_data,
                             &app_data_length) != LXMF_OK)
        return 7;
    (void)printf("{\"event\":\"ready\",\"destination\":\"");
    print_hex(local_destination, sizeof local_destination);
    (void)puts("\"}");

    uint64_t start = 0u, now = 0u, last_announce = 0u;
    if (rns_hal_monotonic_ms(&start) != RNS_OK) return 8;
    do {
        if (rns_hal_monotonic_ms(&now) != RNS_OK) {
            state.failed = true;
            break;
        }
        if (last_announce == 0u || now - last_announce >= 3000u) {
            if (rns_runtime_announce(runtime, &identity, "lxmf",
                    delivery_aspects, 1u, app_data, app_data_length) != RNS_OK)
                state.failed = true;
            last_announce = now;
        }
        size_t processed = 0u;
        if (rns_runtime_poll(runtime, 32u, &processed) != RNS_OK) {
            state.failed = true;
            break;
        }
        if (state.propagation_known && !state.propagation_configured) {
            if (lxmf_router_set_propagation_node(
                    &router, &state.python_identity,
                    state.propagation_destination,
                    state.propagation_cost) != LXMF_OK) {
                state.failed = true;
                break;
            }
            state.propagation_configured = true;
        }
        if (state.propagation_configured && state.delivery_known &&
            !state.outbound_queued) {
            if (!queue_upload(&store, &identity, state.python_delivery,
                              state.outbound_id)) {
                state.failed = true;
                break;
            }
            state.outbound_queued = true;
            (void)printf("{\"event\":\"upload_queued\",\"id\":\"");
            print_hex(state.outbound_id, LXMF_MESSAGE_ID_LENGTH);
            (void)puts("\"}");
        }
        lxmf_router_poll_result_t poll_result;
        if (lxmf_router_poll(&router, 4u, &poll_result) != LXMF_OK) {
            state.failed = true;
            break;
        }
        if (state.outbound_sent && !state.sync_started) {
            if (lxmf_router_propagation_sync_start(&router, false) == LXMF_OK)
                state.sync_started = true;
        }
        if (state.sync_started && !state.sync_complete) {
            lxmf_router_propagation_sync_status_t sync;
            if (lxmf_router_propagation_sync_status(&router, &sync) != LXMF_OK) {
                state.failed = true;
                break;
            }
            if (!sync.active && sync.state != LXMF_PN_IDLE) {
                bool valid = sync.state == LXMF_PN_COMPLETE &&
                    sync.result == LXMF_OK && sync.available == 1u &&
                    sync.accepted == 1u && sync.duplicates == 0u &&
                    sync.rejected == 0u && sync.acknowledged == 1u;
                (void)printf("{\"event\":\"sync_done\",\"valid\":%s,\"state\":%d,\"result\":%d,\"available\":%zu,\"accepted\":%zu,\"duplicates\":%zu,\"rejected\":%zu,\"acknowledged\":%zu}\n",
                    valid ? "true" : "false", (int)sync.state,
                    (int)sync.result, sync.available, sync.accepted,
                    sync.duplicates, sync.rejected, sync.acknowledged);
                state.sync_complete = valid;
                if (!valid) state.failed = true;
            }
        }
        if (state.failed || (state.outbound_sent && state.inbound_received &&
                             state.sync_complete))
            break;
        (void)rns_hal_sleep_ms(2u);
    } while (now - start < 180000u);

    bool success = !state.failed && state.outbound_sent &&
        state.inbound_received && state.sync_complete;
    (void)printf("{\"event\":\"done\",\"ok\":%s,\"upload_sent\":%s,\"sync_received\":%s,\"sync_acknowledged\":%s}\n",
                 success ? "true" : "false",
                 state.outbound_sent ? "true" : "false",
                 state.inbound_received ? "true" : "false",
                 state.sync_complete ? "true" : "false");
    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    rns_runtime_destroy(runtime);
    (void)unlink(argv[3]);
    return success ? 0 : 1;
}
