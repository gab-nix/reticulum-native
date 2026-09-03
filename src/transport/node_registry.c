#define _POSIX_C_SOURCE 200809L
#include "reticulum/node_registry.h"
#include "reticulum/lxmf_router.h"
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "reticulum/destination.h"
#define REGISTRY_MAGIC "RNSN3\0\0\0"
#define REGISTRY_MAGIC_V2 "RNSN2\0\0\0"
#define REGISTRY_MAGIC_V1 "RNSN1\0\0\0"
#define REGISTRY_HEADER_SIZE 24u
#define REGISTRY_RECORD_PREFIX_SIZE 197u
#define REGISTRY_MAX_FILE_SIZE (512u * 1024u)
#define REGISTRY_PATH_MAX 4096u
#define REGISTRY_RECORD_VERSION 1u

_Static_assert(sizeof(double) == sizeof(uint64_t),
               "node registry persistence requires 64-bit doubles");

enum {
    RECORD_REACHABLE = 1u << 0,
    RECORD_PROPAGATION = 1u << 1,
    RECORD_HAS_RATCHET = 1u << 2,
    RECORD_HAS_MESSAGE_DESTINATION = 1u << 3,
    RECORD_LXMF_APP_DATA_VALID = 1u << 4,
    RECORD_LXMF_HAS_STAMP_COST = 1u << 5,
    RECORD_KNOWN_FLAGS = (1u << 6) - 1u
};

typedef struct {
    uint8_t destination[16],next_hop[16],public_key[64],message_destination[16];
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];size_t app_data_length;
    uint64_t announce_timebase;uint8_t hops;uint64_t interface_id;int32_t gravity;
    double seen_at,expires_at;bool reachable,propagation,has_ratchet,has_message_destination;
    rns_node_kind kind;char name[64];
} rns_node_record_v1;

/* Exact in-memory schema written by RNSN2 on this ABI. It is migration-only.
 * Raw bytes are copied field-by-field, and padding is never inspected. */
typedef struct {
    uint8_t destination[16],next_hop[16],public_key[64],message_destination[16];
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];size_t app_data_length;
    uint64_t announce_timebase;uint8_t hops;uint64_t interface_id;int32_t gravity;
    double seen_at,expires_at;bool reachable,propagation,has_ratchet,has_message_destination;
    rns_node_kind kind;char name[64];uint8_t ratchet[32];bool lxmf_app_data_valid;
    bool lxmf_has_stamp_cost;uint8_t lxmf_stamp_cost;uint32_t lxmf_features;
} rns_node_record_v2;

static bool legacy_bool(const uint8_t *raw, size_t offset, bool *value) {
    if (sizeof(bool) != 1U || raw[offset] > 1U) return false;
    *value = raw[offset] != 0U;
    return true;
}

#define LEGACY_COPY_SCALAR(target, raw, legacy_type, member) \
    memcpy(&(target)->member, (raw) + offsetof(legacy_type, member), \
           sizeof((target)->member))
#define LEGACY_COPY_ARRAY(target, raw, legacy_type, member) \
    memcpy((target)->member, (raw) + offsetof(legacy_type, member), \
           sizeof((target)->member))

static bool valid_record(const rns_node_record *record) {
    return record->app_data_length <= RNS_NODE_APP_DATA_MAX &&
           record->persistence_extensions_length <=
               RNS_NODE_PERSISTENCE_EXT_MAX &&
           record->kind >= RNS_NODE_KIND_OTHER &&
           record->kind <= RNS_NODE_KIND_LXMF &&
           isfinite(record->seen_at) && isfinite(record->expires_at);
}

