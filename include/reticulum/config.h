#ifndef RETICULUM_CONFIG_H
#define RETICULUM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_CONFIG_MAX_INTERFACES 16U
#define RNS_CONFIG_NAME_MAX 64U
#define RNS_CONFIG_VALUE_MAX 128U
#define RNS_CONFIG_DIAGNOSTIC_MAX 192U

typedef enum rns_config_interface_type {
    RNS_CONFIG_TCP_CLIENT = 0,
    RNS_CONFIG_TCP_SERVER,
    RNS_CONFIG_UDP,
    RNS_CONFIG_AUTO,
    RNS_CONFIG_KISS,
    RNS_CONFIG_RNODE
} rns_config_interface_type_t;

typedef struct rns_config_interface {
    char name[RNS_CONFIG_NAME_MAX];
    rns_config_interface_type_t type;
    bool type_set;
    bool enabled;
    char target_host[RNS_CONFIG_VALUE_MAX];
    char listen_ip[RNS_CONFIG_VALUE_MAX];
    char forward_ip[RNS_CONFIG_VALUE_MAX];
    uint16_t target_port;
    uint16_t listen_port;
    uint16_t forward_port;
    char device[RNS_CONFIG_VALUE_MAX];
    uint32_t speed;
    uint32_t frequency;
    uint32_t bandwidth;
    int32_t tx_power;
    uint8_t spreading_factor;
    uint8_t coding_rate;
} rns_config_interface_t;

typedef struct rns_config {
    bool enable_transport;
    bool share_instance;
    bool panic_on_interface_error;
    uint16_t instance_control_port;
    uint16_t instance_data_port;
    rns_config_interface_t interfaces[RNS_CONFIG_MAX_INTERFACES];
    size_t interface_count;
} rns_config_t;

typedef struct rns_config_diagnostic {
    size_t line;
    rns_status_t status;
    char message[RNS_CONFIG_DIAGNOSTIC_MAX];
} rns_config_diagnostic_t;

void rns_config_init(rns_config_t *config);
rns_status_t rns_config_parse(const char *text,
                              size_t text_length,
                              rns_config_t *config,
                              rns_config_diagnostic_t *diagnostic);
rns_status_t rns_config_emit(const rns_config_t *config,
                             char *output,
                             size_t output_capacity,
                             size_t *output_length);
const char *rns_config_interface_type_name(rns_config_interface_type_t type);

#ifdef __cplusplus
}
#endif

#endif

