#ifndef RETICULUM_HOSTED_NODE_H
#define RETICULUM_HOSTED_NODE_H

#include "reticulum/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_hosted_node rns_hosted_node_t;

typedef struct rns_hosted_node_options {
    /* POSIX directory roots, at most 4096 bytes. The final root component and
     * descendants may not be symlinks; configured ancestor paths are trusted. */
    const char *pages_root;
    const char *files_root; /* Optional, local reads only in this version. */
    size_t max_content_size; /* Zero selects 16 KiB; absolute limit is 8 MiB-5. */
    rns_request_access_t access;
    const uint8_t *allow_identity_hashes;
    size_t allow_identity_count;
} rns_hosted_node_options_t;

/* Explicit opt-in only: create neither announces nor scans/publishes files.
 * runtime must outlive node. Use from its event-loop owner, outside callbacks.
 * Directory descriptors pin the roots; contents are read afresh per request. */
rns_status_t rns_hosted_node_create(
    rns_hosted_node_t **node, rns_runtime_t *runtime,
    const rns_identity *identity, const rns_hosted_node_options_t *options);
void rns_hosted_node_destroy(rns_hosted_node_t *node);
const uint8_t *rns_hosted_node_destination(const rns_hosted_node_t *node);

/* Publish one relative page path, for example "index.mu" or "guide/start.mu".
 * Publishes /page/<relative_path>. Hidden files, traversal, symlinks and
 * executable pages fail closed. A regular `<page>.allowed` sidecar restricts
 * that page to its newline-delimited identity hashes, in addition to the
 * global access policy. Up to the runtime request-handler limit is supported. */
rns_status_t rns_hosted_node_publish_page(rns_hosted_node_t *node,
                                         const char *relative_path);
size_t rns_hosted_node_page_count(const rns_hosted_node_t *node);

/* Safe local storage adapter for previews/tests. Accepts /page/ or /file/.
 * This does not apply network authentication; callers must not expose it as an
 * unauthenticated service. Remote /file streaming metadata is not implemented. */
rns_status_t rns_hosted_node_read(rns_hosted_node_t *node, const char *path,
                                 uint8_t *output, size_t capacity,
                                 size_t *length);
/* Reap closed inbound links after polling runtime; never blocks on networking. */
void rns_hosted_node_poll(rns_hosted_node_t *node);
rns_status_t rns_hosted_node_announce(rns_hosted_node_t *node,
                                     const uint8_t *name, size_t name_length);

#ifdef __cplusplus
}
#endif
#endif