static bool migrate_v1(rns_node_record *to, const uint8_t *raw) {
    memset(to, 0, sizeof *to);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v1, destination);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v1, next_hop);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v1, public_key);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v1, message_destination);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v1, app_data);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, app_data_length);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, announce_timebase);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, hops);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, interface_id);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, gravity);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, seen_at);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, expires_at);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v1, kind);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v1, name);
    bool discarded_ratchet_flag;
    if (!legacy_bool(raw, offsetof(rns_node_record_v1, reachable),
                     &to->reachable) ||
        !legacy_bool(raw, offsetof(rns_node_record_v1, propagation),
                     &to->propagation) ||
        !legacy_bool(raw, offsetof(rns_node_record_v1, has_ratchet),
                     &discarded_ratchet_flag) ||
        !legacy_bool(raw, offsetof(rns_node_record_v1,
                                   has_message_destination),
                     &to->has_message_destination)) return false;
    /* RNSN1 had a ratchet-presence flag but no ratchet bytes. Do not migrate a
     * zero key as usable cryptographic state. */
    to->has_ratchet = false;
    to->name[sizeof to->name - 1U] = '\0';
    return valid_record(to);
}

static bool migrate_v2(rns_node_record *to, const uint8_t *raw) {
    memset(to, 0, sizeof *to);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, destination);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, next_hop);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, public_key);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, message_destination);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, app_data);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, app_data_length);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, announce_timebase);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, hops);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, interface_id);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, gravity);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, seen_at);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, expires_at);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, kind);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, name);
    LEGACY_COPY_ARRAY(to, raw, rns_node_record_v2, ratchet);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, lxmf_stamp_cost);
    LEGACY_COPY_SCALAR(to, raw, rns_node_record_v2, lxmf_features);
    if (!legacy_bool(raw, offsetof(rns_node_record_v2, reachable),
                     &to->reachable) ||
        !legacy_bool(raw, offsetof(rns_node_record_v2, propagation),
                     &to->propagation) ||
        !legacy_bool(raw, offsetof(rns_node_record_v2, has_ratchet),
                     &to->has_ratchet) ||
        !legacy_bool(raw, offsetof(rns_node_record_v2,
                                   has_message_destination),
                     &to->has_message_destination) ||
        !legacy_bool(raw, offsetof(rns_node_record_v2, lxmf_app_data_valid),
                     &to->lxmf_app_data_valid) ||
        !legacy_bool(raw, offsetof(rns_node_record_v2, lxmf_has_stamp_cost),
                     &to->lxmf_has_stamp_cost)) return false;
    to->name[sizeof to->name - 1U] = '\0';
    return valid_record(to);
}

void rns_node_registry_init(rns_node_registry *r, double lifetime) { if (r) { memset(r, 0, sizeof(*r)); r->lifetime = lifetime > 0 ? lifetime : 3600.0; } }
int rns_node_registry_upsert(rns_node_registry *r, const rns_node_record *x) {
    if (!r || !x) return 0; size_t i; for (i=0;i<r->count;i++) if (!memcmp(r->records[i].destination,x->destination,16)) break;
    if (i==r->count) { if (r->count==RNS_NODE_REGISTRY_MAX) return 0; r->count++; }
    r->records[i]=*x; if (r->records[i].expires_at <= 0.0) r->records[i].expires_at=r->records[i].seen_at+r->lifetime; r->records[i].name[sizeof(r->records[i].name)-1]=0; return 1;
}
size_t rns_node_registry_expire(rns_node_registry *r, double now) { if (!r) return 0; size_t n=0; for(size_t i=0;i<r->count;) { if(r->records[i].expires_at>0&&r->records[i].expires_at<=now){r->records[i]=r->records[--r->count];n++;} else i++; } return n; }
const rns_node_record *rns_node_registry_get(const rns_node_registry *r,const uint8_t d[16]) { if(!r||!d)return NULL; for(size_t i=0;i<r->count;i++)if(!memcmp(r->records[i].destination,d,16))return &r->records[i]; return NULL; }
static bool contains_ascii_casefold(const char *haystack, const char *needle) {
    if (*needle == '\0') return true;
    for (size_t i = 0u; haystack[i] != '\0'; ++i) {
        size_t j = 0u;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               tolower((unsigned char)haystack[i + j]) ==
                   tolower((unsigned char)needle[j]))
            ++j;
        if (needle[j] == '\0') return true;
    }
    return false;
}

