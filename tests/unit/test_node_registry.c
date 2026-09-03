#define _POSIX_C_SOURCE 200809L
#include "reticulum/destination.h"
#include "reticulum/lxmf_router.h"
#include "reticulum/lxmf_propagation.h"
#include "reticulum/node_registry.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint8_t destination[16],next_hop[16],public_key[64],message_destination[16];
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];size_t app_data_length;
    uint64_t announce_timebase;uint8_t hops;uint64_t interface_id;int32_t gravity;
    double seen_at,expires_at;bool reachable,propagation,has_ratchet,has_message_destination;
    rns_node_kind kind;char name[64];
} legacy_record_v1;

typedef struct {
    uint8_t destination[16],next_hop[16],public_key[64],message_destination[16];
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];size_t app_data_length;
    uint64_t announce_timebase;uint8_t hops;uint64_t interface_id;int32_t gravity;
    double seen_at,expires_at;bool reachable,propagation,has_ratchet,has_message_destination;
    rns_node_kind kind;char name[64];uint8_t ratchet[32];bool lxmf_app_data_valid;
    bool lxmf_has_stamp_cost;uint8_t lxmf_stamp_cost;uint32_t lxmf_features;
} legacy_record_v2;

static void temporary_path(char path[64], const char *stem) {
    (void)snprintf(path, 64U, "/tmp/%s-XXXXXX", stem);
    int descriptor = mkstemp(path);
    assert(descriptor >= 0 && close(descriptor) == 0);
}

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL && fseek(file, 0L, SEEK_END) == 0);
    long end = ftell(file);
    assert(end >= 0 && fseek(file, 0L, SEEK_SET) == 0);
    *length = (size_t)end;
    uint8_t *bytes = malloc(*length == 0U ? 1U : *length);
    assert(bytes != NULL && fread(bytes, 1U, *length, file) == *length);
    assert(fgetc(file) == EOF && fclose(file) == 0);
    return bytes;
}

static void write_file(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fwrite(bytes, 1U, length, file) == length);
    assert(fclose(file) == 0);
}

static uint32_t test_get32(const uint8_t *input) {
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | input[3];
}

