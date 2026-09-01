#ifndef RETICULUM_LXMF_STORE_H
#define RETICULUM_LXMF_STORE_H

#include "reticulum/lxmf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LXMF_STORE_MAX_CONTENT 4096u
#define LXMF_STORE_MAX_MESSAGES 1024u
#define LXMF_STORE_MAX_FILE_SIZE (16u * 1024u * 1024u)
#define LXMF_STORE_PATH_MAX 1023u

typedef enum {
    LXMF_DELIVERY_QUEUED = 0,
    LXMF_DELIVERY_SENDING = 1,
    LXMF_DELIVERY_SENT = 2,
    LXMF_DELIVERY_DELIVERED = 3,
    LXMF_DELIVERY_FAILED = 4
} lxmf_delivery_status_t;

typedef struct {
    uint8_t message_id[LXMF_MESSAGE_ID_LENGTH];
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    uint8_t source[LXMF_SOURCE_LENGTH];
    double timestamp;
    lxmf_delivery_status_t status;
    lxmf_slice_t content;
} lxmf_store_message_t;

typedef struct { void *implementation; } lxmf_store_t;
typedef bool (*lxmf_store_list_fn)(void *context,
                                   const lxmf_store_message_t *message);

lxmf_status_t lxmf_store_open(lxmf_store_t *store, const char *path);
void lxmf_store_close(lxmf_store_t *store);
lxmf_status_t lxmf_store_put(lxmf_store_t *store,
                             const lxmf_store_message_t *message,
                             bool *inserted);
lxmf_status_t lxmf_store_update_status(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_delivery_status_t status);
lxmf_status_t lxmf_store_read(
    lxmf_store_t *store, const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
    lxmf_store_message_t *message, uint8_t *content, size_t content_capacity);
lxmf_status_t lxmf_store_list(lxmf_store_t *store, lxmf_store_list_fn callback,
                              void *context);
/* Writes path.tmp, fsyncs it, and atomically renames it over path. */
lxmf_status_t lxmf_store_compact(lxmf_store_t *store);
size_t lxmf_store_count(const lxmf_store_t *store);

#ifdef __cplusplus
}
#endif
#endif
