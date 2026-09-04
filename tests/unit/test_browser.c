#include "reticulum/browser.h"

#include <assert.h>

int main(void) {
    rns_browser_t *browser = (rns_browser_t *)1;
    rns_identity identity = {0};
    rns_runtime_t *runtime = NULL;
    rns_config_t config;
    assert(rns_browser_create(NULL, NULL, NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_create(&browser, NULL, NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(browser == NULL);
    rns_config_init(&config);
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    assert(rns_browser_create(&browser, runtime, NULL) == RNS_OK);
    assert(browser != NULL);
    assert(rns_browser_state(browser) == RNS_BROWSER_IDLE);
    uint8_t oversized_form[RNS_BROWSER_FORM_MAX + 1u] = {0};
    assert(rns_browser_open(browser,
                            "00000000000000000000000000000000:/page/index.mu",
                            &identity, oversized_form,
                            sizeof oversized_form) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_open(browser, "not-a-node:/page/index.mu", &identity,
                            NULL, 0U) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_open(browser,
                            "00000000000000000000000000000000:/page/../secret",
                            &identity, NULL, 0U) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_open(browser,
                            "00000000000000000000000000000000:/page/index.mu",
                            &identity, NULL, 0U) == RNS_ERROR_INVALID_STATE);
    assert(rns_browser_state(browser) == RNS_BROWSER_FAILED);
    rns_browser_destroy(browser);
    rns_runtime_destroy(runtime);
    assert(rns_browser_open(NULL, "00000000000000000000000000000000:/page/index.mu",
                            &identity, NULL, 0U) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_poll(NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_state(NULL) == RNS_BROWSER_FAILED);
    assert(rns_browser_progress(NULL) == 0.0);
    assert(rns_browser_error(NULL) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_browser_url(NULL) == NULL);
    assert(rns_browser_page(NULL) == NULL);
    rns_browser_cancel(NULL);
    rns_browser_destroy(NULL);
    return 0;
}