static void test_put32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static uint32_t test_crc32(const uint8_t *input, size_t length) {
    uint32_t crc = UINT32_MAX;
    while (length-- != 0U) {
        crc ^= *input++;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^
                  (UINT32_C(0xedb88320) &
                   (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static rns_node_record complete_record(uint8_t marker) {
    rns_node_record record = {0};
    memset(record.destination, marker, sizeof record.destination);
    memset(record.next_hop, marker + 1U, sizeof record.next_hop);
    memset(record.public_key, marker + 2U, sizeof record.public_key);
    memset(record.message_destination, marker + 3U,
           sizeof record.message_destination);
    memcpy(record.app_data, "opaque-app", 10U);
    record.app_data_length = 10U;
    record.announce_timebase = UINT64_C(0x0102030405060708);
    record.hops = 7U;
    record.interface_id = UINT64_C(0x1112131415161718);
    record.gravity = -123456;
    record.seen_at = 12.5;
    record.expires_at = 42.75;
    record.reachable = true;
    record.propagation = true;
    record.has_ratchet = true;
    record.has_message_destination = true;
    record.kind = RNS_NODE_KIND_LXMF;
    memcpy(record.name, "Rei node", 9U);
    memset(record.ratchet, marker + 4U, sizeof record.ratchet);
    record.lxmf_app_data_valid = true;
    record.lxmf_has_stamp_cost = true;
    record.lxmf_stamp_cost = 9U;
    record.lxmf_features = UINT32_C(0x10203040);
    record.lxmf_pn_app_data_valid = true;
    record.lxmf_pn_enabled = true;
    record.lxmf_pn_stamp_cost = 11U;
    record.lxmf_pn_stamp_flexibility = 3U;
    static const uint8_t extension[] = {0x81, 0xa1, 'x', 0x2a};
    memcpy(record.persistence_extensions, extension, sizeof extension);
    record.persistence_extensions_length = sizeof extension;
    return record;
}

static void assert_complete_record(const rns_node_record *record,
                                   uint8_t marker) {
    assert(record != NULL && record->destination[0] == marker);
    assert(record->next_hop[0] == (uint8_t)(marker + 1U));
    assert(record->public_key[0] == (uint8_t)(marker + 2U));
    assert(record->message_destination[0] == (uint8_t)(marker + 3U));
    assert(record->app_data_length == 10U &&
           memcmp(record->app_data, "opaque-app", 10U) == 0);
    assert(record->announce_timebase == UINT64_C(0x0102030405060708));
    assert(record->hops == 7U &&
           record->interface_id == UINT64_C(0x1112131415161718));
    assert(record->gravity == -123456 && record->seen_at == 12.5 &&
           record->expires_at == 42.75);
    assert(record->reachable && record->propagation && record->has_ratchet &&
           record->has_message_destination);
    assert(record->kind == RNS_NODE_KIND_LXMF &&
           strcmp(record->name, "Rei node") == 0);
    assert(record->ratchet[0] == (uint8_t)(marker + 4U));
    assert(record->lxmf_app_data_valid && record->lxmf_has_stamp_cost &&
           record->lxmf_stamp_cost == 9U &&
           record->lxmf_features == UINT32_C(0x10203040));
    assert(record->lxmf_pn_app_data_valid && record->lxmf_pn_enabled &&
           record->lxmf_pn_stamp_cost == 11U &&
           record->lxmf_pn_stamp_flexibility == 3U);
    static const uint8_t extension[] = {0x81, 0xa1, 'x', 0x2a};
    assert(record->persistence_extensions_length == sizeof extension &&
           memcmp(record->persistence_extensions, extension,
                  sizeof extension) == 0);
}

static void test_portable_roundtrip_corruption_and_atomic_replace(void) {
    char path[64];
    temporary_path(path, "rns-node-v3");
    rns_node_registry registry;
    rns_node_registry_init(&registry, 10.0);
    registry.records[0] = complete_record(0x21U);
    registry.count = 1U;
    char unterminated_path[5000];
    memset(unterminated_path, 'x', sizeof unterminated_path);
    rns_node_registry loaded;
    rns_node_registry_init(&loaded, 10.0);
    assert(!rns_node_registry_save(&registry, unterminated_path));
    assert(!rns_node_registry_load(&loaded, unterminated_path, 10.0));
    assert(!rns_node_registry_save(&registry, ""));
    assert(!rns_node_registry_load(&loaded, "", 10.0));
    assert(rns_node_registry_save(&registry, path));

    size_t length;
    uint8_t *valid = read_file(path, &length);
    assert(length > 24U && memcmp(valid, "RNSN3\0\0\0", 8U) == 0);
    assert(valid[8] == 0U && valid[9] == 3U && valid[10] == 0U &&
           valid[11] == 24U && valid[12] == 0U && valid[13] == 0U &&
           valid[14] == 0U && valid[15] == 1U);
    assert(memcmp(valid + 146U, "\x01\x02\x03\x04\x05\x06\x07\x08", 8U) == 0);
    rns_node_registry_destroy(&loaded);
    rns_node_registry_init(&loaded, 77.0);
    assert(rns_node_registry_load(&loaded, path, 10.0));
    assert(loaded.count == 1U && loaded.lifetime == 10.0);
    assert_complete_record(&loaded.records[0], 0x21U);

    /* The first portable record format did not include propagation-node
     * costs. It must remain readable, with only those new fields defaulted. */
    const size_t record_offset = 28U;
    const size_t v1_prefix_end = record_offset + 197U;
    assert(length > v1_prefix_end + 2U);
    uint8_t *portable_v1 = malloc(length - 2U);
    assert(portable_v1 != NULL);
    memcpy(portable_v1, valid, v1_prefix_end);
    memcpy(portable_v1 + v1_prefix_end, valid + v1_prefix_end + 2U,
           length - v1_prefix_end - 2U);
    portable_v1[record_offset] = 0U;
    portable_v1[record_offset + 1U] = 1U;
    portable_v1[record_offset + 2U] = 0U;
    portable_v1[record_offset + 3U] &= 0x3fU;
    test_put32(portable_v1 + 24U, test_get32(valid + 24U) - 2U);
    test_put32(portable_v1 + 16U, test_get32(valid + 16U) - 2U);
    test_put32(portable_v1 + 20U,
               test_crc32(portable_v1 + 24U,
                          test_get32(portable_v1 + 16U)));
    write_file(path, portable_v1, length - 2U);
    assert(rns_node_registry_load(&loaded, path, 10.0));
    assert(!loaded.records[0].lxmf_pn_app_data_valid &&
           !loaded.records[0].lxmf_pn_enabled &&
           loaded.records[0].lxmf_pn_stamp_cost == 0U &&
           loaded.records[0].lxmf_pn_stamp_flexibility == 0U);
    free(portable_v1);
    write_file(path, valid, length);

    for (size_t cut = 0U; cut < length; ++cut) {
        write_file(path, valid, cut);
        loaded.records[0].destination[0] = 0xeeU;
        loaded.count = 1U;
        assert(!rns_node_registry_load(&loaded, path, 10.0));
        assert(loaded.count == 1U && loaded.records[0].destination[0] == 0xeeU);
    }
    uint8_t *corrupt = malloc(length);
    assert(corrupt != NULL);
    memcpy(corrupt, valid, length);
    corrupt[length - 1U] ^= 0x80U;
    write_file(path, corrupt, length);
    assert(!rns_node_registry_load(&loaded, path, 10.0));
    assert(loaded.records[0].destination[0] == 0xeeU);
    free(corrupt);

    registry.records[0] = complete_record(0x31U);
    assert(rns_node_registry_save(&registry, path));
    assert(rns_node_registry_load(&loaded, path, 10.0));
    assert_complete_record(&loaded.records[0], 0x31U);

    registry.records[0].app_data_length = RNS_NODE_APP_DATA_MAX + 1U;
    assert(!rns_node_registry_save(&registry, path));
    assert(rns_node_registry_load(&loaded, path, 10.0));
    assert_complete_record(&loaded.records[0], 0x31U);
    free(valid);
    assert(unlink(path) == 0);
    rns_node_registry_destroy(&loaded);
    rns_node_registry_destroy(&registry);
}

static void test_legacy_migration(void) {
    char path[64];
    temporary_path(path, "rns-node-v1");
    legacy_record_v1 v1 = {0};
    v1.destination[0] = 2U;
    v1.has_ratchet = true;
    memcpy(v1.name, "Legacy", 7U);
    uint32_t count = 1U;
    FILE *file = fopen(path, "wb");
    assert(file != NULL && fwrite("RNSN1\0\0\0", 1U, 8U, file) == 8U);
    assert(fwrite(&count, sizeof count, 1U, file) == 1U &&
           fwrite(&v1, sizeof v1, 1U, file) == 1U && fclose(file) == 0);
    rns_node_registry loaded;
    rns_node_registry_init(&loaded, 10.0);
    assert(rns_node_registry_load(&loaded, path, 10.0));
    assert(loaded.count == 1U && loaded.records[0].destination[0] == 2U);
    assert(!loaded.records[0].has_ratchet &&
           strcmp(loaded.records[0].name, "Legacy") == 0);

    legacy_record_v2 v2 = {0};
    v2.destination[0] = 3U;
    v2.ratchet[0] = 0xa5U;
    v2.has_ratchet = true;
    v2.lxmf_app_data_valid = true;
    v2.lxmf_has_stamp_cost = true;
    v2.lxmf_stamp_cost = 8U;
    v2.lxmf_features = LXMF_FEATURE_COMPRESSION;
    memcpy(v2.name, "Version two", 12U);
    file = fopen(path, "wb");
    assert(file != NULL && fwrite("RNSN2\0\0\0", 1U, 8U, file) == 8U);
    assert(fwrite(&count, sizeof count, 1U, file) == 1U &&
           fwrite(&v2, sizeof v2, 1U, file) == 1U && fclose(file) == 0);
    assert(rns_node_registry_load(&loaded, path, 10.0));
    assert(loaded.count == 1U && loaded.records[0].destination[0] == 3U);
    assert(loaded.records[0].has_ratchet && loaded.records[0].ratchet[0] == 0xa5U);
    assert(loaded.records[0].lxmf_app_data_valid &&
           loaded.records[0].lxmf_has_stamp_cost &&
           loaded.records[0].lxmf_stamp_cost == 8U &&
           loaded.records[0].lxmf_features == LXMF_FEATURE_COMPRESSION);
    assert(strcmp(loaded.records[0].name, "Version two") == 0);
    assert(rns_node_registry_save(&loaded, path));
    size_t length;
    uint8_t *migrated = read_file(path, &length);
    assert(length > 24U && memcmp(migrated, "RNSN3\0\0\0", 8U) == 0);
    free(migrated);

    uint8_t raw[sizeof v2];
    memcpy(raw, &v2, sizeof raw);
    raw[offsetof(legacy_record_v2, reachable)] = 2U;
    file = fopen(path, "wb");
    assert(file != NULL && fwrite("RNSN2\0\0\0", 1U, 8U, file) == 8U);
    assert(fwrite(&count, sizeof count, 1U, file) == 1U &&
           fwrite(raw, 1U, sizeof raw, file) == sizeof raw && fclose(file) == 0);
    loaded.records[0].destination[0] = 0xfeU;
    assert(!rns_node_registry_load(&loaded, path, 10.0));
    assert(loaded.records[0].destination[0] == 0xfeU);
    assert(unlink(path) == 0);
    rns_node_registry_destroy(&loaded);
}

static void test_registry_and_verified_announces(void) {
    rns_node_registry registry;
    rns_node_registry_init(&registry, 10.0);
    rns_node_record basic = {0};
    basic.destination[0] = 1U;
    basic.seen_at = 2.0;
    assert(rns_node_registry_upsert(&registry, &basic));
    assert(rns_node_registry_get(&registry, basic.destination) != NULL);
    assert(rns_node_registry_expire(&registry, 12.0) == 1U);

    uint8_t private_key[64];
    for (size_t i = 0U; i < sizeof private_key; ++i)
        private_key[i] = (uint8_t)(i + 1U);
    rns_node_result result = {0};
    assert(rns_identity_from_private(&result.announce_identity, private_key));
    const char *node_aspects[] = {"node"};
    assert(rns_destination_hash(&result.announce_identity, "nomadnetwork",
                                node_aspects, 1U, result.destination_hash));
    result.has_verified_announce = 1;
    result.announce_timebase = 9U;
    result.received_at = 20.0;
    result.announce_app_data = (const uint8_t *)"Rei Node";
    result.announce_app_data_length = 8U;
    assert(rns_node_registry_consider_announce(&registry, &result));
    const rns_node_record *record = rns_node_registry_get(
        &registry, result.destination_hash);
    assert(record != NULL && record->kind == RNS_NODE_KIND_NOMAD &&
           record->has_message_destination && strcmp(record->name, "Rei Node") == 0);

    const char *delivery_aspects[] = {"delivery"};
    assert(rns_destination_hash(&result.announce_identity, "lxmf",
                                delivery_aspects, 1U, result.destination_hash));
    lxmf_announce_data_t announce = {0};
    memcpy(announce.display_name, "Rei", 3U);
    announce.display_name_len = 3U;
    announce.has_stamp_cost = true;
    announce.stamp_cost = 8U;
    announce.features = LXMF_FEATURE_COMPRESSION;
    uint8_t app_data[64], ratchet[32];
    size_t app_data_length = 0U;
    memset(ratchet, 0xa5, sizeof ratchet);
    assert(lxmf_announce_encode(&announce, app_data, sizeof app_data,
                                &app_data_length) == LXMF_OK);
    result.announce_timebase = 10U;
    result.announce_app_data = app_data;
    result.announce_app_data_length = app_data_length;
    result.announce_has_ratchet = 1;
    result.announce_ratchet = ratchet;
    assert(rns_node_registry_consider_announce(&registry, &result));
    record = rns_node_registry_get(&registry, result.destination_hash);
    assert(record != NULL && record->kind == RNS_NODE_KIND_LXMF &&
           record->lxmf_app_data_valid && record->has_ratchet &&
           memcmp(record->ratchet, ratchet, sizeof ratchet) == 0);
    assert(record->lxmf_has_stamp_cost && record->lxmf_stamp_cost == 8U &&
           (record->lxmf_features & LXMF_FEATURE_COMPRESSION) != 0U &&
           strcmp(record->name, "Rei") == 0);

    memset(ratchet, 0x11, sizeof ratchet);
    assert(!rns_node_registry_consider_announce(&registry, &result));
    record = rns_node_registry_get(&registry, result.destination_hash);
    assert(record != NULL && record->ratchet[0] == 0xa5U);

    const char *propagation_aspects[] = {"propagation"};
    assert(rns_destination_hash(&result.announce_identity, "lxmf",
                                propagation_aspects, 1U,
                                result.destination_hash));
    lxmf_pn_announce_t pn = {0};
    pn.enabled = true;
    pn.stamp_cost = 11U;
    pn.stamp_flexibility = 3U;
    size_t pn_length = 0U;
    assert(lxmf_pn_announce_encode(&pn, app_data, sizeof app_data,
                                   &pn_length) == LXMF_OK);
    result.announce_timebase = 11U;
    result.announce_app_data = app_data;
    result.announce_app_data_length = pn_length;
    result.announce_has_ratchet = 0;
    result.announce_ratchet = NULL;
    assert(rns_node_registry_consider_announce(&registry, &result));
    record = rns_node_registry_get(&registry, result.destination_hash);
    assert(record != NULL && record->propagation &&
           record->lxmf_pn_app_data_valid && record->lxmf_pn_enabled &&
           record->lxmf_pn_stamp_cost == 11U &&
           record->lxmf_pn_stamp_flexibility == 3U);

    rns_node_record filtered[8];
    size_t matches = rns_node_registry_sorted_filter(
        &registry, filtered, 8U, "rei");
    assert(matches >= 2U);
    assert(rns_node_registry_sorted_filter(&registry, filtered,
                                           8U, "REI") ==
           matches);
    assert(rns_node_registry_sorted_filter(&registry, filtered,
                                           8U,
                                           "missing") == 0U);
    rns_node_registry_destroy(&registry);
}

static void test_registry_scales_past_legacy_limit(void) {
    enum { TEST_NODE_COUNT = 1024 };
    rns_node_registry registry;
    rns_node_registry_init(&registry, 3600.0);
    for (uint32_t i = 0U; i < TEST_NODE_COUNT; ++i) {
        rns_node_record record = {0};
        record.destination[0] = (uint8_t)(i >> 8U);
        record.destination[1] = (uint8_t)i;
        record.seen_at = (double)i;
        record.reachable = (i & 1U) == 0U;
        (void)snprintf(record.name, sizeof record.name, "node-%04u", i);
        assert(rns_node_registry_upsert(&registry, &record));
    }
    assert(registry.count == TEST_NODE_COUNT);
    uint8_t destination[16] = {3U, 255U};
    assert(rns_node_registry_get(&registry, destination) != NULL);
    assert(rns_node_registry_count_filter(&registry, "node-") ==
           TEST_NODE_COUNT);

    rns_node_record *sorted = malloc(TEST_NODE_COUNT * sizeof *sorted);
    assert(sorted != NULL);
    assert(rns_node_registry_sorted(&registry, sorted, TEST_NODE_COUNT) ==
           TEST_NODE_COUNT);
    assert(sorted[0].reachable && !sorted[TEST_NODE_COUNT - 1U].reachable);
    rns_node_record first;
    assert(rns_node_registry_sorted(&registry, &first, 1U) == 1U);
    assert(first.reachable && first.seen_at == 1022.0);

    char path[64];
    temporary_path(path, "rns-node-scale");
    assert(rns_node_registry_save(&registry, path));
    rns_node_registry loaded;
    rns_node_registry_init(&loaded, 3600.0);
    assert(rns_node_registry_load(&loaded, path, 3600.0));
    assert(loaded.count == TEST_NODE_COUNT);
    assert(rns_node_registry_get(&loaded, destination) != NULL);
    assert(unlink(path) == 0);

    free(sorted);
    rns_node_registry_destroy(&loaded);
    rns_node_registry_destroy(&registry);
}

int main(void) {
    test_portable_roundtrip_corruption_and_atomic_replace();
    test_legacy_migration();
    test_registry_and_verified_announces();
    test_registry_scales_past_legacy_limit();
    puts("node registry tests passed");
    return 0;
}