static bool record_matches(const rns_node_record *record, const char *filter) {
    if (filter == NULL || *filter == '\0') return true;
    if (contains_ascii_casefold(record->name, filter)) return true;
    char address[33];
    for (size_t i = 0u; i < sizeof record->destination; ++i)
        (void)snprintf(address + i * 2u, 3u, "%02x", record->destination[i]);
    return contains_ascii_casefold(address, filter);
}

size_t rns_node_registry_list(const rns_node_registry *r, rns_node_record *out,
                              size_t cap, const char *filter) {
    if (r == NULL) return 0u;
    size_t count = 0u;
    for (size_t i = 0u; i < r->count && count < cap; ++i) {
        if (!record_matches(&r->records[i], filter)) continue;
        if (out != NULL) out[count] = r->records[i];
        ++count;
    }
    return count;
}
int rns_node_registry_consider_announce(rns_node_registry *r,const rns_node_result *a){
    if(!r||!a||!a->has_verified_announce||
       a->announce_app_data_length>RNS_NODE_APP_DATA_MAX||
       (a->announce_app_data_length&&!a->announce_app_data)||
       (a->announce_has_ratchet&&!a->announce_ratchet))return 0;
    const rns_node_record *old=rns_node_registry_get(r,a->destination_hash);
    if(old&&old->announce_timebase>=a->announce_timebase)return 0;
    rns_node_record n={0};
    memcpy(n.destination,a->destination_hash,16);memcpy(n.next_hop,a->next_hop,16);
    rns_identity_export_public(&a->announce_identity,n.public_key);
    if(a->announce_app_data_length)memcpy(n.app_data,a->announce_app_data,a->announce_app_data_length);
    n.app_data_length=a->announce_app_data_length;n.announce_timebase=a->announce_timebase;
    n.hops=a->hops;n.interface_id=a->received_interface_id;n.seen_at=a->received_at;
    n.expires_at=a->received_at+r->lifetime;n.reachable=true;
    n.has_ratchet=a->announce_has_ratchet!=0;
    if(n.has_ratchet)memcpy(n.ratchet,a->announce_ratchet,sizeof n.ratchet);
    const char *node_aspects[]={"node"};
    const char *delivery_aspects[]={"delivery"};
    const char *propagation_aspects[]={"propagation"};
    uint8_t node_hash[16],delivery_hash[16],propagation_hash[16];
    if(rns_destination_hash(&a->announce_identity,"nomadnetwork",node_aspects,1,node_hash)&&
       !memcmp(node_hash,a->destination_hash,16)){
        n.kind=RNS_NODE_KIND_NOMAD;
        if(rns_destination_hash(&a->announce_identity,"lxmf",delivery_aspects,1,delivery_hash)){
            memcpy(n.message_destination,delivery_hash,16);n.has_message_destination=true;
        }
        size_t name_length=a->announce_app_data_length<sizeof(n.name)-1?a->announce_app_data_length:sizeof(n.name)-1;
        for(size_t i=0;i<name_length;i++){unsigned char c=a->announce_app_data[i];n.name[i]=c>=32u&&c!=127u?(char)c:'?';}
    }else if(rns_destination_hash(&a->announce_identity,"lxmf",delivery_aspects,1,delivery_hash)&&
             !memcmp(delivery_hash,a->destination_hash,16)){
        lxmf_announce_data_t decoded;
        n.kind=RNS_NODE_KIND_LXMF;memcpy(n.message_destination,a->destination_hash,16);
        n.has_message_destination=true;
        if(a->announce_app_data_length&&
           lxmf_announce_parse(a->announce_app_data,a->announce_app_data_length,&decoded)==LXMF_OK){
            n.lxmf_app_data_valid=true;n.lxmf_has_stamp_cost=decoded.has_stamp_cost;
            n.lxmf_stamp_cost=decoded.stamp_cost;n.lxmf_features=decoded.features;
            size_t name_length=decoded.display_name_len<sizeof(n.name)-1?decoded.display_name_len:sizeof(n.name)-1;
            memcpy(n.name,decoded.display_name,name_length);n.name[name_length]='\0';
        }
    }else if(rns_destination_hash(&a->announce_identity,"lxmf",propagation_aspects,1,propagation_hash)&&
             !memcmp(propagation_hash,a->destination_hash,16)){
        n.kind=RNS_NODE_KIND_LXMF;n.propagation=true;
    }
    return rns_node_registry_upsert(r,&n);
}
static int before(const rns_node_record *a,const rns_node_record *b){if(a->reachable!=b->reachable)return a->reachable;if(a->seen_at!=b->seen_at)return a->seen_at>b->seen_at;return memcmp(a->destination,b->destination,16)<0;}
size_t rns_node_registry_sorted_filter(const rns_node_registry *r,
                                       rns_node_record *out, size_t cap,
                                       const char *filter) {
    size_t count = rns_node_registry_list(r, out, cap, filter);
    for (size_t i = 1u; i < count; ++i) {
        rns_node_record value = out[i];
        size_t j = i;
        while (j != 0u && before(&value, &out[j - 1u])) {
            out[j] = out[j - 1u];
            --j;
        }
        out[j] = value;
    }
    return count;
}

