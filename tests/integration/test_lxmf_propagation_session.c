#include "reticulum/lxmf_propagation_session.h"
#include "reticulum/crypto.h"
#include "reticulum/destination.h"
#include "reticulum/udp.h"
#include <assert.h>
#include <string.h>

#define ITEMS 10u
typedef struct {
    rns_runtime_link_t *links[16];
    size_t link_count, delivered, acked, uploads;
    uint8_t messages[ITEMS][128], ids[ITEMS][32];
    bool reject_storage, duplicate_list, wrong_message, deny, empty;
} peer_t;

static uint16_t port(void) {
    rns_udp_endpoint_t *ep = NULL; rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&ep, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(ep, "127.0.0.1", 0) == RNS_OK);
    assert(rns_udp_local_address(ep, &address) == RNS_OK);
    rns_udp_endpoint_destroy(ep); return address.port;
}
static void config(rns_config_t *c, uint16_t listen, uint16_t forward) {
    rns_config_init(c); c->interface_count = 1;
    rns_config_interface_t *i = &c->interfaces[0];
    strcpy(i->name, "test"); i->type = RNS_CONFIG_UDP;
    i->type_set = true; i->enabled = true;
    strcpy(i->listen_ip, "127.0.0.1"); strcpy(i->forward_ip, "127.0.0.1");
    i->listen_port = listen; i->forward_port = forward;
}
static void accepted(rns_runtime_destination_t *d, rns_runtime_link_t *link,
                     void *context) {
    peer_t *p = context; (void)d;
    assert(p->link_count < 16); p->links[p->link_count++] = link;
}
static bool accept_resource(rns_runtime_link_t *link,
    const rns_resource_advertisement_t *advertisement, void *context) {
    (void)link; (void)advertisement; (void)context; return true;
}
static void upload_received(rns_runtime_link_t *link, const uint8_t hash[32],
    rns_status_t status, const uint8_t *data, size_t length, void *context) {
    peer_t *p = context; (void)link; (void)hash;
    assert(status == RNS_OK);
    lxmf_pn_upload_t upload;
    assert(lxmf_pn_upload_decode(data, length, &upload) == LXMF_OK);
    assert(upload.count == 1 && upload.messages[0].len == 128);
    p->uploads++;
}
static rns_status_t serve(rns_runtime_request_handler_t *handler,
    rns_runtime_link_t *link, const rns_request_view_t *request,
    const rns_identity *identity, uint8_t *output, size_t capacity,
    size_t *length, void *context) {
    peer_t *p = context; (void)handler; (void)link;
    assert(identity != NULL);
    lxmf_pn_get_request_t get;
    assert(lxmf_pn_get_request_decode(request->data_msgpack,
        request->data_msgpack_length, &get) == LXMF_OK);
    lxmf_pn_get_response_t result = {0};
    bool listing = get.wants_null && get.haves_null;
    if (p->deny) { result.kind = LXMF_PN_RESPONSE_ERROR;
        result.error = LXMF_PN_ERROR_NO_ACCESS;
    } else if (listing) {
        result.count = p->empty ? 0 : ITEMS;
        for (size_t i = 0; i < result.count; ++i)
            result.items[i] = (lxmf_slice_t){p->ids[p->duplicate_list ? 0 : i], 32};
    } else if (get.wants_null) {
        assert(!get.haves_null && get.haves_count > 0);
        p->acked += get.haves_count;
    } else {
        assert(get.wants_count <= 8 && get.has_limit);
        for (size_t i = 0; i < get.wants_count; ++i) {
            for (size_t j = 0; j < ITEMS; ++j) {
                if (memcmp(get.wants[i].data, p->ids[j], 32) == 0) {
                    result.items[result.count++] = (lxmf_slice_t){
                        p->messages[p->wrong_message ? ITEMS - 1 : j], 128};
                    break;
                }
            }
        }
    }
    return lxmf_pn_get_response_encode(&result, listing, output, capacity,
        length) == LXMF_OK ? RNS_OK : RNS_ERROR_OVERFLOW;
}
static bool received(const uint8_t id[32], const uint8_t *message,
                     size_t length, void *context) {
    peer_t *p = context; uint8_t actual[32];
    assert(length == 128 && rns_sha256(message, length, actual));
    assert(memcmp(actual, id, 32) == 0); p->delivered++;
    return !p->reject_storage;
}
static void pump(rns_runtime_t *client, rns_runtime_t *server,
                 lxmf_pn_session_t *session) {
    for (size_t i = 0; i < 10000; ++i) {
        size_t processed;
        assert(rns_runtime_poll(client, 32, &processed) == RNS_OK);
        assert(rns_runtime_poll(server, 32, &processed) == RNS_OK);
        (void)lxmf_pn_session_poll(session, 1);
        if (lxmf_pn_session_progress(session)->state >= LXMF_PN_COMPLETE)
            return;
    }
    assert(0 && "session did not complete bounded loopback exchange");
}
int main(void) {
    uint16_t a = port(), b = port(); while (a == b) b = port();
    rns_config_t ca, cb; config(&ca, a, b); config(&cb, b, a);
    rns_runtime_t *client = NULL, *server = NULL;
    assert(rns_runtime_create(&client, &ca, NULL) == RNS_OK);
    assert(rns_runtime_create(&server, &cb, NULL) == RNS_OK);
    rns_identity local, node;
    assert(rns_identity_generate(&local) && rns_identity_generate(&node));
    uint8_t destination[16]; const char *aspects[] = {"propagation"};
    assert(rns_destination_hash(&node, "lxmf", aspects, 1, destination));
    peer_t peer = {0};
    for (size_t i = 0; i < ITEMS; ++i) {
        memset(peer.messages[i], (int)i, 128);
        assert(rns_sha256(peer.messages[i], 128, peer.ids[i]));
    }
    rns_runtime_link_options_t links = {0};
    links.resource_accept_callback = accept_resource;
    links.resource_receive_callback = upload_received;
    links.callback_context = &peer; links.max_incoming_resource_size = 4096;
    rns_runtime_destination_t *registration = NULL;
    assert(rns_runtime_register_link_destination(server, destination, &node,
        &links, accepted, &peer, &registration) == RNS_OK);
    rns_runtime_request_handler_options_t handler = {0};
    handler.access = RNS_REQUEST_ALLOW_IDENTIFIED;
    handler.max_response_size = 4096; handler.callback = serve;
    handler.callback_context = &peer;
    rns_runtime_request_handler_t *get = NULL;
    assert(rns_runtime_destination_register_request_handler(registration,
        "/get", &handler, &get) == RNS_OK);
    assert(rns_runtime_announce(server, &node, "lxmf", aspects, 1, NULL, 0)
        == RNS_OK);
    size_t processed;
    assert(rns_runtime_poll(client, 32, &processed) == RNS_OK);
    lxmf_pn_session_options_t options = {0};
    options.runtime = client; options.local_identity = &local;
    options.node_identity = &node; options.message_callback = received;
    options.callback_context = &peer;
    memcpy(options.node_destination, destination, 16);
    lxmf_pn_session_t *session = NULL;
    options.node_destination[0] ^= 1;
    assert(lxmf_pn_session_create(&session, &options) == RNS_ERROR_INVALID_ARGUMENT);
    options.node_destination[0] ^= 1;
    assert(lxmf_pn_session_create(&session, &options) == RNS_OK);
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    assert(lxmf_pn_session_sync(session, 0) == RNS_ERROR_INVALID_STATE);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_COMPLETE);
    assert(peer.delivered == ITEMS && peer.acked == ITEMS);
    assert(lxmf_pn_session_progress(session)->acknowledged == ITEMS);
    peer.reject_storage = true;
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    pump(client, server, session);
    assert(peer.delivered == ITEMS * 2 && peer.acked == ITEMS);
    assert(lxmf_pn_session_progress(session)->received == 0);
    peer.reject_storage = false; peer.duplicate_list = true;
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_FAILED);
    assert(peer.delivered == ITEMS * 2 && peer.acked == ITEMS);
    peer.duplicate_list = false; peer.wrong_message = true;
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_FAILED);
    assert(peer.delivered == ITEMS * 2 && peer.acked == ITEMS);
    peer.wrong_message = false; peer.deny = true;
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->remote_error == LXMF_PN_ERROR_NO_ACCESS);
    peer.deny = false;
    lxmf_pn_upload_t upload = {0}; upload.timebase = 10; upload.count = 1;
    upload.messages[0] = (lxmf_slice_t){peer.messages[0], 128};
    assert(lxmf_pn_session_upload(session, &upload, 0) == RNS_OK);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_COMPLETE);
    assert(peer.uploads == 1);
    peer.empty = true;
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_COMPLETE);
    assert(lxmf_pn_session_progress(session)->available == 0);
    peer.empty = false;
    lxmf_pn_session_destroy(session); session = NULL;
    options.retain_on_node = true;
    assert(lxmf_pn_session_create(&session, &options) == RNS_OK);
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    pump(client, server, session);
    assert(lxmf_pn_session_progress(session)->received == ITEMS);
    assert(lxmf_pn_session_progress(session)->acknowledged == 0);
    assert(peer.acked == ITEMS);
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    assert(lxmf_pn_session_poll(session, 1) == RNS_OK);
    lxmf_pn_session_cancel(session);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_CANCELLED);
    assert(lxmf_pn_session_sync(session, 0) == RNS_OK);
    assert(lxmf_pn_session_poll(session, 61) == RNS_ERROR_TIMEOUT);
    assert(lxmf_pn_session_progress(session)->state == LXMF_PN_FAILED);
    lxmf_pn_session_destroy(session);
    for (size_t i = 0; i < peer.link_count; ++i)
        rns_runtime_link_destroy(peer.links[i]);
    rns_runtime_destination_destroy(registration);
    rns_runtime_destroy(client); rns_runtime_destroy(server);
    return 0;
}
