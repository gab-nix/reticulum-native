#include "reticulum/browser.h"
#include "reticulum/packet.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct rns_browser {
    rns_runtime_t *runtime;
    rns_runtime_link_t *link;
    rns_request_receipt_t *receipt;
    rns_browser_options_t options;
    rns_browser_state_t state;
    rns_status_t error;
    rns_identity identity;
    uint8_t destination[16];
    char url[RNS_BROWSER_URL_MAX + 1U];
    char path[RNS_BROWSER_PATH_MAX + 1U];
    uint8_t form[RNS_MTU];
    size_t form_length;
    rns_micron_page page;
};

static int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool valid_utf8(const uint8_t *data, size_t length) {
    size_t i = 0U;
    while (i < length) {
        uint8_t first = data[i++];
        if (first < 0x80U) {
            if (first == 0U) return false;
            continue;
        }
        size_t following;
        uint32_t codepoint;
        if ((first & 0xe0U) == 0xc0U) { following = 1U; codepoint = first & 0x1fU; }
        else if ((first & 0xf0U) == 0xe0U) { following = 2U; codepoint = first & 0x0fU; }
        else if ((first & 0xf8U) == 0xf0U) { following = 3U; codepoint = first & 0x07U; }
        else return false;
        if (i + following > length) return false;
        for (size_t j = 0U; j < following; j++) {
            uint8_t next = data[i++];
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (uint32_t)(next & 0x3fU);
        }
        if ((following == 1U && codepoint < 0x80U) ||
            (following == 2U && codepoint < 0x800U) ||
            (following == 3U && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    }
    return true;
}

static rns_status_t parse_url(const char *url, uint8_t destination[16],
                              char path[RNS_BROWSER_PATH_MAX + 1U]) {
    if (url == NULL || strlen(url) > RNS_BROWSER_URL_MAX || strlen(url) < 35U ||
        url[32] != ':' || url[33] != '/') return RNS_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0U; i < 16U; i++) {
        int high = hex_value(url[i * 2U]);
        int low = hex_value(url[i * 2U + 1U]);
        if (high < 0 || low < 0) return RNS_ERROR_INVALID_ARGUMENT;
        destination[i] = (uint8_t)((high << 4) | low);
    }
    const char *source_path = url + 33U;
    size_t path_length = strlen(source_path);
    if (path_length == 0U || path_length > RNS_BROWSER_PATH_MAX ||
        strstr(source_path, "//") != NULL) return RNS_ERROR_INVALID_ARGUMENT;
    const char *segment = source_path;
    while ((segment = strstr(segment, "..")) != NULL) {
        bool left = segment == source_path || segment[-1] == '/';
        bool right = segment[2] == '\0' || segment[2] == '/' || segment[2] == '#';
        if (left && right) return RNS_ERROR_INVALID_ARGUMENT;
        segment += 2;
    }
    const char *anchor = strchr(source_path, '#');
    if (anchor != NULL) path_length = (size_t)(anchor - source_path);
    if (path_length == 0U) return RNS_ERROR_INVALID_ARGUMENT;
    memcpy(path, source_path, path_length);
    path[path_length] = '\0';
    return RNS_OK;
}

static void browser_fail(rns_browser_t *browser, rns_status_t status) {
    if (browser->state == RNS_BROWSER_CANCELLED ||
        browser->state == RNS_BROWSER_COMPLETE) return;
    browser->error = status;
    browser->state = RNS_BROWSER_FAILED;
}

static void response_received(rns_request_receipt_t *receipt,
                              rns_request_state_t state, rns_status_t status,
                              const uint8_t *response, size_t response_length,
                              void *context) {
    (void)receipt;
    rns_browser_t *browser = context;
    if (state != RNS_REQUEST_COMPLETE || status != RNS_OK) {
        browser_fail(browser, status == RNS_OK ? RNS_ERROR_PROTOCOL : status);
        return;
    }
    if (response_length > browser->options.max_response_size ||
        !valid_utf8(response, response_length) ||
        !rns_micron_parse(&browser->page, response, response_length)) {
        browser_fail(browser, response_length > browser->options.max_response_size
                                  ? RNS_ERROR_OVERFLOW : RNS_ERROR_PROTOCOL);
        return;
    }
    browser->error = RNS_OK;
    browser->state = RNS_BROWSER_COMPLETE;
}

/* The runtime consumes supported response resources before application packet
 * dispatch. Reaching this callback means the peer used a resource context that
 * could not be correlated with the active request. */
static void link_packet(rns_runtime_link_t *link, uint8_t packet_context,
                        const uint8_t *plaintext, size_t plaintext_length,
                        void *context) {
    rns_browser_t *browser = context;
    (void)link;
    (void)plaintext;
    (void)plaintext_length;
    if (packet_context != RNS_LINK_CONTEXT_RESOURCE_ADV &&
        packet_context != RNS_LINK_CONTEXT_RESOURCE) return;
    if (browser->receipt != NULL) rns_request_receipt_cancel(browser->receipt);
    browser_fail(browser, RNS_ERROR_UNSUPPORTED);
}

static void link_changed(rns_runtime_link_t *link, rns_link_state state,
                         rns_status_t reason, void *context) {
    rns_browser_t *browser = context;
    if (state == RNS_LINK_ACTIVE && browser->receipt == NULL) {
        rns_request_options_t options = {
            .timeout_seconds = browser->options.request_timeout_seconds,
            .max_response_size = browser->options.max_response_size,
            .callback = response_received,
            .callback_context = browser
        };
        rns_status_t status = rns_runtime_link_request(
            link, browser->path, browser->form_length ? browser->form : NULL,
            browser->form_length, &options, &browser->receipt);
        if (status != RNS_OK) browser_fail(browser, status);
        else browser->state = RNS_BROWSER_REQUEST_TRANSMISSION;
    } else if (state == RNS_LINK_CLOSED) {
        browser_fail(browser, reason == RNS_OK ? RNS_ERROR_INVALID_STATE : reason);
    }
}

rns_status_t rns_browser_create(rns_browser_t **output, rns_runtime_t *runtime,
                                const rns_browser_options_t *options) {
    if (output == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (runtime == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    rns_browser_t *browser = calloc(1U, sizeof *browser);
    if (browser == NULL) return RNS_ERROR_NO_MEMORY;
    browser->runtime = runtime;
    if (options != NULL) browser->options = *options;
    if (browser->options.max_response_size == 0U)
        browser->options.max_response_size = RNS_REQUEST_DEFAULT_MAX_RESPONSE;
    browser->state = RNS_BROWSER_IDLE;
    *output = browser;
    return RNS_OK;
}

void rns_browser_destroy(rns_browser_t *browser) {
    if (browser == NULL) return;
    rns_request_receipt_destroy(browser->receipt);
    rns_runtime_link_destroy(browser->link);
    free(browser);
}

rns_status_t rns_browser_open(rns_browser_t *browser, const char *url,
                              const rns_identity *node_identity,
                              const uint8_t *form_msgpack,
                              size_t form_msgpack_length) {
    if (browser == NULL || node_identity == NULL ||
        (form_msgpack == NULL && form_msgpack_length != 0U) ||
        form_msgpack_length > sizeof browser->form) return RNS_ERROR_INVALID_ARGUMENT;
    uint8_t destination[16];
    char path[RNS_BROWSER_PATH_MAX + 1U];
    rns_status_t status = parse_url(url, destination, path);
    if (status != RNS_OK) return status;
    rns_request_receipt_destroy(browser->receipt);
    browser->receipt = NULL;
    rns_runtime_link_destroy(browser->link);
    browser->link = NULL;
    memcpy(browser->destination, destination, sizeof destination);
    browser->identity = *node_identity;
    memcpy(browser->path, path, strlen(path) + 1U);
    memcpy(browser->url, url, strlen(url) + 1U);
    if (form_msgpack_length != 0U)
        memcpy(browser->form, form_msgpack, form_msgpack_length);
    browser->form_length = form_msgpack_length;
    browser->error = RNS_OK;
    browser->state = RNS_BROWSER_PATH_DISCOVERY;
    rns_path_entry route;
    if (rns_runtime_path_lookup(browser->runtime, destination, &route) != RNS_OK) {
        status = rns_runtime_request_path(browser->runtime, destination);
        if (status != RNS_OK) browser_fail(browser, status);
    }
    return browser->state == RNS_BROWSER_FAILED ? browser->error : RNS_OK;
}

rns_status_t rns_browser_poll(rns_browser_t *browser) {
    if (browser == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    if (browser->state != RNS_BROWSER_PATH_DISCOVERY) return browser->error;
    rns_path_entry route;
    if (rns_runtime_path_lookup(browser->runtime, browser->destination, &route) != RNS_OK)
        return RNS_OK;
    rns_runtime_link_options_t options = {
        .state_callback = link_changed,
        .packet_callback = link_packet,
        .callback_context = browser
    };
    browser->state = RNS_BROWSER_LINK_ESTABLISHMENT;
    rns_status_t status = rns_runtime_link_open(
        browser->runtime, browser->destination, &browser->identity, &options,
        &browser->link);
    if (status != RNS_OK) browser_fail(browser, status);
    return status;
}

void rns_browser_cancel(rns_browser_t *browser) {
    if (browser == NULL || browser->state == RNS_BROWSER_COMPLETE ||
        browser->state == RNS_BROWSER_FAILED) return;
    rns_request_receipt_cancel(browser->receipt);
    browser->state = RNS_BROWSER_CANCELLED;
    browser->error = RNS_OK;
}

rns_browser_state_t rns_browser_state(const rns_browser_t *browser) {
    return browser != NULL ? browser->state : RNS_BROWSER_FAILED;
}

double rns_browser_progress(const rns_browser_t *browser) {
    if (browser == NULL) return 0.0;
    switch (browser->state) {
        case RNS_BROWSER_IDLE: return 0.0;
        case RNS_BROWSER_PATH_DISCOVERY: return 0.1;
        case RNS_BROWSER_LINK_ESTABLISHMENT: return 0.35;
        case RNS_BROWSER_REQUEST_TRANSMISSION: return 0.6;
        case RNS_BROWSER_COMPLETE: return 1.0;
        default: return 0.0;
    }
}

rns_status_t rns_browser_error(const rns_browser_t *browser) {
    return browser != NULL ? browser->error : RNS_ERROR_INVALID_ARGUMENT;
}

const char *rns_browser_url(const rns_browser_t *browser) {
    return browser != NULL ? browser->url : NULL;
}

const rns_micron_page *rns_browser_page(const rns_browser_t *browser) {
    return browser != NULL && browser->state == RNS_BROWSER_COMPLETE
               ? &browser->page : NULL;
}
