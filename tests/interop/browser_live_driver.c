#define _POSIX_C_SOURCE 200809L
#include "reticulum/browser.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool known;
    rns_identity identity;
    uint8_t destination[16];
} peer_t;

static void announced(rns_runtime_t *runtime, const rns_node_result *event,
                      void *context) {
    (void)runtime;
    peer_t *peer = context;
    uint8_t hash[16];
    const char *aspects[] = {"node"};
    if (!event->has_verified_announce ||
        !rns_destination_hash(&event->announce_identity, "nomadnetwork",
                              aspects, 1U, hash) ||
        memcmp(hash, event->destination_hash, sizeof hash) != 0) return;
    peer->identity = event->announce_identity;
    memcpy(peer->destination, hash, sizeof hash);
    peer->known = true;
}

static bool valid_page(const rns_micron_page *page, size_t index) {
    if (page == NULL || page->truncated || page->unsupported) return false;
    if (index == 0U)
        return page->span_count == 1U &&
            strcmp(rns_micron_span_text(page, &page->spans[0]), "Small page") == 0;
    if (page->span_count != 100U) return false;
    uint32_t seed = 0x13579bdfU;
    for (size_t line = 0U; line < 100U; ++line) {
        const char *text = rns_micron_span_text(page, &page->spans[line]);
        if (strlen(text) != 64U) return false;
        for (size_t col = 0U; col < 64U; ++col) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            if (text[col] != (char)('A' + seed % 26U)) return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    bool tcp = strcmp(argv[1], "tcp") == 0;
    if (!tcp && strcmp(argv[1], "udp") != 0) return 2;
    char *end;
    unsigned long local = strtoul(argv[2], &end, 10);
    if (*end || local > 65535U) return 2;
    unsigned long remote = strtoul(argv[3], &end, 10);
    if (*end || remote == 0U || remote > 65535U) return 2;
    rns_config_t config;
    rns_config_init(&config);
    config.interface_count = 1U;
    rns_config_interface_t *interface = &config.interfaces[0];
    strcpy(interface->name, "python-page-test");
    interface->type = tcp ? RNS_CONFIG_TCP_CLIENT : RNS_CONFIG_UDP;
    interface->type_set = interface->enabled = true;
    strcpy(interface->listen_ip, "127.0.0.1");
    strcpy(interface->forward_ip, "127.0.0.1");
    strcpy(interface->target_host, "127.0.0.1");
    interface->listen_port = (uint16_t)local;
    interface->forward_port = interface->target_port = (uint16_t)remote;
    peer_t peer = {0};
    rns_runtime_options_t options = {.announce_callback = announced,
                                      .callback_context = &peer};
    rns_runtime_t *runtime = NULL;
    rns_browser_t *browser = NULL;
    if (rns_runtime_create(&runtime, &config, &options) != RNS_OK) return 3;
    if (rns_browser_create(&browser, runtime, NULL) != RNS_OK) {
        rns_runtime_destroy(runtime);
        return 4;
    }
    uint64_t start, now;
    size_t completed = 0U;
    bool opened = false, failed = false;
    if (rns_hal_monotonic_ms(&start) != RNS_OK) failed = true;
    while (!failed && completed < 2U) {
        if (rns_hal_monotonic_ms(&now) != RNS_OK || now - start > 60000U) break;
        size_t processed;
        if (rns_runtime_poll(runtime, 64U, &processed) != RNS_OK) { failed = true; break; }
        if (peer.known && !opened) {
            char url[100];
            for (size_t i = 0U; i < 16U; ++i)
                (void)snprintf(url + 2U*i, sizeof url - 2U*i, "%02x", peer.destination[i]);
            (void)snprintf(url + 32U, sizeof url - 32U, ":/page/%s.mu",
                           completed == 0U ? "index" : "large");
            if (rns_browser_open(browser, url, &peer.identity, NULL, 0U) != RNS_OK) {
                failed = true; break;
            }
            opened = true;
        }
        if (opened) {
            (void)rns_browser_poll(browser);
            if (rns_browser_state(browser) == RNS_BROWSER_FAILED) { failed = true; break; }
            if (rns_browser_state(browser) == RNS_BROWSER_COMPLETE) {
                if (!valid_page(rns_browser_page(browser), completed)) { failed = true; break; }
                ++completed;
                opened = false;
            }
        }
        (void)rns_hal_sleep_ms(2U);
    }
    printf("{\"pages\":%zu,\"error\":%d,\"ok\":%s}\n", completed,
           (int)rns_browser_error(browser), !failed && completed == 2U ? "true" : "false");
    rns_browser_destroy(browser);
    rns_runtime_destroy(runtime);
    return !failed && completed == 2U ? 0 : 1;
}
