#ifndef RETICULUM_NODE_REGISTRY_H
#define RETICULUM_NODE_REGISTRY_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "reticulum/node.h"

#define RNS_NODE_REGISTRY_MAX 256u
#define RNS_NODE_APP_DATA_MAX 317u
typedef enum { RNS_NODE_KIND_OTHER = 0, RNS_NODE_KIND_NOMAD, RNS_NODE_KIND_LXMF } rns_node_kind;
typedef struct { uint8_t destination[16], next_hop[16], public_key[64], message_destination[16]; uint8_t app_data[RNS_NODE_APP_DATA_MAX]; size_t app_data_length; uint64_t announce_timebase; uint8_t hops; uint64_t interface_id; int32_t gravity; double seen_at, expires_at; bool reachable, propagation, has_ratchet, has_message_destination; rns_node_kind kind; char name[64]; } rns_node_record;
typedef struct { rns_node_record records[RNS_NODE_REGISTRY_MAX]; size_t count; double lifetime; } rns_node_registry;
void rns_node_registry_init(rns_node_registry *r, double lifetime);
int rns_node_registry_upsert(rns_node_registry *r, const rns_node_record *record);
size_t rns_node_registry_expire(rns_node_registry *r, double now);
const rns_node_record *rns_node_registry_get(const rns_node_registry *r, const uint8_t destination[16]);
size_t rns_node_registry_list(const rns_node_registry *r, rns_node_record *out, size_t capacity, const char *filter);
int rns_node_registry_consider_announce(rns_node_registry *r, const rns_node_result *announce);
size_t rns_node_registry_sorted(const rns_node_registry *r, rns_node_record *out, size_t capacity);
int rns_node_registry_save(const rns_node_registry *r, const char *path);
int rns_node_registry_load(rns_node_registry *r, const char *path, double lifetime);
#endif