size_t rns_node_registry_sorted(const rns_node_registry *r, rns_node_record *out,
                                size_t cap) {
    return rns_node_registry_sorted_filter(r, out, cap, NULL);
}
static void put16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void put32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void put64(uint8_t *output, uint64_t value) {
    for (size_t i = 0U; i < 8U; ++i)
        output[i] = (uint8_t)(value >> (56U - i * 8U));
}

static uint16_t get16(const uint8_t *input) {
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t get32(const uint8_t *input) {
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | input[3];
}

static uint64_t get64(const uint8_t *input) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < 8U; ++i) value = (value << 8U) | input[i];
    return value;
}

static uint32_t crc32_bytes(const uint8_t *input, size_t length) {
    uint32_t crc = UINT32_MAX;
    while (length-- != 0U) {
        crc ^= *input++;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^
                  (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static size_t bounded_name_length(const char name[64]) {
    const char *end = memchr(name, '\0', 64U);
    return end == NULL ? SIZE_MAX : (size_t)(end - name);
}

static bool encode_record(const rns_node_record *record, uint8_t *output,
                          size_t capacity, size_t *encoded_length) {
    if (!valid_record(record)) return false;
    size_t name_length = bounded_name_length(record->name);
    if (name_length == SIZE_MAX) return false;
    size_t record_length = REGISTRY_RECORD_PREFIX_SIZE +
                           record->app_data_length + name_length +
                           record->persistence_extensions_length;
    if (record_length > UINT32_MAX || capacity < 4U + record_length)
        return false;
    put32(output, (uint32_t)record_length);
    uint8_t *cursor = output + 4U;
    put16(cursor, REGISTRY_RECORD_VERSION); cursor += 2U;
    uint16_t flags = (record->reachable ? RECORD_REACHABLE : 0U) |
                     (record->propagation ? RECORD_PROPAGATION : 0U) |
                     (record->has_ratchet ? RECORD_HAS_RATCHET : 0U) |
                     (record->has_message_destination
                          ? RECORD_HAS_MESSAGE_DESTINATION : 0U) |
                     (record->lxmf_app_data_valid
                          ? RECORD_LXMF_APP_DATA_VALID : 0U) |
                     (record->lxmf_has_stamp_cost
                          ? RECORD_LXMF_HAS_STAMP_COST : 0U);
    put16(cursor, flags); cursor += 2U;
#define ENCODE_ARRAY(member) do { \
    memcpy(cursor, record->member, sizeof record->member); \
    cursor += sizeof record->member; \
} while (0)
    ENCODE_ARRAY(destination);
    ENCODE_ARRAY(next_hop);
    ENCODE_ARRAY(public_key);
    ENCODE_ARRAY(message_destination);
#undef ENCODE_ARRAY
    put16(cursor, (uint16_t)record->app_data_length); cursor += 2U;
    put64(cursor, record->announce_timebase); cursor += 8U;
    *cursor++ = record->hops;
    *cursor++ = (uint8_t)record->kind;
    *cursor++ = record->lxmf_stamp_cost;
    *cursor++ = 0U;
    put64(cursor, record->interface_id); cursor += 8U;
    uint32_t gravity_bits;
    memcpy(&gravity_bits, &record->gravity, sizeof gravity_bits);
    put32(cursor, gravity_bits); cursor += 4U;
    uint64_t bits;
    memcpy(&bits, &record->seen_at, sizeof bits);
    put64(cursor, bits); cursor += 8U;
    memcpy(&bits, &record->expires_at, sizeof bits);
    put64(cursor, bits); cursor += 8U;
    put32(cursor, record->lxmf_features); cursor += 4U;
    *cursor++ = (uint8_t)name_length;
    memcpy(cursor, record->ratchet, sizeof record->ratchet);
    cursor += sizeof record->ratchet;
    put16(cursor, (uint16_t)record->persistence_extensions_length);
    cursor += 2U;
    memcpy(cursor, record->app_data, record->app_data_length);
    cursor += record->app_data_length;
    memcpy(cursor, record->name, name_length); cursor += name_length;
    memcpy(cursor, record->persistence_extensions,
           record->persistence_extensions_length);
    cursor += record->persistence_extensions_length;
    *encoded_length = (size_t)(cursor - output);
    return *encoded_length == 4U + record_length;
}

static bool decode_record(rns_node_record *record, const uint8_t *input,
                          size_t length) {
    if (length < REGISTRY_RECORD_PREFIX_SIZE ||
        get16(input) != REGISTRY_RECORD_VERSION) return false;
    uint16_t flags = get16(input + 2U);
    if ((flags & (uint16_t)~RECORD_KNOWN_FLAGS) != 0U) return false;
    const uint8_t *cursor = input + 4U;
    memset(record, 0, sizeof *record);
#define DECODE_ARRAY(member) do { \
    memcpy(record->member, cursor, sizeof record->member); \
    cursor += sizeof record->member; \
} while (0)
    DECODE_ARRAY(destination);
    DECODE_ARRAY(next_hop);
    DECODE_ARRAY(public_key);
    DECODE_ARRAY(message_destination);
#undef DECODE_ARRAY
    size_t app_data_length = get16(cursor); cursor += 2U;
    record->announce_timebase = get64(cursor); cursor += 8U;
    record->hops = *cursor++;
    record->kind = (rns_node_kind)*cursor++;
    record->lxmf_stamp_cost = *cursor++;
    if (*cursor++ != 0U) return false;
    record->interface_id = get64(cursor); cursor += 8U;
    uint32_t gravity_bits = get32(cursor); cursor += 4U;
    memcpy(&record->gravity, &gravity_bits, sizeof gravity_bits);
    uint64_t bits = get64(cursor); cursor += 8U;
    memcpy(&record->seen_at, &bits, sizeof bits);
    bits = get64(cursor); cursor += 8U;
    memcpy(&record->expires_at, &bits, sizeof bits);
    record->lxmf_features = get32(cursor); cursor += 4U;
    size_t name_length = *cursor++;
    memcpy(record->ratchet, cursor, sizeof record->ratchet);
    cursor += sizeof record->ratchet;
    size_t extensions_length = get16(cursor); cursor += 2U;
    if (app_data_length > RNS_NODE_APP_DATA_MAX || name_length >= 64U ||
        extensions_length > RNS_NODE_PERSISTENCE_EXT_MAX ||
        (size_t)(cursor - input) + app_data_length + name_length +
                extensions_length != length) return false;
    memcpy(record->app_data, cursor, app_data_length); cursor += app_data_length;
    memcpy(record->name, cursor, name_length); cursor += name_length;
    record->name[name_length] = '\0';
    memcpy(record->persistence_extensions, cursor, extensions_length);
    record->app_data_length = app_data_length;
    record->persistence_extensions_length = extensions_length;
    record->reachable = (flags & RECORD_REACHABLE) != 0U;
    record->propagation = (flags & RECORD_PROPAGATION) != 0U;
    record->has_ratchet = (flags & RECORD_HAS_RATCHET) != 0U;
    record->has_message_destination =
        (flags & RECORD_HAS_MESSAGE_DESTINATION) != 0U;
    record->lxmf_app_data_valid =
        (flags & RECORD_LXMF_APP_DATA_VALID) != 0U;
    record->lxmf_has_stamp_cost =
        (flags & RECORD_LXMF_HAS_STAMP_COST) != 0U;
    return valid_record(record);
}

static bool sync_parent(const char *path, size_t length) {
    char *directory = malloc(length + 1U);
    if (directory == NULL) return false;
    memcpy(directory, path, length + 1U);
    char *slash = strrchr(directory, '/');
    if (slash == NULL) memcpy(directory, ".", 2U);
    else if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    int descriptor = open(directory, O_RDONLY);
    free(directory);
    if (descriptor < 0) return false;
    int result = fsync(descriptor);
    (void)close(descriptor);
    return result == 0;
}

int rns_node_registry_save(const rns_node_registry *registry,
                           const char *path) {
    if (registry == NULL || path == NULL ||
        registry->count > RNS_NODE_REGISTRY_MAX)
        return 0;
    size_t path_length = strnlen(path, REGISTRY_PATH_MAX + 1U);
    if (path_length == 0U || path_length > REGISTRY_PATH_MAX) return 0;
    uint8_t *payload = malloc(REGISTRY_MAX_FILE_SIZE);
    if (payload == NULL) return 0;
    size_t payload_length = 0U;
    bool ok = true;
    for (size_t i = 0U; i < registry->count && ok; ++i) {
        size_t encoded_length = 0U;
        ok = encode_record(&registry->records[i], payload + payload_length,
                           REGISTRY_MAX_FILE_SIZE - payload_length,
                           &encoded_length);
        payload_length += encoded_length;
    }
    if (!ok || payload_length > UINT32_MAX) {
        free(payload);
        return 0;
    }
    uint8_t header[REGISTRY_HEADER_SIZE] = {0};
    memcpy(header, REGISTRY_MAGIC, 8U);
    put16(header + 8U, 3U);
    put16(header + 10U, REGISTRY_HEADER_SIZE);
    put32(header + 12U, (uint32_t)registry->count);
    put32(header + 16U, (uint32_t)payload_length);
    put32(header + 20U, crc32_bytes(payload, payload_length));

    char *temporary = malloc(path_length + sizeof ".tmp.XXXXXX");
    if (temporary == NULL) { free(payload); return 0; }
    (void)snprintf(temporary, path_length + sizeof ".tmp.XXXXXX",
                   "%s.tmp.XXXXXX", path);
    int descriptor = mkstemp(temporary);
    FILE *file = descriptor < 0 ? NULL : fdopen(descriptor, "wb");
    if (file == NULL) {
        if (descriptor >= 0) (void)close(descriptor);
        (void)unlink(temporary); free(temporary); free(payload); return 0;
    }
    ok = fwrite(header, 1U, sizeof header, file) == sizeof header &&
         fwrite(payload, 1U, payload_length, file) == payload_length &&
         fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temporary, path) != 0) ok = false;
    if (ok) ok = sync_parent(path, path_length);
    if (!ok) (void)unlink(temporary);
    free(temporary);
    free(payload);
    return ok ? 1 : 0;
}

