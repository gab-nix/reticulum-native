#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "reticulum/hosted_node.h"
#include "reticulum/crypto.h"
#include "reticulum/udp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint16_t reserve_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0U) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}
static void config_udp(rns_config_t *config, uint16_t local, uint16_t remote) {
    rns_config_init(config);
    config->interface_count = 1U;
    rns_config_interface_t *interface = &config->interfaces[0];
    strcpy(interface->name, "hosted-test");
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = interface->enabled = true;
    strcpy(interface->listen_ip, "127.0.0.1");
    strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = local;
    interface->forward_port = remote;
}
static void poll_pair(rns_runtime_t *client, rns_runtime_t *server,
                       rns_hosted_node_t *node) {
    size_t processed;
    assert(rns_runtime_poll(server, 32U, &processed) == RNS_OK);
    assert(rns_runtime_poll(client, 32U, &processed) == RNS_OK);
    rns_hosted_node_poll(node);
}
typedef struct result {
    size_t count;
    size_t length;
    uint8_t bytes[4096];
} result_t;
typedef struct execute_context {
    size_t calls;
    bool saw_identity;
} execute_context_t;

static rns_status_t execute_page(
    rns_hosted_node_t *node,
    const rns_hosted_page_execution_t *execution,
    uint8_t *output, size_t capacity, size_t *length, void *context) {
    (void)node;
    execute_context_t *record = context;
    assert(execution != NULL && execution->form != NULL);
    assert(strcmp(execution->relative_path, "dynamic.mu") == 0);
    assert(execution->source_length == 10U);
    assert(memcmp(execution->source, "#!provider", 10U) == 0);
    assert(execution->link_id != NULL);
    record->saw_identity = execution->remote_identity != NULL;
    const uint8_t *name = NULL;
    size_t name_length = 0U;
    for (size_t i = 0U; i < execution->form->count; ++i) {
        const rns_hosted_form_entry_t *entry = &execution->form->entries[i];
        if (entry->key_length == 10U &&
            memcmp(entry->key, "field_name", 10U) == 0) {
            assert(entry->kind == RNS_HOSTED_FORM_STRING);
            name = entry->bytes;
            name_length = entry->bytes_length;
        }
    }
    static const uint8_t prefix[] = "Generated for ";
    if (name == NULL || sizeof prefix - 1U > capacity ||
        name_length > capacity - (sizeof prefix - 1U))
        return RNS_ERROR_OVERFLOW;
    memcpy(output, prefix, sizeof prefix - 1U);
    memcpy(output + sizeof prefix - 1U, name, name_length);
    *length = sizeof prefix - 1U + name_length;
    record->calls++;
    return RNS_OK;
}
static void response(rns_request_receipt_t *receipt, rns_request_state_t state,
                      rns_status_t status, const uint8_t *bytes, size_t length,
                      void *context) {
    (void)receipt;
    result_t *result = context;
    if (state == RNS_REQUEST_CANCELLED) return;
    assert(state == RNS_REQUEST_COMPLETE && status == RNS_OK);
    assert(length <= sizeof result->bytes);
    memcpy(result->bytes, bytes, length);
    result->count++;
    result->length = length;
}
static void write_hash(FILE *file, const uint8_t hash[16]) {
    for (size_t i = 0U; i < 16U; ++i)
        assert(fprintf(file, "%02x", hash[i]) == 2);
    assert(fputc('\n', file) == '\n');
}
static void run_access(rns_request_access_t access) {
    char root[] = "/tmp/rns-hosted-link-XXXXXX", path[512], sidecar[520],
         dynamic[512];
    assert(mkdtemp(root) != NULL);
    (void)snprintf(path, sizeof path, "%s/index.mu", root);
    (void)snprintf(sidecar, sizeof sidecar, "%s/index.mu.allowed", root);
    (void)snprintf(dynamic, sizeof dynamic, "%s/dynamic.mu", root);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    for (size_t i = 0U; i < 2048U; ++i) assert(fputc('A', file) == 'A');
    assert(fclose(file) == 0);
    file = fopen(dynamic, "wb");
    assert(file != NULL && fwrite("#!provider", 1U, 10U, file) == 10U &&
           fclose(file) == 0 && chmod(dynamic, 0700) == 0);
    uint16_t client_port = reserve_port(), server_port = reserve_port();
    assert(client_port != server_port);
    rns_config_t client_config, server_config;
    config_udp(&client_config, client_port, server_port);
    config_udp(&server_config, server_port, client_port);
    rns_runtime_t *client = NULL, *server = NULL;
    assert(rns_runtime_create(&client, &client_config, NULL) == RNS_OK);
    assert(rns_runtime_create(&server, &server_config, NULL) == RNS_OK);
    rns_identity identity, visitor;
    assert(rns_identity_generate(&identity) && rns_identity_generate(&visitor));
    uint8_t visitor_public[RNS_IDENTITY_PUBLIC_SIZE], visitor_digest[32];
    rns_identity_export_public(&visitor, visitor_public);
    assert(rns_sha256(visitor_public, sizeof visitor_public, visitor_digest));
    execute_context_t execution = {0};
    rns_hosted_node_options_t options = {
        .pages_root = root, .access = access,
        .allow_identity_hashes = visitor_digest, .allow_identity_count = 1U,
        .page_executor = execute_page, .page_executor_context = &execution};
    rns_hosted_node_t *node = NULL;
    assert(rns_hosted_node_create(&node, server, &identity, &options) == RNS_OK);
    assert(rns_hosted_node_publish_page(node, "index.mu") == RNS_OK);
    assert(rns_hosted_node_publish_page(node, "dynamic.mu") == RNS_OK);
    assert(rns_hosted_node_announce(node, (const uint8_t *)"Test node", 9U) == RNS_OK);
    rns_path_entry entry;
    for (size_t i = 0U; i < 1000U && rns_runtime_path_lookup(client,
        rns_hosted_node_destination(node), &entry) != RNS_OK; ++i) poll_pair(client, server, node);
    assert(rns_runtime_path_lookup(client, rns_hosted_node_destination(node), &entry) == RNS_OK);
    rns_runtime_link_t *link = NULL;
    assert(rns_runtime_link_open(client, rns_hosted_node_destination(node),
                                 &identity, NULL, &link) == RNS_OK);
    for (size_t i = 0U; i < 1000U && rns_runtime_link_state(link) != RNS_LINK_ACTIVE; ++i)
        poll_pair(client, server, node);
    assert(rns_runtime_link_state(link) == RNS_LINK_ACTIVE);
    result_t result = {0};
    rns_request_options_t request_options = {
        .timeout_seconds = 5.0, .callback = response, .callback_context = &result};
    rns_request_receipt_t *receipt = NULL;
    assert(rns_runtime_link_request(link, "/page/index.mu", NULL, 0U,
                                    &request_options, &receipt) == RNS_OK);
    for (size_t i = 0U; i < 16U; ++i) poll_pair(client, server, node);
    assert(result.count == 0U);
    rns_request_receipt_cancel(receipt);
    rns_request_receipt_destroy(receipt);
    assert(rns_runtime_link_identify(link, &visitor) == RNS_OK);
    for (size_t i = 0U; i < 4U; ++i) poll_pair(client, server, node);
    assert(rns_runtime_link_request(link, "/page/index.mu", NULL, 0U,
                                    &request_options, &receipt) == RNS_OK);
    for (size_t i = 0U; i < 4000U && result.count == 0U; ++i) poll_pair(client, server, node);
    assert(result.count == 1U && result.length == 2048U);
    for (size_t i = 0U; i < result.length; ++i) assert(result.bytes[i] == 'A');
    rns_request_receipt_destroy(receipt);
    file = fopen(sidecar, "wb");
    assert(file != NULL);
    write_hash(file, visitor_digest);
    assert(fclose(file) == 0);
    assert(rns_runtime_link_request(link, "/page/index.mu", NULL, 0U,
                                    &request_options, &receipt) == RNS_OK);
    for (size_t i = 0U; i < 4000U && result.count == 1U; ++i)
        poll_pair(client, server, node);
    assert(result.count == 2U && result.length == 2048U);
    rns_request_receipt_destroy(receipt);
    uint8_t other_hash[16];
    memset(other_hash, 0x77, sizeof other_hash);
    file = fopen(sidecar, "wb");
    assert(file != NULL);
    write_hash(file, other_hash);
    assert(fclose(file) == 0);
    assert(rns_runtime_link_request(link, "/page/index.mu", NULL, 0U,
                                    &request_options, &receipt) == RNS_OK);
    for (size_t i = 0U; i < 1000U && result.count == 2U; ++i)
        poll_pair(client, server, node);
    static const uint8_t denied[] =
        ">Request Not Allowed\n\nYou are not authorised to carry out the request.\n";
    assert(result.count == 3U && result.length == sizeof denied - 1U);
    assert(memcmp(result.bytes, denied, sizeof denied - 1U) == 0);
    rns_request_receipt_destroy(receipt);
    file = fopen(sidecar, "wb");
    assert(file != NULL && fwrite("not a hash\n", 1U, 11U, file) == 11U &&
           fclose(file) == 0);
    assert(rns_runtime_link_request(link, "/page/index.mu", NULL, 0U,
                                    &request_options, &receipt) == RNS_OK);
    for (size_t i = 0U; i < 16U; ++i) poll_pair(client, server, node);
    assert(result.count == 3U);
    rns_request_receipt_cancel(receipt);
    rns_request_receipt_destroy(receipt);
    assert(unlink(sidecar) == 0);
    file = fopen(path, "wb");
    assert(file != NULL && fwrite("AAA", 1U, 3U, file) == 3U && fclose(file) == 0);
    assert(rns_runtime_link_request(link, "/page/index.mu", NULL, 0U,
                                    &request_options, &receipt) == RNS_OK);
    for (size_t i = 0U; i < 1000U && result.count == 3U; ++i) poll_pair(client, server, node);
    assert(result.count == 4U && result.length == 3U);
    assert(memcmp(result.bytes, "AAA", 3U) == 0);
    rns_request_receipt_destroy(receipt);
    static const uint8_t form[] = {
        0x82U, 0xaaU, 'f','i','e','l','d','_','n','a','m','e',
        0xa3U, 'R','e','i',
        0xaaU, 'v','a','r','_','a','n','c','h','o','r',
        0xa3U, 't','o','p'};
    assert(rns_runtime_link_request(link, "/page/dynamic.mu", form,
                                    sizeof form, &request_options,
                                    &receipt) == RNS_OK);
    for (size_t i = 0U; i < 1000U && result.count == 4U; ++i)
        poll_pair(client, server, node);
    static const uint8_t generated[] = "Generated for Rei";
    assert(result.count == 5U && result.length == sizeof generated - 1U);
    assert(memcmp(result.bytes, generated, sizeof generated - 1U) == 0);
    assert(execution.calls == 1U && execution.saw_identity);
    rns_request_receipt_destroy(receipt);

    /* Retained form variables must be scalar; malformed executable-page
     * inputs fail without invoking the provider or emitting a response. */
    static const uint8_t composite[] = {
        0x81U, 0xa9U, 'f','i','e','l','d','_','b','a','d', 0x91U, 0x01U};
    assert(rns_runtime_link_request(link, "/page/dynamic.mu", composite,
                                    sizeof composite, &request_options,
                                    &receipt) == RNS_OK);
    for (size_t i = 0U; i < 16U; ++i) poll_pair(client, server, node);
    assert(result.count == 5U && execution.calls == 1U);
    rns_request_receipt_cancel(receipt);
    rns_request_receipt_destroy(receipt);
    /* Destroying the service closes owned links before its context disappears. */
    rns_hosted_node_destroy(node);
    rns_runtime_link_destroy(link);
    rns_runtime_destroy(server);
    rns_runtime_destroy(client);
    assert(unlink(path) == 0 && unlink(dynamic) == 0 && rmdir(root) == 0);
}
int main(void) {
    run_access(RNS_REQUEST_ALLOW_IDENTIFIED);
    run_access(RNS_REQUEST_ALLOW_LIST);
    return 0;
}
