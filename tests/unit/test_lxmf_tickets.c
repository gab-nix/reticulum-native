#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "reticulum/lxmf_tickets.h"

int main(void) {
    char path[] = "/tmp/lxmf-tickets-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    close(descriptor);
    unlink(path);

    uint8_t peer[16] = {7u};
    lxmf_ticket_store_t *store = NULL;
    assert(lxmf_ticket_store_open(&store, path) == LXMF_OK);
    lxmf_ticket_entry_t issued;
    bool created = false;
    assert(lxmf_ticket_store_issue(store, peer, 1000u, &issued, &created) ==
           LXMF_OK && created);
    assert(issued.expires_at == 1000u + LXMF_TICKET_EXPIRY_SECONDS);
    lxmf_ticket_entry_t reused;
    assert(lxmf_ticket_store_issue(store, peer, 1001u, &reused, &created) ==
           LXMF_OK && !created);
    assert(memcmp(reused.ticket, issued.ticket, sizeof issued.ticket) == 0);

    assert(lxmf_ticket_store_mark_delivered(store, peer, 1100u) == LXMF_OK);
    assert(lxmf_ticket_store_issue(store, peer, 1101u, &reused, &created) ==
           LXMF_ERR_PENDING);
    lxmf_ticket_store_close(store);

    assert(lxmf_ticket_store_open(&store, path) == LXMF_OK);
    lxmf_ticket_entry_t inbound[2];
    size_t inbound_count = 0u;
    assert(lxmf_ticket_store_get_inbound(store, peer, 1200u, inbound, 2u,
                                         &inbound_count) == LXMF_OK);
    assert(inbound_count == 1u &&
           memcmp(inbound[0].ticket, issued.ticket, sizeof issued.ticket) == 0);

    lxmf_ticket_entry_t remote = {5000u, {0}};
    memset(remote.ticket, 0xa5, sizeof remote.ticket);
    assert(lxmf_ticket_store_remember_outbound(store, peer, &remote, 1200u) ==
           LXMF_OK);
    lxmf_ticket_entry_t found;
    assert(lxmf_ticket_store_get_outbound(store, peer, 4999u, &found) ==
           LXMF_OK);
    assert(found.expires_at == remote.expires_at &&
           memcmp(found.ticket, remote.ticket, sizeof remote.ticket) == 0);
    uint8_t message_id[32] = {0x42u};
    uint8_t outbound_stamp[16];
    assert(lxmf_ticket_store_stamp_outbound(store, peer, 1200u, message_id,
                                            outbound_stamp) == LXMF_OK);
    assert(lxmf_ticket_stamp_valid(outbound_stamp, remote.ticket, message_id));
    uint8_t inbound_stamp[16];
    lxmf_ticket_stamp(issued.ticket, message_id, inbound_stamp);
    assert(lxmf_ticket_store_validate_inbound(store, peer, 1200u, message_id,
                                              inbound_stamp) == LXMF_OK);
    inbound_stamp[0] ^= 1u;
    assert(lxmf_ticket_store_validate_inbound(store, peer, 1200u, message_id,
                                              inbound_stamp) ==
           LXMF_ERR_FORMAT);
    assert(lxmf_ticket_store_get_outbound(store, peer, 5000u, &found) ==
           LXMF_ERR_PENDING);

    uint64_t cleanup_at = issued.expires_at + LXMF_TICKET_GRACE_SECONDS + 1u;
    assert(lxmf_ticket_store_cleanup(store, cleanup_at) == LXMF_OK);
    assert(lxmf_ticket_store_count(store) == 1u); /* last-delivery marker */
    lxmf_ticket_store_close(store);

    FILE *corrupt = fopen(path, "r+b");
    assert(corrupt != NULL && fputc('X', corrupt) != EOF && fclose(corrupt) == 0);
    store = NULL;
    assert(lxmf_ticket_store_open(&store, path) == LXMF_ERR_FORMAT);
    assert(store == NULL);
    unlink(path);
    return 0;
}
