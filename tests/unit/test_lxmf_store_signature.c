/* Signature state retention, its journal encoding and the caps that bound
 * messages retained from senders whose identity is not held yet. */
#include "reticulum/lxmf_store.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t crc32_bytes(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8u; i++) c = (c >> 1) ^ (0xedb88320u & (~(c & 1u) + 1u));
    }
    return ~c;
}

static void store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Writes a record in the pre-signature-state format: type 1, a 77 byte fixed
 * prefix and the content. */
static void append_legacy_put(const char *path, uint8_t id_seed, uint8_t source_seed,
                              const char *content) {
    uint8_t payload[77u + 32u];
    uint8_t header[16] = {'L', 'X', 'M', 'S', 1u, 1u, 0u, 0u};
    size_t content_len = strlen(content);
    uint32_t n = (uint32_t)(77u + content_len);
    double timestamp = 7.5;
    uint64_t bits;
    int fd;

    assert(content_len <= 32u);
    memset(payload, 0, sizeof payload);
    payload[0] = id_seed;
    payload[32] = 9u;
    payload[48] = source_seed;
    memcpy(&bits, &timestamp, sizeof bits);
    for (unsigned i = 0; i < 8u; i++) payload[64u + 7u - i] = (uint8_t)(bits >> (8u * i));
    payload[72] = (uint8_t)LXMF_DELIVERY_DELIVERED;
    store32(payload + 73, (uint32_t)content_len);
    memcpy(payload + 77, content, content_len);
    store32(header + 8, n);
    store32(header + 12, crc32_bytes(payload, n));
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    assert(fd >= 0);
    assert(write(fd, header, sizeof header) == (ssize_t)sizeof header);
    assert(write(fd, payload, n) == (ssize_t)n);
    close(fd);
}

static void fill_message(lxmf_store_message_t *message, uint8_t id_seed,
                         uint8_t source_seed) {
    memset(message, 0, sizeof *message);
    message->message_id[0] = id_seed;
    message->message_id[1] = source_seed;
    message->destination[0] = 0xdd;
    message->source[0] = source_seed;
    message->timestamp = 1.0 + id_seed;
    message->status = LXMF_DELIVERY_DELIVERED;
    message->content = (lxmf_slice_t){(const uint8_t *)"body", 4u};
}

typedef struct { uint8_t source_seed; size_t count; } source_tally_t;

static bool tally(void *context, const lxmf_store_message_t *message) {
    source_tally_t *state = context;
    if (message->signature_state == LXMF_SIGNATURE_UNVERIFIED &&
        message->source[0] == state->source_seed)
        state->count++;
    return true;
}

static size_t retained_from(lxmf_store_t *store, uint8_t source_seed) {
    source_tally_t state = {source_seed, 0u};
    assert(lxmf_store_list(store, tally, &state) == LXMF_OK);
    return state.count;
}

