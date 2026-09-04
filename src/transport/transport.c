#include "reticulum/transport.h"

#include "reticulum/identity.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool live(double now, double deadline) {
    return isfinite(now) && now < deadline;
}

static double transport_now(const rns_transport *transport) {
    return transport->config.clock(transport->config.clock_context);
}

static uint64_t random_blob_timebase(const uint8_t blob[10]) {
    uint64_t value = 0u;
    for (size_t i = 5u; i < 10u; ++i) value = (value << 8u) | blob[i];
    return value;
}

static bool allocation_size_ok(size_t count, size_t size) {
    return count != 0u && count <= SIZE_MAX / size;
}

int rns_transport_init(rns_transport *transport,
                       const rns_transport_config *config) {
    if (transport == NULL || config == NULL || config->clock == NULL ||
        !allocation_size_ok(config->path_capacity, sizeof(rns_path_entry)) ||
        !allocation_size_ok(config->dedupe_capacity, sizeof(rns_dedupe_entry)) ||
        !allocation_size_ok(config->reverse_capacity, sizeof(rns_reverse_entry)) ||
        config->random_blob_history == 0u ||
        config->random_blob_history > RNS_TRANSPORT_MAX_RANDOM_BLOBS ||
        !isfinite(config->path_lifetime) || config->path_lifetime <= 0.0 ||
        !isfinite(config->dedupe_lifetime) || config->dedupe_lifetime <= 0.0 ||
        !isfinite(config->reverse_lifetime) || config->reverse_lifetime <= 0.0)
        return 0;

    memset(transport, 0, sizeof *transport);
    transport->config = *config;
    if (transport->config.link_capacity == 0u)
        transport->config.link_capacity = RNS_TRANSPORT_DEFAULT_LINK_CAPACITY;
    if (transport->config.link_lifetime == 0.0)
        transport->config.link_lifetime = RNS_TRANSPORT_LINK_TIMEOUT;
    if (transport->config.link_proof_timeout_per_hop == 0.0)
        transport->config.link_proof_timeout_per_hop =
            RNS_TRANSPORT_LINK_PROOF_TIMEOUT_PER_HOP;
    if (!allocation_size_ok(transport->config.link_capacity,
                            sizeof(rns_transport_link_entry)) ||
        !isfinite(transport->config.link_lifetime) ||
        transport->config.link_lifetime <= 0.0 ||
        !isfinite(transport->config.link_proof_timeout_per_hop) ||
        transport->config.link_proof_timeout_per_hop <= 0.0)
        return 0;

    transport->paths = calloc(transport->config.path_capacity,
                              sizeof *transport->paths);
    transport->dedupe = calloc(transport->config.dedupe_capacity,
                               sizeof *transport->dedupe);
    transport->reverse_paths = calloc(transport->config.reverse_capacity,
                                      sizeof *transport->reverse_paths);
    transport->links = calloc(transport->config.link_capacity,
                              sizeof *transport->links);
    if (transport->paths == NULL || transport->dedupe == NULL ||
        transport->reverse_paths == NULL || transport->links == NULL) {
        rns_transport_free(transport);
        return 0;
    }
    return 1;
}

void rns_transport_free(rns_transport *transport) {
    if (transport == NULL) return;
    free(transport->paths);
    free(transport->dedupe);
    free(transport->reverse_paths);
    free(transport->links);
    memset(transport, 0, sizeof *transport);
}

size_t rns_transport_expire(rns_transport *transport) {
    if (transport == NULL || transport->config.clock == NULL) return 0u;
    double now = transport_now(transport);
    if (!isfinite(now)) return 0u;
    size_t removed = 0u;
    for (size_t i = 0u; i < transport->config.path_capacity; ++i)
        if (transport->paths[i].occupied &&
            now >= transport->paths[i].expires_at) {
            memset(&transport->paths[i], 0, sizeof transport->paths[i]);
            ++removed;
        }
    for (size_t i = 0u; i < transport->config.dedupe_capacity; ++i)
        if (transport->dedupe[i].occupied &&
            now >= transport->dedupe[i].expires_at) {
            memset(&transport->dedupe[i], 0, sizeof transport->dedupe[i]);
            ++removed;
        }
    for (size_t i = 0u; i < transport->config.reverse_capacity; ++i)
        if (transport->reverse_paths[i].occupied &&
            now >= transport->reverse_paths[i].expires_at) {
            memset(&transport->reverse_paths[i], 0,
                   sizeof transport->reverse_paths[i]);
            ++removed;
        }
    for (size_t i = 0u; i < transport->config.link_capacity; ++i) {
        rns_transport_link_entry *entry = &transport->links[i];
        double deadline = entry->validated ? entry->expires_at
                                           : entry->proof_deadline;
        if (entry->occupied && now >= deadline) {
            memset(entry, 0, sizeof *entry);
            ++removed;
        }
    }
    return removed;
}

