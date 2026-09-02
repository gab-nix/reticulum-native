#ifndef RETICULUM_LXMF_ROUTER_H
#define RETICULUM_LXMF_ROUTER_H

#include "reticulum/lxmf.h"
#include "reticulum/lxmf_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_DISPLAY_NAME_MAX 127u
#define LXMF_IDENTITY_PUBLIC_LENGTH 64u
#define LXMF_FEATURE_COMPRESSION 0x00000001u

typedef struct {
    char display_name[LXMF_DISPLAY_NAME_MAX + 1u];
    size_t display_name_len;
    bool has_stamp_cost;
    uint8_t stamp_cost;
    uint32_t features;
} lxmf_announce_data_t;

lxmf_status_t lxmf_announce_encode(const lxmf_announce_data_t *data,
                                   uint8_t *output, size_t capacity,
                                   size_t *output_len);
lxmf_status_t lxmf_announce_parse(const uint8_t *input, size_t input_len,
                                  lxmf_announce_data_t *data);

typedef uint64_t (*lxmf_clock_fn)(void *context);

typedef struct {
    bool used;
    uint8_t delivery_hash[LXMF_DESTINATION_LENGTH];
    uint8_t identity_public[LXMF_IDENTITY_PUBLIC_LENGTH];
    char display_name[LXMF_DISPLAY_NAME_MAX + 1u];
    size_t display_name_len;
    bool has_stamp_cost;
    uint8_t stamp_cost;
    uint32_t features;
    uint64_t last_seen;
} lxmf_contact_t;

typedef struct {
    lxmf_contact_t *entries;
    size_t capacity;
    size_t count;
    lxmf_clock_fn clock;
    void *clock_context;
} lxmf_contact_book_t;

typedef const rns_identity *(*lxmf_router_identity_resolver_fn)(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH]);
typedef lxmf_status_t (*lxmf_router_send_fn)(void *context,
                                             const uint8_t *packet,
                                             size_t packet_length);
typedef struct {
    rns_identity *identity;
    lxmf_store_t *store;
    lxmf_router_identity_resolver_fn resolve_identity;
    void *resolve_context;
    lxmf_router_send_fn send_packet;
    void *send_context;
} lxmf_router_config_t;
typedef struct { lxmf_router_config_t config; } lxmf_router_t;

lxmf_status_t lxmf_router_init(lxmf_router_t *router,
                               const lxmf_router_config_t *config);
lxmf_status_t lxmf_router_send_message(lxmf_router_t *router,
                                       const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH]);

lxmf_status_t lxmf_contact_book_init(lxmf_contact_book_t *book,
                                     lxmf_contact_t *storage, size_t capacity,
                                     lxmf_clock_fn clock, void *clock_context);
lxmf_status_t lxmf_contact_book_update(
    lxmf_contact_book_t *book,
    const uint8_t delivery_hash[LXMF_DESTINATION_LENGTH],
    const uint8_t identity_public[LXMF_IDENTITY_PUBLIC_LENGTH],
    const lxmf_announce_data_t *announce);
const lxmf_contact_t *lxmf_contact_book_lookup(
    const lxmf_contact_book_t *book,
    const uint8_t delivery_hash[LXMF_DESTINATION_LENGTH]);
size_t lxmf_contact_book_expire(lxmf_contact_book_t *book,
                                uint64_t max_age);

#ifdef __cplusplus
}
#endif
#endif
