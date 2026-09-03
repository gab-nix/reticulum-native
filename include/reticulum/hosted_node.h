#ifndef RETICULUM_HOSTED_NODE_H
#define RETICULUM_HOSTED_NODE_H

#include "reticulum/hosted_form.h"
#include "reticulum/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rns_hosted_node rns_hosted_node_t;

typedef struct rns_hosted_page_execution {
    /* All pointers are immutable and callback-scoped. The source was read
     * from the same descriptor-pinned regular file that was classified as
     * executable, so providers never need to reopen an attacker-swappable
     * pathname. */
    const char *relative_path;
    const uint8_t *source;
    size_t source_length;
    const rns_hosted_form_t *form;
    const uint8_t *link_id; /* 16 bytes. */
    const rns_identity *remote_identity; /* NULL until the link identifies. */
    double requested_at;
} rns_hosted_page_execution_t;

/* Generates raw page bytes into the caller-owned bounded output buffer.
 * This callback is the only executable-page mechanism in libreticulum: the
 * library never invokes a shell or executes page bytes. Applications may use
 * an in-process interpreter or a separately sandboxed helper. The callback
 * runs synchronously and must not re-enter the node or its runtime. */
typedef rns_status_t (*rns_hosted_page_executor_t)(
    rns_hosted_node_t *node,
    const rns_hosted_page_execution_t *execution,
    uint8_t *output, size_t output_capacity, size_t *output_length,
    void *context);

typedef struct rns_hosted_node_options {
    /* POSIX directory roots, at most 4096 bytes. The final root component and
     * descendants may not be symlinks; configured ancestor paths are trusted. */
    const char *pages_root;
    const char *files_root; /* Optional, local reads only in this version. */
    size_t max_content_size; /* Zero selects 16 KiB; absolute limit is 8 MiB-5. */
    rns_request_access_t access;
    const uint8_t *allow_identity_hashes;
    size_t allow_identity_count;
    /* NULL keeps executable pages disabled. A non-NULL provider explicitly
     * opts in and is called synchronously by rns_runtime_poll(). */
    rns_hosted_page_executor_t page_executor;
    void *page_executor_context;
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
 * executable pages fail closed unless a page executor was explicitly supplied
 * at creation. A regular `<page>.allowed` sidecar restricts that page to its
 * newline-delimited identity hashes, in addition to the global access policy.
 * Executable policy sidecars always fail closed. Up to the runtime request-
 * handler limit is supported. */
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
