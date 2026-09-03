#include "reticulum/hosted_form.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const rns_hosted_form_entry_t *find_entry(
    const rns_hosted_form_t *form, const char *key) {
    size_t length = strlen(key);
    for (size_t i = 0U; i < form->count; ++i)
        if (form->entries[i].key_length == length &&
            memcmp(form->entries[i].key, key, length) == 0)
            return &form->entries[i];
    return NULL;
}

static void test_canonical_scalars_and_unknowns(void) {
    /* Canonical MessagePack forms produced by RNS.vendor.umsgpack use string
     * keys and scalar field_/var_ values. Unknown metadata remains valid but
     * is not exposed to a hosted-page callback. */
    static const uint8_t packed[] = {
        0x89,
        0xaa, 'f','i','e','l','d','_','n','a','m','e', 0xa3, 'R','e','i',
        0xa8, 'v','a','r','_','b','l','o','b', 0xc4, 0x02, 0x00, 0xff,
        0xa8, 'f','i','e','l','d','_','o','k', 0xc3,
        0xa9, 'f','i','e','l','d','_','n','e','g', 0xd0, 0xfe,
        0xa9, 'v','a','r','_','c','o','u','n','t', 0xcd, 0x12, 0x34,
        0xaa, 'f','i','e','l','d','_','r','a','t','e', 0xcb,
          0x3f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xab, 'f','i','e','l','d','_','e','m','p','t','y', 0xc0,
        0xa4, 'm','e','t','a', 0x91, 0x81, 0xa1, 'x', 0x01,
        0x01, 0xa4, 's','k','i','p'
    };
    rns_hosted_form_t form;
    assert(rns_hosted_form_decode(packed, sizeof packed, &form) == RNS_OK);
    assert(form.count == 7U);

    const rns_hosted_form_entry_t *entry = find_entry(&form, "field_name");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_STRING);
    assert(entry->bytes == packed + 13U);
    assert(entry->bytes_length == 3U && memcmp(entry->bytes, "Rei", 3U) == 0);
    entry = find_entry(&form, "var_blob");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_BINARY);
    assert(entry->bytes_length == 2U && entry->bytes[0] == 0U &&
           entry->bytes[1] == 0xffU);
    entry = find_entry(&form, "field_ok");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_BOOL &&
           entry->bool_value);
    entry = find_entry(&form, "field_neg");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_SIGNED &&
           entry->signed_value == -2);
    entry = find_entry(&form, "var_count");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_UNSIGNED &&
           entry->unsigned_value == 0x1234U);
    entry = find_entry(&form, "field_rate");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_FLOAT &&
           fabs(entry->float_value - 1.5) < 0.000001);
    entry = find_entry(&form, "field_empty");
    assert(entry != NULL && entry->kind == RNS_HOSTED_FORM_NIL);
    assert(find_entry(&form, "meta") == NULL);
}

static void test_nil_duplicate_and_map_headers(void) {
    rns_hosted_form_t form;
    static const uint8_t nil[] = {0xc0};
    assert(rns_hosted_form_decode(nil, sizeof nil, &form) == RNS_OK);
    assert(form.count == 0U);

    static const uint8_t duplicate[] = {
        0x82, 0xa6, 'v','a','r','_','i','d', 0x01,
              0xa6, 'v','a','r','_','i','d', 0x02
    };
    assert(rns_hosted_form_decode(duplicate, sizeof duplicate, &form) == RNS_OK);
    assert(form.count == 1U && form.entries[0].unsigned_value == 2U);

    static const uint8_t map16[] = {
        0xde, 0x00, 0x01, 0xa6, 'v','a','r','_','i','d', 0x2a
    };
    assert(rns_hosted_form_decode(map16, sizeof map16, &form) == RNS_OK);
    assert(form.count == 1U && form.entries[0].unsigned_value == 42U);

    static const uint8_t map32[] = {
        0xdf, 0x00, 0x00, 0x00, 0x01,
        0xa6, 'v','a','r','_','i','d', 0x2b
    };
    assert(rns_hosted_form_decode(map32, sizeof map32, &form) == RNS_OK);
    assert(form.count == 1U && form.entries[0].unsigned_value == 43U);
}

static void test_malformed_and_bounds(void) {
    rns_hosted_form_t form;
    static const uint8_t valid[] = {
        0x81, 0xa6, 'v','a','r','_','i','d', 0xa2, 'o','k'
    };
    assert(rns_hosted_form_decode(NULL, sizeof valid, &form) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_hosted_form_decode(valid, sizeof valid, NULL) ==
           RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_hosted_form_decode(valid, 0U, &form) ==
           RNS_ERROR_INVALID_ARGUMENT);
    for (size_t length = 1U; length < sizeof valid; ++length)
        assert(rns_hosted_form_decode(valid, length, &form) ==
               RNS_ERROR_PROTOCOL);

    uint8_t trailing[sizeof valid + 1U];
    memcpy(trailing, valid, sizeof valid);
    trailing[sizeof valid] = 0U;
    assert(rns_hosted_form_decode(trailing, sizeof trailing, &form) ==
           RNS_ERROR_PROTOCOL);

    static const uint8_t too_many[] = {0xde, 0x00, 0x41};
    assert(rns_hosted_form_decode(too_many, sizeof too_many, &form) ==
           RNS_ERROR_OVERFLOW);

    uint8_t too_large[RNS_HOSTED_FORM_MAX_ENCODED + 1U] = {0x80};
    assert(rns_hosted_form_decode(too_large, sizeof too_large, &form) ==
           RNS_ERROR_OVERFLOW);

    uint8_t long_key[1U + 2U + 129U + 1U] = {0x81, 0xd9, 129};
    memcpy(long_key + 3U, "field_", 6U);
    memset(long_key + 9U, 'k', 123U);
    long_key[sizeof long_key - 1U] = 0xc0U;
    assert(rns_hosted_form_decode(long_key, sizeof long_key, &form) ==
           RNS_ERROR_OVERFLOW);

    uint8_t long_value[1U + 1U + 6U + 3U + 4097U] = {0x81, 0xa6};
    memcpy(long_value + 2U, "var_id", 6U);
    long_value[8U] = 0xdaU;
    long_value[9U] = 0x10U;
    long_value[10U] = 0x01U;
    memset(long_value + 11U, 'v', 4097U);
    assert(rns_hosted_form_decode(long_value, sizeof long_value, &form) ==
           RNS_ERROR_OVERFLOW);

    static const uint8_t composite_retained[] = {
        0x81, 0xa6, 'v','a','r','_','i','d', 0x91, 0x01
    };
    assert(rns_hosted_form_decode(composite_retained,
                                  sizeof composite_retained, &form) ==
           RNS_ERROR_PROTOCOL);

    static const uint8_t reserved[] = {
        0x81, 0xa4, 'm','e','t','a', 0xc1
    };
    assert(rns_hosted_form_decode(reserved, sizeof reserved, &form) ==
           RNS_ERROR_PROTOCOL);
}

int main(void) {
    test_canonical_scalars_and_unknowns();
    test_nil_duplicate_and_map_headers();
    test_malformed_and_bounds();
    puts("hosted form tests passed");
    return 0;
}