static bool decode_v3(rns_node_registry *registry, const uint8_t *input,
                      size_t length) {
    if (length < REGISTRY_HEADER_SIZE || memcmp(input, REGISTRY_MAGIC, 8U) != 0 ||
        get16(input + 8U) != 3U ||
        get16(input + 10U) != REGISTRY_HEADER_SIZE) return false;
    uint32_t count = get32(input + 12U);
    uint32_t payload_length = get32(input + 16U);
    if (count > RNS_NODE_REGISTRY_MAX ||
        payload_length != length - REGISTRY_HEADER_SIZE ||
        crc32_bytes(input + REGISTRY_HEADER_SIZE, payload_length) !=
            get32(input + 20U)) return false;
    const uint8_t *cursor = input + REGISTRY_HEADER_SIZE;
    size_t remaining = payload_length;
    for (uint32_t i = 0U; i < count; ++i) {
        if (remaining < 4U) return false;
        uint32_t record_length = get32(cursor); cursor += 4U; remaining -= 4U;
        if (record_length > remaining ||
            !decode_record(&registry->records[i], cursor, record_length))
            return false;
        cursor += record_length; remaining -= record_length;
    }
    if (remaining != 0U) return false;
    registry->count = count;
    return true;
}

static bool decode_legacy(rns_node_registry *registry, const uint8_t *input,
                          size_t length) {
    if (length < 12U) return false;
    uint32_t count;
    memcpy(&count, input + 8U, sizeof count);
    if (count > RNS_NODE_REGISTRY_MAX) return false;
    bool version_one = memcmp(input, REGISTRY_MAGIC_V1, 8U) == 0;
    bool version_two = memcmp(input, REGISTRY_MAGIC_V2, 8U) == 0;
    if (!version_one && !version_two) return false;
    size_t record_size = version_one ? sizeof(rns_node_record_v1)
                                     : sizeof(rns_node_record_v2);
    if (count > (SIZE_MAX - 12U) / record_size ||
        length != 12U + (size_t)count * record_size) return false;
    const uint8_t *cursor = input + 12U;
    for (uint32_t i = 0U; i < count; ++i) {
        bool ok = version_one ? migrate_v1(&registry->records[i], cursor)
                              : migrate_v2(&registry->records[i], cursor);
        if (!ok) return false;
        cursor += record_size;
    }
    registry->count = count;
    return true;
}