static void test_round_trip(const char *path) {
    lxmf_store_t store = {0};
    lxmf_store_message_t message, got;
    uint8_t body[32], retained[LXMF_STORE_MAX_PACKED];
    static const uint8_t packed[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    size_t retained_length = 0;
    bool inserted = false;

    assert(lxmf_store_open(&store, path) == LXMF_OK);
    fill_message(&message, 1u, 0x11u);
    message.signature_state = LXMF_SIGNATURE_UNVERIFIED;
    message.packed = (lxmf_slice_t){packed, sizeof packed};
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    assert(lxmf_store_unverified_count(&store) == 1u);

    /* A verified message keeps no packed copy and is not counted. */
    lxmf_store_message_t verified;
    fill_message(&verified, 2u, 0x22u);
    assert(lxmf_store_put(&store, &verified, &inserted) == LXMF_OK && inserted);
    assert(lxmf_store_unverified_count(&store) == 1u);
    assert(lxmf_store_read_packed(&store, verified.message_id, retained,
                                  sizeof retained, &retained_length) == LXMF_ERR_FORMAT);

    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_UNVERIFIED);
    assert(got.packed.len == 0u && got.packed.data == NULL);
    assert(got.content.len == 4u && memcmp(body, "body", 4) == 0);
    assert(lxmf_store_read_packed(&store, message.message_id, retained,
                                  sizeof retained, &retained_length) == LXMF_OK);
    assert(retained_length == sizeof packed &&
           memcmp(retained, packed, sizeof packed) == 0);
    /* A capacity below the retained length must not truncate silently. */
    assert(lxmf_store_read_packed(&store, message.message_id, retained, 2u,
                                  &retained_length) == LXMF_ERR_BOUNDS);
    lxmf_store_close(&store);

    /* The state, the packed copy and the delivery status survive a restart. */
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_unverified_count(&store) == 1u);
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_UNVERIFIED &&
           got.status == LXMF_DELIVERY_DELIVERED && got.timestamp == message.timestamp);
    assert(lxmf_store_read_packed(&store, message.message_id, retained,
                                  sizeof retained, &retained_length) == LXMF_OK);
    assert(retained_length == sizeof packed);

    assert(lxmf_store_update_signature(&store, message.message_id,
                                       LXMF_SIGNATURE_VERIFIED) == LXMF_OK);
    assert(lxmf_store_unverified_count(&store) == 0u);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_VERIFIED);

    /* Compaction rewrites every record and keeps both the state and the copy. */
    assert(lxmf_store_update_signature(&store, message.message_id,
                                       LXMF_SIGNATURE_UNVERIFIED) == LXMF_OK);
    assert(lxmf_store_compact(&store) == LXMF_OK);
    assert(lxmf_store_count(&store) == 2u && lxmf_store_unverified_count(&store) == 1u);
    assert(lxmf_store_read_packed(&store, message.message_id, retained,
                                  sizeof retained, &retained_length) == LXMF_OK);
    assert(retained_length == sizeof packed &&
           memcmp(retained, packed, sizeof packed) == 0);

    /* Removal drops the message and survives a restart. */
    assert(lxmf_store_remove(&store, message.message_id) == LXMF_OK);
    assert(lxmf_store_count(&store) == 1u && lxmf_store_unverified_count(&store) == 0u);
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) ==
           LXMF_ERR_FORMAT);
    assert(lxmf_store_read(&store, verified.message_id, &got, body, sizeof body) == LXMF_OK);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_count(&store) == 1u);
    assert(lxmf_store_remove(&store, message.message_id) == LXMF_ERR_FORMAT);
    lxmf_store_close(&store);
}

static void test_caps(const char *path) {
    lxmf_store_t store = {0};
    lxmf_store_message_t message;
    static const uint8_t packed[] = {1u, 2u, 3u};
    bool inserted = false;

    assert(lxmf_store_open(&store, path) == LXMF_OK);
    /* One sender cannot grow the retained queue past the cap. */
    for (unsigned i = 0; i < LXMF_STORE_MAX_UNVERIFIED + 8u; i++) {
        fill_message(&message, (uint8_t)(i + 1u), 0x41u);
        message.signature_state = LXMF_SIGNATURE_UNVERIFIED;
        message.packed = (lxmf_slice_t){packed, sizeof packed};
        assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
        assert(lxmf_store_unverified_count(&store) <= LXMF_STORE_MAX_UNVERIFIED);
    }
    assert(lxmf_store_unverified_count(&store) == LXMF_STORE_MAX_UNVERIFIED);
    assert(lxmf_store_count(&store) == LXMF_STORE_MAX_UNVERIFIED);
    /* Eviction is oldest first, so the earliest message is gone and the newest
     * is retained. */
    fill_message(&message, 1u, 0x41u);
    lxmf_store_message_t got;
    uint8_t body[32];
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) ==
           LXMF_ERR_FORMAT);
    fill_message(&message, (uint8_t)(LXMF_STORE_MAX_UNVERIFIED + 8u), 0x41u);
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    lxmf_store_close(&store);

    /* The bound holds across a restart rather than being an in-memory guard. */
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_unverified_count(&store) == LXMF_STORE_MAX_UNVERIFIED);

    /* A flooding sender is drained before another sender's message is, so the
     * distinct-sender cap admits new senders. */
    for (unsigned s = 0; s < LXMF_STORE_MAX_UNVERIFIED_SOURCES + 4u; s++) {
        fill_message(&message, (uint8_t)(200u + s), (uint8_t)(0x81u + s));
        message.signature_state = LXMF_SIGNATURE_UNVERIFIED;
        message.packed = (lxmf_slice_t){packed, sizeof packed};
        assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
        assert(lxmf_store_unverified_count(&store) <= LXMF_STORE_MAX_UNVERIFIED);
        assert(retained_from(&store, (uint8_t)(0x81u + s)) == 1u);
    }
    /* The flood is fully drained, and no more distinct senders are retained
     * than the cap allows. */
    assert(retained_from(&store, 0x41u) < LXMF_STORE_MAX_UNVERIFIED);
    size_t distinct = 0;
    for (unsigned s = 0; s < LXMF_STORE_MAX_UNVERIFIED_SOURCES + 4u; s++)
        if (retained_from(&store, (uint8_t)(0x81u + s)) > 0u) distinct++;
    if (retained_from(&store, 0x41u) > 0u) distinct++;
    assert(distinct <= LXMF_STORE_MAX_UNVERIFIED_SOURCES);
    lxmf_store_close(&store);
}