const rns_path_entry *rns_transport_lookup(rns_transport *transport,
                                           const uint8_t destination_hash[16]) {
    if (transport == NULL || destination_hash == NULL ||
        transport->paths == NULL || transport->config.clock == NULL)
        return NULL;
    double now = transport_now(transport);
    if (!isfinite(now)) return NULL;
    for (size_t i = 0u; i < transport->config.path_capacity; ++i) {
        rns_path_entry *entry = &transport->paths[i];
        if (!entry->occupied ||
            memcmp(entry->destination_hash, destination_hash, 16u) != 0)
            continue;
        if (!live(now, entry->expires_at)) {
            memset(entry, 0, sizeof *entry);
            return NULL;
        }
        return entry;
    }
    return NULL;
}

int rns_transport_mark_unresponsive(rns_transport *transport,
                                    const uint8_t destination_hash[16]) {
    rns_path_entry *entry = (rns_path_entry *)rns_transport_lookup(
        transport, destination_hash);
    if (entry == NULL) return 0;
    entry->unresponsive = 1;
    return 1;
}

static bool path_heard_blob(const rns_path_entry *entry,
                            const uint8_t blob[10]) {
    for (size_t i = 0u; i < entry->random_blob_count; ++i)
        if (memcmp(entry->random_blobs[i], blob, 10u) == 0) return true;
    return false;
}

static void path_add_blob(rns_path_entry *entry, const uint8_t blob[10],
                          size_t history) {
    if (path_heard_blob(entry, blob)) return;
    if (entry->random_blob_count < history) {
        memcpy(entry->random_blobs[entry->random_blob_count++], blob, 10u);
        return;
    }
    if (history > 1u)
        memmove(entry->random_blobs, entry->random_blobs[1],
                (history - 1u) * 10u);
    memcpy(entry->random_blobs[history - 1u], blob, 10u);
    entry->random_blob_count = history;
}

static size_t path_replacement_slot(rns_transport *transport) {
    size_t oldest = 0u;
    for (size_t i = 0u; i < transport->config.path_capacity; ++i) {
        if (!transport->paths[i].occupied) return i;
        if (transport->paths[i].updated_at < transport->paths[oldest].updated_at)
            oldest = i;
    }
    return oldest;
}

rns_path_result rns_transport_consider_announce(
    rns_transport *transport, const uint8_t destination_hash[16],
    const uint8_t next_hop[16], uint64_t interface_id,
    int32_t interface_gravity, uint8_t hops,
    const uint8_t random_blob[10], const uint8_t packet_hash[32]) {
    if (transport == NULL || destination_hash == NULL || next_hop == NULL ||
        random_blob == NULL || packet_hash == NULL || transport->paths == NULL ||
        transport->config.clock == NULL)
        return RNS_PATH_REJECTED;
    double now = transport_now(transport);
    if (!isfinite(now)) return RNS_PATH_REJECTED;
    uint64_t timebase = random_blob_timebase(random_blob);
    rns_path_entry *entry = NULL;
    bool expired_match = false;
    for (size_t i = 0u; i < transport->config.path_capacity; ++i)
        if (transport->paths[i].occupied &&
            memcmp(transport->paths[i].destination_hash, destination_hash,
                   16u) == 0) {
            entry = &transport->paths[i];
            expired_match = now >= entry->expires_at;
            break;
        }

    rns_path_result result = RNS_PATH_INSERTED;
    bool reset_history = false;
    if (entry != NULL && !expired_match) {
        bool heard = path_heard_blob(entry, random_blob);
        bool higher_gravity = interface_gravity > entry->interface_gravity;
        if (!entry->unresponsive && timebase < entry->announce_timebase)
            return RNS_PATH_REJECTED;
        if (!entry->unresponsive && timebase == entry->announce_timebase &&
            !higher_gravity)
            return RNS_PATH_REJECTED;
        if (!entry->unresponsive && heard && !higher_gravity)
            return RNS_PATH_REJECTED;
        result = RNS_PATH_UPDATED;
    } else if (entry != NULL) {
        result = RNS_PATH_UPDATED;
        reset_history = true;
    } else {
        entry = &transport->paths[path_replacement_slot(transport)];
        reset_history = true;
    }

    uint8_t old_blobs[RNS_TRANSPORT_MAX_RANDOM_BLOBS][10];
    size_t old_count = reset_history ? 0u : entry->random_blob_count;
    if (old_count != 0u) memcpy(old_blobs, entry->random_blobs,
                                old_count * 10u);
    memset(entry, 0, sizeof *entry);
    memcpy(entry->destination_hash, destination_hash, 16u);
    memcpy(entry->next_hop, next_hop, 16u);
    memcpy(entry->announce_packet_hash, packet_hash, 32u);
    if (old_count != 0u) {
        memcpy(entry->random_blobs, old_blobs, old_count * 10u);
        entry->random_blob_count = old_count;
    }
    path_add_blob(entry, random_blob, transport->config.random_blob_history);
    entry->announce_timebase = timebase;
    entry->interface_id = interface_id;
    entry->interface_gravity = interface_gravity;
    entry->hops = hops;
    entry->updated_at = now;
    entry->expires_at = now + transport->config.path_lifetime;
    entry->occupied = 1;
    return result;
}

