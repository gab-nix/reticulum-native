#ifndef RETICULUM_BROWSER_H
#define RETICULUM_BROWSER_H

#include <stddef.h>
#include <stdint.h>

#include "reticulum/identity.h"
#include "reticulum/micron.h"
#include "reticulum/runtime.h"
#include "reticulum/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_BROWSER_URL_MAX 1024u
#define RNS_BROWSER_PATH_MAX 768u

typedef struct rns_browser rns_browser_t;

typedef enum rns_browser_state {
    RNS_BROWSER_IDLE = 0,
    RNS_BROWSER_PATH_DISCOVERY,
    RNS_BROWSER_LINK_ESTABLISHMENT,
    RNS_BROWSER_REQUEST_TRANSMISSION,
    RNS_BROWSER_COMPLETE,
    RNS_BROWSER_CANCELLED,
    RNS_BROWSER_FAILED
} rns_browser_state_t;

typedef struct rns_browser_options {
    size_t max_response_size;
    double request_timeout_seconds;
} rns_browser_options_t;

rns_status_t rns_browser_create(rns_browser_t **browser, rns_runtime_t *runtime,
                                const rns_browser_options_t *options);
void rns_browser_destroy(rns_browser_t *browser);

/* URL syntax is <32-hex-node>:/page/path. The supplied identity must be the
 * verified public identity from that node's announce. */
rns_status_t rns_browser_open(rns_browser_t *browser, const char *url,
                              const rns_identity *node_identity,
                              const uint8_t *form_msgpack,
                              size_t form_msgpack_length);
rns_status_t rns_browser_poll(rns_browser_t *browser);
void rns_browser_cancel(rns_browser_t *browser);

rns_browser_state_t rns_browser_state(const rns_browser_t *browser);
double rns_browser_progress(const rns_browser_t *browser);
rns_status_t rns_browser_error(const rns_browser_t *browser);
const char *rns_browser_url(const rns_browser_t *browser);
const rns_micron_page *rns_browser_page(const rns_browser_t *browser);

#ifdef __cplusplus
}
#endif
#endif
