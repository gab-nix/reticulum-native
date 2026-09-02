#ifndef RETICULUM_IFAC_H
#define RETICULUM_IFAC_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/identity.h"
#include "reticulum/packet.h"

#define RNS_IFAC_MIN_SIZE 1u
#define RNS_IFAC_MAX_SIZE 64u
#define RNS_IFAC_KEY_SIZE 64u

typedef struct {
    uint8_t key[RNS_IFAC_KEY_SIZE];
    rns_identity identity;
    size_t tag_size;
} rns_ifac;

/* Derives an IFAC key from UTF-8 network name/passphrase byte strings. At
 * least one input must be non-empty. */
int rns_ifac_derive(rns_ifac *ifac, const uint8_t *network_name,
                    size_t network_name_length, const uint8_t *passphrase,
                    size_t passphrase_length, size_t tag_size);

size_t rns_ifac_protected_bound(size_t raw_length, size_t tag_size);
int rns_ifac_protect(const rns_ifac *ifac, const uint8_t *raw,
                     size_t raw_length, uint8_t *out, size_t out_capacity,
                     size_t *out_length);
int rns_ifac_unprotect(const rns_ifac *ifac, const uint8_t *protected_raw,
                       size_t protected_length, uint8_t *out,
                       size_t out_capacity, size_t *out_length);

#endif
