#include "tui_host.h"
#include <string.h>

rns_status_t tui_host_start(tui_host_t *host, rns_runtime_t *runtime,
    const rns_identity *identity, const tui_settings_t *settings) {
    if (host == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (host->node != NULL) return RNS_ERROR_INVALID_STATE;
    if (runtime == NULL || identity == NULL || !tui_settings_valid(settings) ||
        settings->host_pages_root[0] == 0 || settings->host_pages[0] == 0)
        return host->error = RNS_ERROR_INVALID_ARGUMENT;
    rns_hosted_node_options_t options = {.pages_root = settings->host_pages_root,
        /* Reserve the request envelope and MessagePack binary header. */
        .max_content_size = RNS_RESOURCE_MAX_SIZE - 19u - 5u,
        .access = settings->host_identified_only ? RNS_REQUEST_ALLOW_IDENTIFIED : RNS_REQUEST_ALLOW_ALL};
    rns_hosted_node_t *node = NULL;
    rns_status_t status = rns_hosted_node_create(&node, runtime, identity, &options);
    const char *page = settings->host_pages;
    while (status == RNS_OK && *page != 0) {
        const char *separator = strchr(page, ';');
        size_t length = separator != NULL ? (size_t)(separator - page) : strlen(page);
        char relative[RNS_REQUEST_PATH_MAX + 1u];
        if (length == 0 || length >= sizeof relative) { status = RNS_ERROR_INVALID_ARGUMENT; break; }
        memcpy(relative, page, length); relative[length] = 0;
        status = rns_hosted_node_publish_page(node, relative);
        if (separator == NULL) break;
        page = separator + 1;
        if (*page == 0) status = RNS_ERROR_INVALID_ARGUMENT;
    }
    if (status != RNS_OK) rns_hosted_node_destroy(node);
    else { host->node = node; host->next_announce_ms = 0; host->announces = 0; }
    host->error = status; return status;
}

void tui_host_stop(tui_host_t *host) {
    if (host == NULL) return;
    rns_hosted_node_destroy(host->node); host->node = NULL;
    host->next_announce_ms = 0;
}

rns_status_t tui_host_announce(tui_host_t *host, const tui_settings_t *settings, uint64_t now) {
    if (host == NULL || host->node == NULL || !tui_settings_valid(settings)) return RNS_ERROR_INVALID_STATE;
    host->error = rns_hosted_node_announce(host->node,
        (const uint8_t *)settings->display_name, settings->display_name_len);
    uint64_t delay = host->error == RNS_OK ? tui_settings_interval_ms(settings) : 30000u;
    host->next_announce_ms = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
    if (host->error == RNS_OK) host->announces++;
    return host->error;
}

void tui_host_poll(tui_host_t *host, const tui_settings_t *settings, uint64_t now) {
    if (host == NULL || host->node == NULL) return;
    rns_hosted_node_poll(host->node);
    if (host->next_announce_ms == 0 || now >= host->next_announce_ms)
        (void)tui_host_announce(host, settings, now);
}
