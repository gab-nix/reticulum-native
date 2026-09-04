#include "reticulum/destination.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_router.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    rns_identity sender, recipient;
    uint8_t cost;
    uint8_t packet[500];
    size_t packet_length, sends;
    lxmf_router_event_t event;
} fixture_t;

static const rns_identity *resolve(void *context, const uint8_t hash[16]) {
    fixture_t *fixture = context;
    (void)hash;
    return &fixture->recipient;
}

static bool cost(void *context, const uint8_t hash[16], uint8_t *value) {
    fixture_t *fixture = context;
    (void)hash;
    *value = fixture->cost;
    return true;
}

static lxmf_status_t send_packet(void *context, const uint8_t *packet,
                                 size_t length) {
    fixture_t *fixture = context;
    assert(length <= sizeof fixture->packet);
    memcpy(fixture->packet, packet, length);
    fixture->packet_length = length;
    fixture->sends++;
    return LXMF_OK;
}

static void event(void *context, const lxmf_router_event_t *value) {
    ((fixture_t *)context)->event = *value;
}

static void queue(lxmf_store_t *store, fixture_t *fixture, double time,
                   uint8_t id[32]) {
    static const char *const aspects[] = {"delivery"};
    /* Unknown extension includes a nested array and binary data. */
    static const uint8_t fields[] = {0x81, 0xcc, 0xaa, 0x92, 0xc4, 2, 0, 255,
                                     0xa2, 'o', 'k'};
    lxmf_message_t message = {.timestamp = time,
        .title = {(const uint8_t *)"title", 5},
        .content = {(const uint8_t *)"message", 7},
        .fields_msgpack = {fields, sizeof fields}};
    assert(rns_destination_hash(&fixture->sender, "lxmf", aspects, 1,
                                message.source));
    assert(rns_destination_hash(&fixture->recipient, "lxmf", aspects, 1,
                                message.destination));
    uint8_t packed[512];
    size_t length;
    assert(lxmf_pack(&message, lxmf_identity_signer, &fixture->sender, packed,
                     sizeof packed, &length) == LXMF_OK);
    lxmf_message_t parsed;
    assert(lxmf_unpack(packed, length, NULL, NULL, &parsed) == LXMF_OK);
    memcpy(id, parsed.message_id, 32);
    lxmf_store_message_t stored = {.timestamp = time,
        .status = LXMF_DELIVERY_QUEUED,
        .signature_state = LXMF_SIGNATURE_VERIFIED,
        .content = message.content, .packed = {packed, length}};
    memcpy(stored.message_id, id, 32);
    memcpy(stored.source, message.source, 16);
    memcpy(stored.destination, message.destination, 16);
    bool inserted;
    assert(lxmf_store_put(store, &stored, &inserted) == LXMF_OK && inserted);
}

static void finish(lxmf_router_t *router, const uint8_t id[32]) {
    lxmf_status_t status = LXMF_ERR_PENDING;
    size_t polls = 0;
    while (status == LXMF_ERR_PENDING && polls++ < 10000)
        status = lxmf_router_send_message(router, id);
    assert(status == LXMF_OK);
}

