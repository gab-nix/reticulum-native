#include "reticulum/lxmf_router.h"

#include "../fixtures/lxmf_delivery_announce_vectors.h"

#include <assert.h>
#include <string.h>

int main(void) {
    for (size_t i = 0u; i < LXMF_PYTHON_ANNOUNCE_FIXTURE_COUNT; ++i) {
        const lxmf_python_announce_fixture *fixture =
            &lxmf_python_announce_fixtures[i];
        lxmf_announce_data_t announce = {0};
        uint8_t encoded[512];
        size_t encoded_len = 0u;

        if (fixture->name_len != 0u)
            memcpy(announce.display_name, fixture->name, fixture->name_len);
        announce.display_name_len = fixture->name_len;
        announce.has_stamp_cost = fixture->stamp_cost >= 0;
        if (announce.has_stamp_cost)
            announce.stamp_cost = (uint8_t)fixture->stamp_cost;
        announce.features = LXMF_FEATURE_COMPRESSION;

        assert(lxmf_announce_encode(&announce, encoded, sizeof encoded,
                                    &encoded_len) == LXMF_OK);
        assert(encoded_len == fixture->wire_len);
        assert(memcmp(encoded, fixture->wire, encoded_len) == 0);

        memset(&announce, 0, sizeof announce);
        assert(lxmf_announce_parse(fixture->wire, fixture->wire_len,
                                   &announce) == LXMF_OK);
        assert(announce.display_name_len == fixture->name_len);
        assert(fixture->name_len == 0u ||
               memcmp(announce.display_name, fixture->name,
                      fixture->name_len) == 0);
        assert(announce.has_stamp_cost == (fixture->stamp_cost >= 0));
        assert(!announce.has_stamp_cost ||
               announce.stamp_cost == (uint8_t)fixture->stamp_cost);
        assert((announce.features & LXMF_FEATURE_COMPRESSION) != 0u);
    }
    return 0;
}