static size_t oldest_dedupe_slot(rns_transport *transport) {
    size_t oldest = 0u;
    for (size_t i = 0u; i < transport->config.dedupe_capacity; ++i) {
        if (!transport->dedupe[i].occupied) return i;
        if (transport->dedupe[i].seen_at < transport->dedupe[oldest].seen_at)
            oldest = i;
    }
    return oldest;
}

int rns_transport_accept_packet_hash(rns_transport *transport,
                                     const uint8_t packet_hash[32]) {
    if (transport == NULL || packet_hash == NULL || transport->dedupe == NULL ||
        transport->config.clock == NULL)
        return 0;
    double now = transport_now(transport);
    if (!isfinite(now)) return 0;
    for (size_t i = 0u; i < transport->config.dedupe_capacity; ++i) {
        rns_dedupe_entry *entry = &transport->dedupe[i];
        if (entry->occupied && now >= entry->expires_at)
            memset(entry, 0, sizeof *entry);
        else if (entry->occupied &&
                 memcmp(entry->packet_hash, packet_hash, 32u) == 0)
            return 0;
    }
    rns_dedupe_entry *entry =
        &transport->dedupe[oldest_dedupe_slot(transport)];
    memset(entry, 0, sizeof *entry);
    memcpy(entry->packet_hash, packet_hash, 32u);
    entry->seen_at = now;
    entry->expires_at = now + transport->config.dedupe_lifetime;
    entry->occupied = 1;
    return 1;
}

static size_t oldest_reverse_slot(rns_transport *transport, double now) {
    size_t oldest = 0u;
    for (size_t i = 0u; i < transport->config.reverse_capacity; ++i) {
        rns_reverse_entry *entry = &transport->reverse_paths[i];
        if (!entry->occupied || now >= entry->expires_at) return i;
        if (entry->created_at < transport->reverse_paths[oldest].created_at)
            oldest = i;
    }
    return oldest;
}

int rns_transport_record_reverse(rns_transport *transport,
                                 const uint8_t packet_hash[32],
                                 uint64_t received_interface_id,
                                 uint64_t outbound_interface_id) {
    return rns_transport_record_reverse_transaction(
        transport, packet_hash, received_interface_id, outbound_interface_id,
        NULL);
}

int rns_transport_record_reverse_transaction(
    rns_transport *transport, const uint8_t packet_hash[32],
    uint64_t received_interface_id, uint64_t outbound_interface_id,
    rns_transport_transaction *transaction) {
    if (transaction != NULL) memset(transaction, 0, sizeof *transaction);
    if (transport == NULL || packet_hash == NULL ||
        transport->reverse_paths == NULL || transport->config.clock == NULL)
        return 0;
    double now = transport_now(transport);
    if (!isfinite(now)) return 0;
    size_t slot = oldest_reverse_slot(transport, now);
    rns_reverse_entry *entry = &transport->reverse_paths[slot];
    if (transaction != NULL) {
        transaction->kind = RNS_TRANSPORT_TRANSACTION_REVERSE;
        transaction->slot = slot;
        transaction->previous.reverse = *entry;
    }
    memset(entry, 0, sizeof *entry);
    memcpy(entry->packet_hash, packet_hash, 16u);
    entry->received_interface_id = received_interface_id;
    entry->outbound_interface_id = outbound_interface_id;
    entry->created_at = now;
    entry->expires_at = now + transport->config.reverse_lifetime;
    entry->occupied = 1;
    return 1;
}

