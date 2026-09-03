#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "reticulum/ratchet_store.h"

int main(void) {
    char path[] = "/tmp/rns-ratchets-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    close(descriptor);
    unlink(path);

    rns_identity identity;
    assert(rns_identity_generate(&identity));
    rns_ratchet_store_t *store = NULL;
    assert(rns_ratchet_store_open(&store, path, &identity, 2u, 30u) ==
           RNS_OK);
    assert(rns_ratchet_store_count(store) == 0u);

    uint8_t private_one[32], public_one[32], id_one[16];
    bool rotated = false;
    assert(rns_ratchet_store_current(store, 100u, private_one, public_one,
                                     id_one, &rotated) == RNS_OK);
    assert(rotated && rns_ratchet_store_count(store) == 1u);

    uint8_t private_same[32], public_same[32], id_same[16];
    assert(rns_ratchet_store_current(store, 130u, private_same, public_same,
                                     id_same, &rotated) == RNS_OK);
    assert(!rotated && memcmp(private_one, private_same, 32u) == 0 &&
           memcmp(public_one, public_same, 32u) == 0 &&
           memcmp(id_one, id_same, 16u) == 0);

    uint8_t private_two[32], public_two[32], id_two[16];
    assert(rns_ratchet_store_current(store, 131u, private_two, public_two,
                                     id_two, &rotated) == RNS_OK);
    assert(rotated && memcmp(private_one, private_two, 32u) != 0 &&
           rns_ratchet_store_count(store) == 2u);

    uint8_t copied[64];
    size_t copied_count = 0u;
    assert(rns_ratchet_store_copy_private(store, copied, 1u, &copied_count) ==
           RNS_ERROR_OVERFLOW && copied_count == 2u);
    assert(rns_ratchet_store_copy_private(store, copied, 2u, &copied_count) ==
           RNS_OK && copied_count == 2u);
    assert(memcmp(copied, private_two, 32u) == 0 &&
           memcmp(copied + 32u, private_one, 32u) == 0);
    rns_ratchet_store_close(store);

    /* The signed Reticulum-compatible file survives restart. Reload sets the
     * rotation clock to zero, as upstream does, so inspect before current(). */
    assert(rns_ratchet_store_open(&store, path, &identity, 2u, 30u) ==
           RNS_OK);
    assert(rns_ratchet_store_copy_private(store, copied, 2u, &copied_count) ==
           RNS_OK && copied_count == 2u &&
           memcmp(copied, private_two, 32u) == 0);
    rns_ratchet_store_close(store);

    rns_identity other;
    assert(rns_identity_generate(&other));
    assert(rns_ratchet_store_open(&store, path, &other, 2u, 30u) ==
           RNS_ERROR_CRYPTO && store == NULL);

    FILE *file = fopen(path, "r+b");
    assert(file != NULL && fseek(file, -1L, SEEK_END) == 0);
    int value = fgetc(file);
    assert(value != EOF && fseek(file, -1L, SEEK_END) == 0 &&
           fputc(value ^ 1, file) != EOF && fclose(file) == 0);
    assert(rns_ratchet_store_open(&store, path, &identity, 2u, 30u) ==
           RNS_ERROR_CRYPTO && store == NULL);
    unlink(path);
    return 0;
}