int rns_node_registry_load(rns_node_registry *registry, const char *path,
                           double lifetime) {
    if (registry == NULL || path == NULL) return 0;
    size_t path_length = strnlen(path, REGISTRY_PATH_MAX + 1U);
    if (path_length == 0U || path_length > REGISTRY_PATH_MAX) return 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) (void)fclose(file);
        return 0;
    }
    long end = ftell(file);
    if (end < 0 || (unsigned long)end > REGISTRY_MAX_FILE_SIZE ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return 0;
    }
    size_t length = (size_t)end;
    uint8_t *encoded = malloc(length == 0U ? 1U : length);
    if (encoded == NULL) { (void)fclose(file); return 0; }
    bool read_ok = fread(encoded, 1U, length, file) == length;
    if (read_ok && fgetc(file) != EOF) read_ok = false;
    if (fclose(file) != 0) read_ok = false;
    if (!read_ok) { free(encoded); return 0; }
    rns_node_registry *candidate = malloc(sizeof *candidate);
    if (candidate == NULL) { free(encoded); return 0; }
    rns_node_registry_init(candidate, lifetime);
    bool ok = decode_v3(candidate, encoded, length) ||
              decode_legacy(candidate, encoded, length);
    if (ok) *registry = *candidate;
    free(candidate);
    free(encoded);
    return ok ? 1 : 0;
}