rns_reverse_result rns_transport_consume_reverse(
    rns_transport *transport, const uint8_t packet_hash[16],
    uint64_t ingress_interface_id, uint64_t *forward_interface_id) {
    if (forward_interface_id != NULL) *forward_interface_id = 0u;
    if (transport == NULL || packet_hash == NULL ||
        forward_interface_id == NULL || transport->reverse_paths == NULL ||
        transport->config.clock == NULL)
        return RNS_REVERSE_MISSING;
    double now = transport_now(transport);
    if (!isfinite(now)) return RNS_REVERSE_MISSING;
    for (size_t i = 0u; i < transport->config.reverse_capacity; ++i) {
        rns_reverse_entry *entry = &transport->reverse_paths[i];
        if (!entry->occupied || memcmp(entry->packet_hash, packet_hash, 16u) != 0)
            continue;
        if (now >= entry->expires_at) {
            memset(entry, 0, sizeof *entry);
            return RNS_REVERSE_MISSING;
        }
        uint64_t upstream = entry->received_interface_id;
        bool matches = ingress_interface_id == entry->outbound_interface_id;
        memset(entry, 0, sizeof *entry);
        if (!matches) return RNS_REVERSE_WRONG_INTERFACE;
        *forward_interface_id = upstream;
        return RNS_REVERSE_MATCHED;
    }
    return RNS_REVERSE_MISSING;
}

int rns_transport_set_path_identity(rns_transport *transport,
                                    const uint8_t destination_hash[16],
                                    const uint8_t public_key[64]) {
    rns_path_entry *entry = (rns_path_entry *)rns_transport_lookup(
        transport, destination_hash);
    if (entry == NULL || public_key == NULL) return 0;
    memcpy(entry->identity_public_key, public_key, 64u);
    entry->has_identity = 1;
    return 1;
}

static size_t oldest_link_slot(rns_transport *transport, double now) {
    size_t oldest = 0u;
    for (size_t i = 0u; i < transport->config.link_capacity; ++i) {
        rns_transport_link_entry *entry = &transport->links[i];
        double deadline = entry->validated ? entry->expires_at
                                           : entry->proof_deadline;
        if (!entry->occupied || now >= deadline) return i;
        if (entry->created_at < transport->links[oldest].created_at) oldest = i;
    }
    return oldest;
}

int rns_transport_record_link_request(
    rns_transport *transport, const uint8_t link_id[16],
    const rns_path_entry *path, uint64_t received_interface_id,
    uint8_t taken_hops) {
    return rns_transport_record_link_request_transaction(
        transport, link_id, path, received_interface_id, taken_hops, NULL);
}

int rns_transport_record_link_request_transaction(
    rns_transport *transport, const uint8_t link_id[16],
    const rns_path_entry *path, uint64_t received_interface_id,
    uint8_t taken_hops, rns_transport_transaction *transaction) {
    if (transaction != NULL) memset(transaction, 0, sizeof *transaction);
    if (transport == NULL || link_id == NULL || path == NULL ||
        !path->occupied || !path->has_identity || transport->links == NULL ||
        transport->config.clock == NULL)
        return 0;
    double now = transport_now(transport);
    double interval = (double)path->hops *
                      transport->config.link_proof_timeout_per_hop;
    if (!isfinite(now) || !isfinite(interval)) return 0;
    size_t slot = oldest_link_slot(transport, now);
    rns_transport_link_entry *entry = &transport->links[slot];
    if (transaction != NULL) {
        transaction->kind = RNS_TRANSPORT_TRANSACTION_LINK;
        transaction->slot = slot;
        transaction->previous.link = *entry;
    }
    memset(entry, 0, sizeof *entry);
    memcpy(entry->link_id, link_id, 16u);
    memcpy(entry->next_hop, path->next_hop, 16u);
    memcpy(entry->destination_hash, path->destination_hash, 16u);
    memcpy(entry->destination_public_key, path->identity_public_key, 64u);
    entry->next_hop_interface_id = path->interface_id;
    entry->received_interface_id = received_interface_id;
    entry->remaining_hops = path->hops;
    entry->taken_hops = taken_hops;
    entry->created_at = now;
    entry->updated_at = now;
    entry->proof_deadline = now + interval;
    entry->occupied = 1;
    return 1;
}

