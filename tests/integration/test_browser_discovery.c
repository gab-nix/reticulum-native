#include "reticulum/browser.h"
#include "reticulum/udp.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static double fake_now(void *context) { return *(double *)context; }

static uint16_t reserve_port(void) {
    rns_udp_endpoint_t *endpoint = NULL;
    rns_udp_address_t address;
    assert(rns_udp_endpoint_create(&endpoint, RNS_UDP_IPV4) == RNS_OK);
    assert(rns_udp_bind(endpoint, "127.0.0.1", 0U) == RNS_OK);
    assert(rns_udp_local_address(endpoint, &address) == RNS_OK);
    rns_udp_endpoint_destroy(endpoint);
    return address.port;
}

int main(void) {
    rns_config_t config;
    rns_config_init(&config);
    config.interface_count = 1U;
    rns_config_interface_t *interface = &config.interfaces[0];
    strcpy(interface->name, "browser-discovery-test");
    interface->type = RNS_CONFIG_UDP;
    interface->type_set = interface->enabled = true;
    strcpy(interface->listen_ip, "127.0.0.1");
    strcpy(interface->forward_ip, "127.0.0.1");
    interface->listen_port = reserve_port();
    interface->forward_port = interface->listen_port;
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    double now = 100.0;
    rns_browser_options_t options = {
        .path_timeout_seconds = 2.0, .clock = fake_now, .clock_context = &now
    };
    rns_browser_t *browser = NULL;
    rns_identity identity = {0};
    const char *url = "00000000000000000000000000000000:/page/index.mu";
    assert(rns_browser_create(&browser, runtime, &options) == RNS_OK);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    now = 101.999;
    assert(rns_browser_poll(browser) == RNS_OK);
    assert(rns_browser_state(browser) == RNS_BROWSER_PATH_DISCOVERY);
    now = 102.0;
    assert(rns_browser_poll(browser) == RNS_ERROR_TIMEOUT);
    assert(rns_browser_state(browser) == RNS_BROWSER_FAILED);
    assert(strcmp(rns_browser_url(browser), url) == 0);

    /* A new attempt gets a fresh deadline; cancellation remains terminal. */
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    now = 103.0;
    assert(rns_browser_poll(browser) == RNS_OK);
    rns_browser_cancel(browser);
    now = 200.0;
    assert(rns_browser_poll(browser) == RNS_OK);
    assert(rns_browser_state(browser) == RNS_BROWSER_CANCELLED);

    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    now = 199.0;
    assert(rns_browser_poll(browser) == RNS_ERROR_INVALID_STATE);
    now = NAN;
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_ERROR_INVALID_STATE);
    rns_browser_destroy(browser);

    options.path_timeout_seconds = NAN;
    assert(rns_browser_create(&browser, runtime, &options) == RNS_ERROR_INVALID_ARGUMENT);
    assert(browser == NULL);
    options.path_timeout_seconds = -1.0;
    assert(rns_browser_create(&browser, runtime, &options) == RNS_ERROR_INVALID_ARGUMENT);
    options.path_timeout_seconds = 0.0;
    now = 300.0;
    assert(rns_browser_create(&browser, runtime, &options) == RNS_OK);
    assert(rns_browser_open(browser, url, &identity, NULL, 0U) == RNS_OK);
    now = 309.999;
    assert(rns_browser_poll(browser) == RNS_OK);
    now = 310.0;
    assert(rns_browser_poll(browser) == RNS_ERROR_TIMEOUT);
    rns_browser_destroy(browser);
    rns_runtime_destroy(runtime);
    return 0;
}