int main(void) {
    fixture_t fixture = {.cost = 2};
    uint8_t a[64], b[64];
    for (size_t i = 0; i < sizeof a; ++i) {
        a[i] = (uint8_t)i;
        b[i] = (uint8_t)(i + 64);
    }
    assert(rns_identity_from_private(&fixture.sender, a));
    assert(rns_identity_from_private(&fixture.recipient, b));
    char path[] = "/tmp/lxmf-stamp-queue-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    lxmf_router_config_t config = {.identity = &fixture.sender,
        .store = &store, .resolve_identity = resolve,
        .resolve_context = &fixture, .resolve_stamp_cost = cost,
        .stamp_cost_context = &fixture, .send_packet = send_packet,
        .send_context = &fixture, .event_callback = event,
        .event_context = &fixture};
    lxmf_router_t router;
    config.stamp_work_units = 65;
    assert(lxmf_router_init(&router, &config) == LXMF_ERR_ARGUMENT);
    config.stamp_work_units = 64;
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    uint8_t first[32], second[32];
    queue(&store, &fixture, 1, first);
    queue(&store, &fixture, 2, second);
    uint8_t original[512];
    size_t original_length;
    assert(lxmf_store_read_packed(&store, first, original, sizeof original,
                                  &original_length) == LXMF_OK);
    assert(lxmf_router_send_message(&router, first) == LXMF_ERR_PENDING);
    assert(fixture.sends == 0 && fixture.event.attempt == 0 &&
           fixture.event.state == LXMF_DELIVERY_QUEUED &&
           fixture.event.queue_reason == LXMF_QUEUE_REASON_STAMP);
    lxmf_stamp_job_progress_t progress;
    assert(lxmf_router_stamp_progress(&router, first, &progress) == LXMF_OK);
    assert(progress.prepared_rounds == 64 && progress.attempts == 0);
    /* A queued competitor cannot steal/reset the active worker. */
    assert(lxmf_router_send_message(&router, second) == LXMF_ERR_PENDING);
    assert(lxmf_router_stamp_progress(&router, second, &progress) ==
           LXMF_ERR_PENDING);
    assert(lxmf_router_stamp_progress(&router, first, &progress) == LXMF_OK &&
           progress.prepared_rounds == 64);
    lxmf_delivery_metadata_t delivery;
    assert(lxmf_store_read_delivery(&store, first, &delivery) == LXMF_OK &&
           delivery.attempts == 0 && delivery.progress > 0);
    /* Cancellation survives restart and is excluded from automatic retries. */
    assert(lxmf_router_cancel_message(&router, second) == LXMF_OK);
    assert(lxmf_router_cancel_message(&router, first) == LXMF_OK);
    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    lxmf_router_poll_result_t polled;
    assert(lxmf_router_poll(&router, 2, &polled) == LXMF_OK &&
           polled.attempted == 0 && fixture.sends == 0);
    assert(lxmf_store_read_delivery(&store, first, &delivery) == LXMF_OK &&
           delivery.queue_reason == LXMF_QUEUE_REASON_CANCELLED);
    /* Explicit send retries a cancelled message. */
    assert(lxmf_router_send_message(&router, first) == LXMF_ERR_PENDING);
    finish(&router, first);
    assert(fixture.sends == 1);
    uint8_t stamped[512];
    size_t stamped_length;
    assert(lxmf_store_read_packed(&store, first, stamped, sizeof stamped,
                                  &stamped_length) == LXMF_OK);
    lxmf_message_t parsed, base;
    assert(lxmf_unpack(stamped, stamped_length, NULL, NULL, &parsed) == LXMF_OK);
    assert(lxmf_unpack(original, original_length, NULL, NULL, &base) == LXMF_OK);
    assert(parsed.has_stamp && parsed.stamp_len == LXMF_POW_STAMP_LENGTH &&
           memcmp(parsed.message_id, first, 32) == 0 &&
           memcmp(parsed.signature, base.signature, 64) == 0 &&
           parsed.fields_msgpack.len == base.fields_msgpack.len &&
           memcmp(parsed.fields_msgpack.data, base.fields_msgpack.data,
                  base.fields_msgpack.len) == 0 &&
           parsed.title.len == base.title.len &&
           memcmp(parsed.title.data, base.title.data, base.title.len) == 0);
    assert(lxmf_pow_stamp_validate(first, fixture.cost, parsed.stamp, NULL) ==
           LXMF_OK);
    /* Wrong-ID replacement is rejected without modifying the old record. */
    assert(lxmf_store_update_packed(&store, second, stamped, stamped_length) ==
           LXMF_ERR_FORMAT);
    uint8_t plaintext[500];
    size_t plaintext_length;
    lxmf_message_t received;
    assert(lxmf_opportunistic_packet_unpack(fixture.packet,
        fixture.packet_length, &fixture.recipient, NULL, NULL, plaintext,
        sizeof plaintext, &plaintext_length, &received) == LXMF_OK);
    assert(received.has_stamp && memcmp(received.stamp, parsed.stamp, 32) == 0);
    /* Restart retains the exact completed stamp. Its revalidation is also
     * incremental; the first candidate succeeds after preparation. */
    lxmf_router_destroy(&router);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    uint8_t replay[512];
    size_t replay_length;
    assert(lxmf_store_read_packed(&store, first, replay, sizeof replay,
                                  &replay_length) == LXMF_OK &&
           replay_length == stamped_length &&
           memcmp(replay, stamped, stamped_length) == 0);
    assert(lxmf_store_update_status(&store, first, LXMF_DELIVERY_QUEUED) ==
           LXMF_OK);
    assert(lxmf_store_compact(&store) == LXMF_OK);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_router_init(&router, &config) == LXMF_OK);
    assert(lxmf_router_send_message(&router, first) == LXMF_ERR_PENDING);
    finish(&router, first);
    assert(lxmf_router_stamp_progress(&router, first, &progress) == LXMF_OK &&
           progress.attempts == 1);
    assert(lxmf_store_read_packed(&store, first, replay, sizeof replay,
                                  &replay_length) == LXMF_OK &&
           replay_length == stamped_length &&
           memcmp(replay, stamped, stamped_length) == 0);
    fixture.cost = 255;
    assert(lxmf_router_send_message(&router, second) == LXMF_ERR_ARGUMENT &&
           fixture.event.state == LXMF_DELIVERY_FAILED &&
           fixture.event.queue_reason == LXMF_QUEUE_REASON_STAMP);
    fixture.cost = 0;
    assert(lxmf_router_send_message(&router, second) == LXMF_OK);
    /* A disappearing queued message cannot strand the only worker. */
    uint8_t abandoned[32], replacement[32];
    queue(&store, &fixture, 3, abandoned);
    queue(&store, &fixture, 4, replacement);
    fixture.cost = 2;
    assert(lxmf_router_send_message(&router, abandoned) == LXMF_ERR_PENDING);
    assert(lxmf_store_remove(&store, abandoned) == LXMF_OK);
    assert(lxmf_router_send_message(&router, replacement) == LXMF_ERR_PENDING);
    assert(lxmf_router_stamp_progress(&router, replacement, &progress) ==
           LXMF_OK && progress.prepared_rounds == 64);
    /* A changed announce cost resets the work target, never its message ID. */
    fixture.cost = 3;
    assert(lxmf_router_send_message(&router, replacement) == LXMF_ERR_PENDING);
    assert(lxmf_router_stamp_progress(&router, replacement, &progress) ==
           LXMF_OK && progress.prepared_rounds == 64);
    fixture.cost = 0;
    assert(lxmf_router_send_message(&router, replacement) == LXMF_OK &&
           router.stamp_job == NULL);
    lxmf_router_destroy(&router);
    /* A torn replacement record recovers the previous retained bytes. */
    uint8_t torn_id[32];
    queue(&store, &fixture, 5, torn_id);
    assert(lxmf_store_read_packed(&store, torn_id, original, sizeof original,
                                  &original_length) == LXMF_OK);
    assert(lxmf_unpack(original, original_length, NULL, NULL, &parsed) == LXMF_OK);
    parsed.has_stamp = true;
    parsed.stamp_len = LXMF_POW_STAMP_LENGTH;
    memset(parsed.stamp, 0, sizeof parsed.stamp);
    assert(lxmf_pack(&parsed, lxmf_identity_signer, &fixture.sender, stamped,
                     sizeof stamped, &stamped_length) == LXMF_OK);
    struct stat previous;
    assert(stat(path, &previous) == 0);
    assert(lxmf_store_update_packed(&store, torn_id, stamped, stamped_length) ==
           LXMF_OK);
    lxmf_store_close(&store);
    assert(truncate(path, previous.st_size + 20) == 0);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_read_packed(&store, torn_id, replay, sizeof replay,
                                  &replay_length) == LXMF_OK &&
           replay_length == original_length &&
           memcmp(replay, original, original_length) == 0);
    lxmf_store_close(&store);
    unlink(path);
    return 0;
}