const rns_transport_link_entry *rns_transport_link_lookup(
    rns_transport *transport, const uint8_t link_id[16]) {
    if (transport == NULL || link_id == NULL || transport->links == NULL ||
        transport->config.clock == NULL)
        return NULL;
    double now = transport_now(transport);
    if (!isfinite(now)) return NULL;
    for (size_t i = 0u; i < transport->config.link_capacity; ++i) {
        rns_transport_link_entry *entry = &transport->links[i];
        if (!entry->occupied || memcmp(entry->link_id, link_id, 16u) != 0)
            continue;
        double deadline = entry->validated ? entry->expires_at
                                           : entry->proof_deadline;
        if (now >= deadline) {
            memset(entry, 0, sizeof *entry);
            return NULL;
        }
        return entry;
    }
    return NULL;
}

rns_link_route_result rns_transport_accept_link_proof(
    rns_transport *transport, const uint8_t link_id[16],
    const uint8_t *proof, size_t proof_length, uint64_t ingress_interface_id,
    uint8_t proof_hops, uint64_t *forward_interface_id) {
    return rns_transport_accept_link_proof_transaction(
        transport, link_id, proof, proof_length, ingress_interface_id,
        proof_hops, forward_interface_id, NULL);
}

rns_link_route_result rns_transport_accept_link_proof_transaction(
    rns_transport *transport, const uint8_t link_id[16],
    const uint8_t *proof, size_t proof_length, uint64_t ingress_interface_id,
    uint8_t proof_hops, uint64_t *forward_interface_id,
    rns_transport_transaction *transaction) {
    if (transaction != NULL) memset(transaction, 0, sizeof *transaction);
    if (forward_interface_id != NULL) *forward_interface_id = 0u;
    rns_transport_link_entry *entry = (rns_transport_link_entry *)
        rns_transport_link_lookup(transport, link_id);
    if (entry == NULL || forward_interface_id == NULL)
        return RNS_LINK_ROUTE_MISSING;
    if (entry->validated) return RNS_LINK_ROUTE_INVALID_PROOF;
    if (ingress_interface_id != entry->next_hop_interface_id)
        return RNS_LINK_ROUTE_WRONG_INTERFACE;
    if (proof_hops != entry->remaining_hops)
        return RNS_LINK_ROUTE_WRONG_HOPS;
    if (proof == NULL || proof_length != 99u)
        return RNS_LINK_ROUTE_INVALID_PROOF;
    uint8_t preimage[83];
    memcpy(preimage, entry->link_id, 16u);
    memcpy(preimage + 16u, proof + 64u, 32u);
    memcpy(preimage + 48u, entry->destination_public_key + 32u, 32u);
    memcpy(preimage + 80u, proof + 96u, 3u);
    rns_identity identity;
    if (!rns_identity_from_public(&identity, entry->destination_public_key) ||
        !rns_identity_verify(&identity, preimage, sizeof preimage, proof))
        return RNS_LINK_ROUTE_INVALID_PROOF;
    double now = transport_now(transport);
    if (!isfinite(now)) return RNS_LINK_ROUTE_INVALID_PROOF;
    if (transaction != NULL) {
        transaction->kind = RNS_TRANSPORT_TRANSACTION_LINK;
        transaction->slot = (size_t)(entry - transport->links);
        transaction->previous.link = *entry;
    }
    entry->validated = 1;
    entry->updated_at = now;
    entry->expires_at = now + transport->config.link_lifetime;
    *forward_interface_id = entry->received_interface_id;
    return RNS_LINK_ROUTE_MATCHED;
}

rns_link_route_result rns_transport_route_link(
    rns_transport *transport, const uint8_t link_id[16],
    uint64_t ingress_interface_id, uint8_t packet_hops,
    uint64_t *forward_interface_id) {
    return rns_transport_route_link_transaction(
        transport, link_id, ingress_interface_id, packet_hops,
        forward_interface_id, NULL);
}

