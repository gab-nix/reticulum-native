#define _POSIX_C_SOURCE 200809L
#include "reticulum/hosted_node.h"
#include "reticulum/hal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static rns_status_t execute(rns_hosted_node_t *node,
    const rns_hosted_page_execution_t *request, uint8_t *output,
    size_t capacity, size_t *length, void *context) {
    (void)node; (void)context;
    if (strcmp(request->relative_path, "form.mu") != 0 || request->form == NULL)
        return RNS_ERROR_PROTOCOL;
    const rns_hosted_form_entry_t *name = NULL, *action = NULL;
    for (size_t i = 0; i < request->form->count; ++i) {
        const rns_hosted_form_entry_t *entry = &request->form->entries[i];
        if (entry->kind != RNS_HOSTED_FORM_STRING) return RNS_ERROR_PROTOCOL;
        if (entry->key_length == 10u && memcmp(entry->key, "field_name", 10u) == 0) name = entry;
        if (entry->key_length == 10u && memcmp(entry->key, "var_action", 10u) == 0) action = entry;
    }
    if (name == NULL || action == NULL || name->bytes_length > 32u || action->bytes_length > 32u)
        return RNS_ERROR_PROTOCOL;
    size_t required = name->bytes_length + action->bytes_length + 1u;
    if (required > capacity) return RNS_ERROR_OVERFLOW;
    memcpy(output, name->bytes, name->bytes_length);
    output[name->bytes_length] = ':';
    memcpy(output + name->bytes_length + 1u, action->bytes, action->bytes_length);
    *length = required; return RNS_OK;
}

static bool port_number(const char *text, uint16_t *port) {
    char *end; errno = 0; unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != 0 || value == 0 || value > 65535u) return false;
    *port = (uint16_t)value; return true;
}

int main(int argc, char **argv) {
    uint16_t local = 0, remote = 0;
    bool tcp = argc == 4 && strcmp(argv[1], "--tcp") == 0;
    if (argc != 4 || !port_number(argv[2], &remote) ||
        (!tcp && !port_number(argv[1], &local))) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    rns_config_t config; rns_config_init(&config); config.interface_count = 1;
    rns_config_interface_t *interface = &config.interfaces[0];
    strcpy(interface->name, "synthetic hosted acceptance");
    interface->type = tcp ? RNS_CONFIG_TCP_CLIENT : RNS_CONFIG_UDP;
    interface->type_set = interface->enabled = true;
    if (tcp) { strcpy(interface->target_host, "127.0.0.1"); interface->target_port = remote; }
    else {
        strcpy(interface->listen_ip, "127.0.0.1"); strcpy(interface->forward_ip, "127.0.0.1");
        interface->listen_port = local; interface->forward_port = remote;
    }
    rns_identity identity; rns_runtime_t *runtime = NULL; rns_hosted_node_t *node = NULL;
    if (!rns_identity_generate(&identity) || rns_runtime_create(&runtime, &config, NULL) != RNS_OK) return 3;
    rns_hosted_node_options_t options = {.pages_root = argv[3],
        .max_content_size = 128u * 1024u, .access = RNS_REQUEST_ALLOW_ALL,
        .page_executor = execute};
    int result = 1;
    if (rns_hosted_node_create(&node, runtime, &identity, &options) != RNS_OK) goto done;
    const char *pages[] = {"index.mu", "large.mu", "form.mu", "restricted.mu"};
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; ++i)
        if (rns_hosted_node_publish_page(node, pages[i]) != RNS_OK) goto done;
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) goto done;
    printf("{\"event\":\"ready\",\"destination\":\"");
    const uint8_t *destination = rns_hosted_node_destination(node);
    for (size_t i = 0; i < 16; ++i) printf("%02x", destination[i]);
    puts("\"}");
    uint64_t start = 0, now = 0, last_announce = 0;
    if (rns_hal_monotonic_ms(&start) != RNS_OK) goto done;
    while (rns_hal_monotonic_ms(&now) == RNS_OK && now - start < 120000u) {
        size_t processed;
        if (rns_runtime_poll(runtime, 32u, &processed) != RNS_OK) break;
        rns_hosted_node_poll(node);
        if (last_announce == 0 || now - last_announce >= 1000u) {
            (void)rns_hosted_node_announce(node, (const uint8_t *)"Synthetic host", 14u);
            last_announce = now;
        }
        char command; ssize_t count = read(STDIN_FILENO, &command, 1);
        if (count == 1 && command == 'q') { result = 0; break; }
        if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
        (void)rns_hal_sleep_ms(2u);
    }
done:
    rns_hosted_node_destroy(node); rns_runtime_destroy(runtime);
    rns_hal_secure_zero(&identity, sizeof identity);
    return result;
}
