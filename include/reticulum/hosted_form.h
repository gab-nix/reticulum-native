#ifndef RETICULUM_HOSTED_FORM_H
#define RETICULUM_HOSTED_FORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_HOSTED_FORM_MAX_ENCODED 16384u
#define RNS_HOSTED_FORM_MAX_ENTRIES 64u
#define RNS_HOSTED_FORM_KEY_MAX 128u
#define RNS_HOSTED_FORM_VALUE_MAX 4096u

typedef enum rns_hosted_form_value_kind {
    RNS_HOSTED_FORM_STRING = 0,
    RNS_HOSTED_FORM_BINARY,
    RNS_HOSTED_FORM_BOOL,
    RNS_HOSTED_FORM_SIGNED,
    RNS_HOSTED_FORM_UNSIGNED,
    RNS_HOSTED_FORM_FLOAT,
    RNS_HOSTED_FORM_NIL
} rns_hosted_form_value_kind_t;

typedef struct rns_hosted_form_entry {
    /* Spans alias the immutable encoded input. */
    const uint8_t *key;
    size_t key_length;
    rns_hosted_form_value_kind_t kind;
    const uint8_t *bytes;
    size_t bytes_length;
    int64_t signed_value;
    uint64_t unsigned_value;
    double float_value;
    bool bool_value;
} rns_hosted_form_entry_t;

typedef struct rns_hosted_form {
    rns_hosted_form_entry_t entries[RNS_HOSTED_FORM_MAX_ENTRIES];
    size_t count;
} rns_hosted_form_t;

/* Decodes exactly one bounded MessagePack map. Only string keys beginning
 * `field_` or `var_` are retained. Unknown fields are structurally validated
 * and skipped. Values for retained fields must be scalar. Duplicate keys use
 * the final value, matching Python dict unpacking. A MessagePack nil is an
 * empty form. Input storage must outlive the returned view. */
rns_status_t rns_hosted_form_decode(const uint8_t *encoded, size_t length,
                                    rns_hosted_form_t *form);

#ifdef __cplusplus
}
#endif

#endif