rns_link_route_result rns_transport_route_link_transaction(
    rns_transport *transport, const uint8_t link_id[16],
    uint64_t ingress_interface_id, uint8_t packet_hops,
    uint64_t *forward_interface_id, rns_transport_transaction *transaction) {
    if (transaction != NULL) memset(transaction, 0, sizeof *transaction);
    if (forward_interface_id != NULL) *forward_interface_id = 0u;
    rns_transport_link_entry *entry = (rns_transport_link_entry *)
        rns_transport_link_lookup(transport, link_id);
    if (entry == NULL || forward_interface_id == NULL)
        return RNS_LINK_ROUTE_MISSING;
    if (!entry->validated) return RNS_LINK_ROUTE_NOT_VALIDATED;
    uint8_t expected;
    if (ingress_interface_id == entry->received_interface_id) {
        expected = entry->taken_hops;
        *forward_interface_id = entry->next_hop_interface_id;
    } else if (ingress_interface_id == entry->next_hop_interface_id) {
        expected = entry->remaining_hops;
        *forward_interface_id = entry->received_interface_id;
    } else {
        return RNS_LINK_ROUTE_WRONG_INTERFACE;
    }
    if (packet_hops != expected) {
        *forward_interface_id = 0u;
        return RNS_LINK_ROUTE_WRONG_HOPS;
    }
    if (transaction != NULL) {
        transaction->kind = RNS_TRANSPORT_TRANSACTION_LINK;
        transaction->slot = (size_t)(entry - transport->links);
        transaction->previous.link = *entry;
    }
    entry->updated_at = transport_now(transport);
    return RNS_LINK_ROUTE_MATCHED;
}

int rns_transport_forget_link(rns_transport *transport,
                              const uint8_t link_id[16]) {
    rns_transport_link_entry *entry = (rns_transport_link_entry *)
        rns_transport_link_lookup(transport, link_id);
    if (entry == NULL) return 0;
    memset(entry, 0, sizeof *entry);
    return 1;
}

int rns_transport_transaction_rollback(
    rns_transport *transport, rns_transport_transaction *transaction) {
    if (transport == NULL || transaction == NULL) return 0;
    switch (transaction->kind) {
        case RNS_TRANSPORT_TRANSACTION_REVERSE:
            if (transport->reverse_paths == NULL ||
                transaction->slot >= transport->config.reverse_capacity)
                return 0;
            transport->reverse_paths[transaction->slot] =
                transaction->previous.reverse;
            break;
        case RNS_TRANSPORT_TRANSACTION_LINK:
            if (transport->links == NULL ||
                transaction->slot >= transport->config.link_capacity)
                return 0;
            transport->links[transaction->slot] = transaction->previous.link;
            break;
        case RNS_TRANSPORT_TRANSACTION_NONE:
            return 1;
        default:
            return 0;
    }
    memset(transaction, 0, sizeof *transaction);
    return 1;
}

void rns_transport_transaction_commit(
    rns_transport_transaction *transaction) {
    if (transaction != NULL) memset(transaction, 0, sizeof *transaction);
}

int rns_path_request_build(const uint8_t destination_hash[16],
                           const uint8_t requesting_transport[16],
                           const uint8_t *tag, size_t tag_length,
                           uint8_t *output, size_t output_capacity,
                           size_t *output_length) {
    if (output_length != NULL) *output_length = 0u;
    if (destination_hash == NULL || tag == NULL || tag_length == 0u ||
        tag_length > RNS_PATH_REQUEST_MAX_TAG_SIZE || output == NULL ||
        output_length == NULL)
        return 0;
    size_t required = 16u + tag_length +
                      (requesting_transport == NULL ? 0u : 16u);
    if (output_capacity < required) return 0;
    memcpy(output, destination_hash, 16u);
    size_t offset = 16u;
    if (requesting_transport != NULL) {
        memcpy(output + offset, requesting_transport, 16u);
        offset += 16u;
    }
    memcpy(output + offset, tag, tag_length);
    *output_length = required;
    return 1;
}

int rns_path_request_parse(rns_path_request *request, const uint8_t *input,
                           size_t input_length) {
    if (request == NULL || input == NULL || input_length < 17u ||
        input_length > 48u)
        return 0;
    memset(request, 0, sizeof *request);
    memcpy(request->destination_hash, input, 16u);
    size_t offset = 16u;
    if (input_length >= 33u) {
        request->has_requesting_transport = 1;
        memcpy(request->requesting_transport, input + offset, 16u);
        offset += 16u;
    }
    request->tag_length = input_length - offset;
    if (request->tag_length == 0u ||
        request->tag_length > RNS_PATH_REQUEST_MAX_TAG_SIZE)
        return 0;
    memcpy(request->tag, input + offset, request->tag_length);
    return 1;
}
