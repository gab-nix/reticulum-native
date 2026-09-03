#include "reticulum/destination.h"
#include "reticulum/lxmf.h"

#include "../fixtures/lxmf_message_vectors.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    const lxmf_python_message_fixture *fixture;
    bool called;
} signer_context;

static lxmf_status_t fixture_signer(void *context, const uint8_t *data,
                                    size_t data_len, uint8_t signature[64]) {
    signer_context *signer = context;
    uint8_t digest[32];
    assert(signer != NULL && data != NULL && signature != NULL);
    lxmf_sha256(data, data_len, digest);
    assert(memcmp(digest, signer->fixture->preimage_sha256, sizeof digest) == 0);
    memcpy(signature, signer->fixture->signature, 64u);
    signer->called = true;
    return LXMF_OK;
}

static const rns_identity *resolve_fixture_identity(void *context,
                                                     const uint8_t source[16]) {
    const lxmf_python_message_fixture *fixture = context;
    static rns_identity identity;
    if (memcmp(source, fixture->source, 16u) != 0 ||
        !rns_identity_from_public(&identity, fixture->source_public))
        return NULL;
    return &identity;
}

int main(void) {
    for (size_t i = 0u; i < LXMF_PYTHON_MESSAGE_FIXTURE_COUNT; ++i) {
        const lxmf_python_message_fixture *fixture =
            &lxmf_python_message_fixtures[i];
        lxmf_message_t outbound = {0};
        uint8_t packed[1024];
        size_t packed_len = 0u;
        signer_context signer = {fixture, false};

        memcpy(outbound.destination, fixture->destination, 16u);
        memcpy(outbound.source, fixture->source, 16u);
        outbound.timestamp = fixture->timestamp;
        outbound.title = (lxmf_slice_t){fixture->title, fixture->title_len};
        outbound.content = (lxmf_slice_t){fixture->content, fixture->content_len};
        outbound.fields_msgpack =
            (lxmf_slice_t){fixture->fields, fixture->fields_len};
        assert(lxmf_pack(&outbound, fixture_signer, &signer, packed,
                         sizeof packed, &packed_len) == LXMF_OK);
        assert(signer.called && packed_len == fixture->packed_len);
        assert(memcmp(packed, fixture->packed, packed_len) == 0);
        assert(fixture->opportunistic_payload_len == packed_len - 16u);
        assert(memcmp(packed + 16u, fixture->opportunistic_payload,
                      fixture->opportunistic_payload_len) == 0);

        lxmf_identity_verifier_context_t verifier = {
            resolve_fixture_identity, (void *)fixture
        };
        lxmf_message_t inbound;
        assert(lxmf_unpack(fixture->packed, fixture->packed_len,
                           lxmf_identity_verifier, &verifier,
                           &inbound) == LXMF_OK);
        assert(memcmp(inbound.message_id, fixture->message_id, 32u) == 0);
        assert(inbound.timestamp == fixture->timestamp);
        assert(inbound.title.len == fixture->title_len &&
               (inbound.title.len == 0u ||
                memcmp(inbound.title.data, fixture->title, inbound.title.len) == 0));
        assert(inbound.content.len == fixture->content_len &&
               (inbound.content.len == 0u ||
                memcmp(inbound.content.data, fixture->content,
                       inbound.content.len) == 0));
        assert(inbound.fields_msgpack.len == fixture->fields_len &&
               memcmp(inbound.fields_msgpack.data, fixture->fields,
                      inbound.fields_msgpack.len) == 0);
    }
    return 0;
}
