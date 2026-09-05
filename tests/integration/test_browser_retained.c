#define _POSIX_C_SOURCE 200809L
#include "reticulum/browser.h"
#include "reticulum/hosted_node.h"
#include "reticulum/hal.h"
#include "reticulum/udp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static double clock_now(void *context) { return *(double *)context; }
static uint16_t reserve_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0U) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}
static void configure(rns_config_t *config, uint16_t local, uint16_t remote) {
    rns_config_init(config);
    config->interface_count = 1U;
    rns_config_interface_t *interface = &config->interfaces[0];
    strcpy(interface->name, "retained-page-test");
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = interface->enabled = true;
    strcpy(interface->listen_ip, "127.0.0.1");
    strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = local;
    interface->forward_port = remote;
}
static void write_page(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}
static void poll_until(rns_runtime_t *client, rns_runtime_t *server,
                       rns_hosted_node_t *node, rns_browser_t *browser,
                       rns_browser_state_t target) {
    for (size_t i = 0U; i < 2000U && rns_browser_state(browser) != target; ++i) {
        size_t processed;
        assert(rns_runtime_poll(server, 32U, &processed) == RNS_OK);
        assert(rns_runtime_poll(client, 32U, &processed) == RNS_OK);
        rns_hosted_node_poll(node);
        (void)rns_browser_poll(browser);
        assert(rns_hal_sleep_ms(1U) == RNS_OK);
    }
    assert(rns_browser_state(browser) == target);
}
int main(void) {
    char root[] = "/tmp/rns-browser-retained-XXXXXX", file[256], url[128];
    assert(mkdtemp(root) != NULL);
    (void)snprintf(file, sizeof file, "%s/index.mu", root);
    write_page(file, "Original page");
    uint16_t first = reserve_port(), second = reserve_port();
    while (second == first) second = reserve_port();
    rns_config_t a, b;
    configure(&a, first, second); configure(&b, second, first);
    rns_runtime_t *client = NULL, *server = NULL;
    assert(rns_runtime_create(&client, &a, NULL) == RNS_OK);
    assert(rns_runtime_create(&server, &b, NULL) == RNS_OK);
    rns_identity identity;
    assert(rns_identity_generate(&identity));
    rns_hosted_node_options_t hosting = {.pages_root = root, .access = RNS_REQUEST_ALLOW_ALL};
    rns_hosted_node_t *node = NULL;
    assert(rns_hosted_node_create(&node, server, &identity, &hosting) == RNS_OK);
    assert(rns_hosted_node_publish_page(node, "index.mu") == RNS_OK);
    assert(rns_hosted_node_announce(node, NULL, 0U) == RNS_OK);
    const uint8_t *destination = rns_hosted_node_destination(node);
    for (size_t i = 0U; i < 16U; ++i)
        (void)snprintf(url + 2U*i, sizeof url - 2U*i, "%02x", destination[i]);
    strcpy(url + 32U, ":/page/index.mu");
    double now = 100.0;
    rns_browser_options_t options = {.clock = clock_now, .clock_context = &now,
                                     .path_timeout_seconds = 1.0};
    rns_browser_t *browser = NULL;
    assert(rns_browser_create(&browser, client, &options) == RNS_OK);
    assert(rns_browser_page(browser) == NULL && rns_browser_page_url(browser) == NULL);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    const rns_micron_page *original = rns_browser_page(browser);
    assert(original != NULL && original->span_count == 1U);
    assert(strcmp(rns_micron_span_text(original, &original->spans[0]), "Original page") == 0);
    write_page(file, "\xff");
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(rns_browser_state(browser) == RNS_BROWSER_COMPLETE);
    assert(rns_browser_loaded_from_cache(browser));
    original = rns_browser_page(browser);
    assert(strcmp(rns_micron_span_text(original, &original->spans[0]), "Original page") == 0);
    rns_browser_cache_clear(browser);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(rns_browser_page(browser) == original);
    poll_until(client, server, node, browser, RNS_BROWSER_FAILED);
    assert(rns_browser_error(browser) == RNS_ERROR_PROTOCOL);
    assert(rns_browser_page(browser) == original);
    assert(strcmp(rns_browser_page_url(browser), url) == 0);
    const char *missing = "00000000000000000000000000000000:/page/missing.mu";
    assert(rns_browser_open(browser, missing, &identity, NULL, 0U) == RNS_OK);
    rns_browser_cancel(browser);
    assert(rns_browser_page(browser) == original);
    assert(rns_browser_state(browser) == RNS_BROWSER_CANCELLED);
    assert(rns_browser_open(browser, missing, &identity, NULL, 0U) == RNS_OK);
    now += 2.0;
    assert(rns_browser_poll(browser) == RNS_ERROR_TIMEOUT);
    assert(rns_browser_page(browser) == original);
    assert(strcmp(rns_browser_url(browser), missing) == 0);
    assert(strcmp(rns_browser_page_url(browser), url) == 0);
    write_page(file, "Replacement page");
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    const rns_micron_page *replacement = rns_browser_page(browser);
    assert(replacement != NULL && replacement->span_count == 1U);
    assert(strcmp(rns_micron_span_text(replacement, &replacement->spans[0]), "Replacement page") == 0);
    /* Default expiry and backward-clock safety cannot serve stale content. */
    now += 43200.0;
    write_page(file, "#!c=2\nShort lifetime");
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(rns_browser_loaded_from_cache(browser));
    now += 2.0;
    write_page(file, "#!c=0\nUncached page");
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    write_page(file, "Cache again");
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    const uint8_t empty_map[] = {0x80};
    write_page(file, "Form response must not replace cached page");
    assert(rns_browser_open(browser, url, &identity, empty_map, sizeof empty_map) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(rns_browser_loaded_from_cache(browser));
    const rns_micron_page *cached = rns_browser_page(browser);
    assert(strcmp(rns_micron_span_text(cached, &cached->spans[0]), "Cache again") == 0);
    now -= 1.0;
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    rns_browser_cache_clear(browser);
    write_page(file, "#!c=184467440737095516160\nMalformed lifetime");
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
    /* Nine distinct pages evict the oldest of the eight bounded entries. */
    char first_url[128] = {0};
    for (unsigned i = 0; i < 9; ++i) {
        char name[32], other_file[256], other_url[128];
        (void)snprintf(name, sizeof name, "cached%u.mu", i);
        (void)snprintf(other_file, sizeof other_file, "%s/%s", root, name);
        (void)snprintf(other_url, sizeof other_url, "%.32s:/page/%s", url, name);
        if (!i) strcpy(first_url, other_url);
        write_page(other_file, "Eviction test");
        assert(rns_hosted_node_publish_page(node, name) == RNS_OK);
        now += 1.0;
        assert(rns_browser_open(browser, other_url, &identity, NULL, 0U) == RNS_OK);
        poll_until(client, server, node, browser, RNS_BROWSER_COMPLETE);
        assert(unlink(other_file) == 0);
    }
    assert(rns_browser_open(browser, first_url, &identity, NULL, 0U) == RNS_OK);
    assert(!rns_browser_loaded_from_cache(browser));
    rns_browser_destroy(browser);
    rns_hosted_node_destroy(node);
    rns_runtime_destroy(client); rns_runtime_destroy(server);
    assert(unlink(file) == 0 && rmdir(root) == 0);
    assert(rns_browser_page_url(NULL) == NULL);
    return 0;
}
