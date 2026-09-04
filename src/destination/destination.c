#include "reticulum/destination.h"
#include "reticulum/crypto.h"

#include <stdlib.h>
#include <string.h>

static int valid_component(const char *value) { return value && value[0] && strchr(value, '.') == NULL; }

int rns_destination_name_hash(const char *app_name, const char *const *aspects, size_t aspect_count, uint8_t out[10]) {
    uint8_t digest[32]; char *name; size_t length, offset;
    if (!valid_component(app_name) || !out || (aspect_count && !aspects)) return 0;
    length = strlen(app_name);
    for (size_t i = 0; i < aspect_count; ++i) {
        if (!valid_component(aspects[i]) || strlen(aspects[i]) > SIZE_MAX - length - 1) return 0;
        length += 1 + strlen(aspects[i]);
    }
    name = malloc(length + 1); if (!name) return 0;
    offset = strlen(app_name); memcpy(name, app_name, offset);
    for (size_t i = 0; i < aspect_count; ++i) {
        size_t n = strlen(aspects[i]); name[offset++] = '.'; memcpy(name + offset, aspects[i], n); offset += n;
    }
    name[offset] = '\0';
    if (!rns_sha256((const uint8_t *)name, length, digest)) { free(name); return 0; }
    memcpy(out, digest, 10); free(name); return 1;
}

int rns_destination_hash(const rns_identity *identity, const char *app_name, const char *const *aspects,
                         size_t aspect_count, uint8_t out[16]) {
    uint8_t material[26], digest[32]; size_t length = 10;
    if (!out || !rns_destination_name_hash(app_name, aspects, aspect_count, material)) return 0;
    if (identity) { memcpy(material + 10, identity->hash, 16); length = 26; }
    if (!rns_sha256(material, length, digest)) return 0; memcpy(out, digest, 16); return 1;
}