static void test_legacy_journal(const char *path) {
    lxmf_store_t store = {0};
    lxmf_store_message_t message, got;
    uint8_t body[32], retained[LXMF_STORE_MAX_PACKED];
    static const uint8_t packed[] = {4u, 5u, 6u};
    size_t retained_length = 0;
    bool inserted = false;

    /* A journal written before signature states carried no such field. Its
     * messages were stored only after a verified signature, so they load as
     * verified and are readable unchanged. */
    append_legacy_put(path, 0x51u, 0x61u, "legacy one");
    append_legacy_put(path, 0x52u, 0x62u, "legacy two");
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_count(&store) == 2u && lxmf_store_unverified_count(&store) == 0u);
    memset(&message, 0, sizeof message);
    message.message_id[0] = 0x52u;
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_VERIFIED);
    assert(got.status == LXMF_DELIVERY_DELIVERED && got.timestamp == 7.5);
    assert(got.source[0] == 0x62u && got.destination[0] == 9u);
    assert(got.content.len == 10u && memcmp(body, "legacy two", 10) == 0);
    assert(lxmf_store_read_packed(&store, message.message_id, retained,
                                  sizeof retained, &retained_length) == LXMF_ERR_FORMAT);

    /* New records append to the same file and both formats coexist. */
    fill_message(&message, 0x70u, 0x71u);
    message.signature_state = LXMF_SIGNATURE_UNVERIFIED;
    message.packed = (lxmf_slice_t){packed, sizeof packed};
    assert(lxmf_store_put(&store, &message, &inserted) == LXMF_OK && inserted);
    assert(lxmf_store_update_status(&store, message.message_id, LXMF_DELIVERY_SENT) == LXMF_OK);
    lxmf_store_close(&store);
    assert(lxmf_store_open(&store, path) == LXMF_OK);
    assert(lxmf_store_count(&store) == 3u && lxmf_store_unverified_count(&store) == 1u);
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_UNVERIFIED &&
           got.status == LXMF_DELIVERY_SENT);

    /* Compacting migrates the legacy records without losing them. */
    assert(lxmf_store_compact(&store) == LXMF_OK);
    assert(lxmf_store_count(&store) == 3u && lxmf_store_unverified_count(&store) == 1u);
    memset(&message, 0, sizeof message);
    message.message_id[0] = 0x51u;
    assert(lxmf_store_read(&store, message.message_id, &got, body, sizeof body) == LXMF_OK);
    assert(got.signature_state == LXMF_SIGNATURE_VERIFIED && got.content.len == 10u &&
           memcmp(body, "legacy one", 10) == 0);
    lxmf_store_close(&store);
}

int main(void) {
    char round_trip[] = "/tmp/lxmf-signature-XXXXXX";
    char caps[] = "/tmp/lxmf-signature-caps-XXXXXX";
    char legacy[] = "/tmp/lxmf-signature-legacy-XXXXXX";
    int fd;

    fd = mkstemp(round_trip); assert(fd >= 0); close(fd);
    fd = mkstemp(caps); assert(fd >= 0); close(fd);
    fd = mkstemp(legacy); assert(fd >= 0); close(fd);

    test_round_trip(round_trip);
    test_caps(caps);
    test_legacy_journal(legacy);

    unlink(round_trip);
    unlink(caps);
    unlink(legacy);
    return 0;
}
