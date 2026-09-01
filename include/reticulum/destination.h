#ifndef RETICULUM_DESTINATION_H
#define RETICULUM_DESTINATION_H

#include <stddef.h>
#include <stdint.h>
#include "reticulum/identity.h"

#define RNS_NAME_HASH_SIZE 10u

int rns_destination_name_hash(const char *app_name, const char *const *aspects, size_t aspect_count,
                              uint8_t out[10]);
int rns_destination_hash(const rns_identity *identity, const char *app_name,
                         const char *const *aspects, size_t aspect_count, uint8_t out[16]);

#endif
