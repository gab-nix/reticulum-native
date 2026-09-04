#ifndef RETICULUM_AUTO_POSIX_H
#define RETICULUM_AUTO_POSIX_H

#include "reticulum/auto.h"
#include "reticulum/config.h"

typedef struct rns_auto_posix rns_auto_posix_t;

rns_status_t rns_auto_posix_create(
    rns_auto_posix_t **endpoint, const rns_config_interface_t *configuration,
    rns_auto_clock_callback_t clock, void *clock_context);
void rns_auto_posix_destroy(rns_auto_posix_t *endpoint);
rns_status_t rns_auto_posix_poll(rns_auto_posix_t *endpoint,
                                 size_t max_packets,
                                 rns_auto_receive_callback_t callback,
                                 void *context, size_t *packets_received);
rns_status_t rns_auto_posix_send(rns_auto_posix_t *endpoint,
                                 const uint8_t *packet, size_t packet_length,
                                 size_t *peers_sent);
bool rns_auto_posix_online(const rns_auto_posix_t *endpoint);
size_t rns_auto_posix_peer_count(const rns_auto_posix_t *endpoint);

#endif
