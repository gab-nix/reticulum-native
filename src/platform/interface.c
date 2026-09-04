#include "reticulum/interface.h"

#include <string.h>

#include "reticulum/hal.h"

struct rns_interface {
    const rns_interface_ops_t *ops;
    const rns_platform_ops_t *platform;
    void *context;
    int started;
};

rns_status_t rns_interface_create(const rns_interface_ops_t *ops, void *context,
                                  rns_interface_t **interface_out) {
    const rns_platform_ops_t *platform;
    rns_interface_t *created;
    if (interface_out == NULL || ops == NULL || ops->start == NULL ||
        ops->poll == NULL || ops->send == NULL || ops->get_stats == NULL ||
        ops->stop == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *interface_out = NULL;
    platform = rns_platform_current();
    if (platform == NULL) return RNS_ERROR_INVALID_STATE;
    created = platform->allocate(platform->context, sizeof(*created));
    if (created == NULL) return RNS_ERROR_NO_MEMORY;
    memset(created, 0, sizeof(*created));
    created->ops = ops;
    created->platform = platform;
    created->context = context;
    *interface_out = created;
    return RNS_OK;
}

void rns_interface_destroy(rns_interface_t *interface_value) {
    if (interface_value == NULL) return;
    if (interface_value->started != 0) {
        interface_value->ops->stop(interface_value->context);
    }
    if (interface_value->ops->destroy != NULL)
        interface_value->ops->destroy(interface_value->context);
    interface_value->platform->deallocate(interface_value->platform->context,
                                          interface_value);
}

rns_status_t rns_interface_start(rns_interface_t *interface_value) {
    rns_status_t status;
    if (interface_value == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (interface_value->started != 0) return RNS_ERROR_INVALID_STATE;
    status = interface_value->ops->start(interface_value->context);
    if (status == RNS_OK) interface_value->started = 1;
    return status;
}

rns_status_t rns_interface_poll(rns_interface_t *interface_value,
                                rns_interface_receive_fn receive,
                                void *receive_context, size_t budget) {
    if (interface_value == NULL || receive == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (interface_value->started == 0) return RNS_ERROR_INVALID_STATE;
    if (budget == 0U) return RNS_OK;
    return interface_value->ops->poll(interface_value->context, receive,
                                      receive_context, budget);
}

rns_status_t rns_interface_send(rns_interface_t *interface_value,
                                const uint8_t *packet, size_t length) {
    if (interface_value == NULL || packet == NULL || length == 0U)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (interface_value->started == 0) return RNS_ERROR_INVALID_STATE;
    return interface_value->ops->send(interface_value->context, packet, length);
}

rns_status_t rns_interface_get_stats(rns_interface_t *interface_value,
                                     rns_interface_stats_t *stats) {
    if (interface_value == NULL || stats == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    memset(stats, 0, sizeof(*stats));
    return interface_value->ops->get_stats(interface_value->context, stats);
}

void rns_interface_stop(rns_interface_t *interface_value) {
    if (interface_value == NULL || interface_value->started == 0) return;
    interface_value->ops->stop(interface_value->context);
    interface_value->started = 0;
}
