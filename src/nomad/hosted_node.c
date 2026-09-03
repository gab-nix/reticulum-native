#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "reticulum/hosted_node.h"
#include "reticulum/destination.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rns_hosted_node {
    rns_runtime_t *runtime;
    rns_identity identity;
    rns_runtime_destination_t *destination;
    rns_runtime_link_t *links[RNS_RUNTIME_MAX_LINKS];
    rns_runtime_request_handler_options_t handler_options;
    uint8_t allowed[RNS_RUNTIME_MAX_REQUEST_ALLOWLIST * 16U];
    int pages_fd;
    int files_fd;
    size_t max_content;
    size_t page_count;
};

static bool safe_relative(const char *path) {
    if (path == NULL) return false;
    size_t length = strnlen(path, RNS_REQUEST_PATH_MAX + 1U);
    if (length == 0U || length > RNS_REQUEST_PATH_MAX - 6U) return false;
    bool start = true;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];
        /* Conservative printable ASCII namespace. Percent escapes are never
         * decoded, preventing mismatches with browser URL normalization. */
        if (c < 0x21U || c > 0x7eU || c == '\\' || c == '%' || c == '?' ||
            c == '#' || (start && (c == '.' || c == '/'))) return false;
        start = c == '/';
    }
    return !start;
}

/* Walk each component under pinned directory descriptors. O_NOFOLLOW at every
 * step prevents both symlink traversal and replacement races. */
static rns_status_t open_relative(int root, const char *relative, bool page,
                                   int *result) {
    if (root < 0) return RNS_ERROR_NOT_FOUND;
    if (!safe_relative(relative)) return RNS_ERROR_INVALID_ARGUMENT;
    char path[RNS_REQUEST_PATH_MAX + 1U];
    memcpy(path, relative, strlen(relative) + 1U);
    int parent = dup(root);
    if (parent < 0) return RNS_ERROR_IO;
    char *component = path;
    char *separator;
    while ((separator = strchr(component, '/')) != NULL) {
        *separator = '\0';
        int next = openat(parent, component,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
        if (next < 0) return RNS_ERROR_IO;
        parent = next;
        component = separator + 1;
    }
    if (page) {
        size_t component_length = strlen(component);
        if (component_length >= 8U &&
            strcmp(component + component_length - 8U, ".allowed") == 0) {
            close(parent);
            return RNS_ERROR_UNSUPPORTED;
        }
        char sidecar[RNS_REQUEST_PATH_MAX + 9U];
        memcpy(sidecar, component, component_length);
        memcpy(sidecar + component_length, ".allowed", 9U);
        struct stat sidecar_info;
        if (fstatat(parent, sidecar, &sidecar_info, AT_SYMLINK_NOFOLLOW) == 0 ||
            errno != ENOENT) {
            close(parent);
            return RNS_ERROR_UNSUPPORTED;
        }
    }
    int fd = openat(parent, component,
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(parent);
    if (fd < 0) return RNS_ERROR_IO;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return RNS_ERROR_IO;
    }
    if (page && (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) {
        close(fd);
        return RNS_ERROR_UNSUPPORTED;
    }
    *result = fd;
    return RNS_OK;
}

rns_status_t rns_hosted_node_read(rns_hosted_node_t *node, const char *path,
                                 uint8_t *output, size_t capacity,
                                 size_t *length) {
    if (length != NULL) *length = 0U;
    if (node == NULL || path == NULL || output == NULL || length == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    bool page = strncmp(path, "/page/", 6U) == 0;
    if (!page && strncmp(path, "/file/", 6U) != 0)
        return RNS_ERROR_INVALID_ARGUMENT;
    int fd = -1;
    rns_status_t status = open_relative(page ? node->pages_fd : node->files_fd,
                                        path + 6U, page, &fd);
    if (status != RNS_OK) return status;
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < 0) status = RNS_ERROR_IO;
    else if ((uintmax_t)info.st_size > node->max_content ||
             (uintmax_t)info.st_size > capacity) status = RNS_ERROR_OVERFLOW;
    size_t used = 0U;
    while (status == RNS_OK) {
        size_t available = capacity < node->max_content ? capacity : node->max_content;
        if (used == available) {
            uint8_t extra;
            ssize_t count = read(fd, &extra, 1U);
            if (count < 0 && errno == EINTR) continue;
            if (count != 0) status = count > 0 ? RNS_ERROR_OVERFLOW : RNS_ERROR_IO;
            break;
        }
        ssize_t count = read(fd, output + used, available - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { status = RNS_ERROR_IO; break; }
        if (count == 0) break;
        used += (size_t)count;
    }
    close(fd);
    if (status == RNS_OK) *length = used;
    return status;
}

static rns_status_t serve_page(
    rns_runtime_request_handler_t *handler, rns_runtime_link_t *link,
    const rns_request_view_t *request, const rns_identity *remote_identity,
    uint8_t *response, size_t capacity, size_t *length, void *context) {
    (void)link; (void)request; (void)remote_identity;
    if (capacity < 5U) return RNS_ERROR_OVERFLOW;
    size_t content_length = 0U;
    rns_status_t status = rns_hosted_node_read(
        context, rns_runtime_request_handler_path(handler), response + 5U,
        capacity - 5U, &content_length);
    if (status != RNS_OK) return status;
    size_t prefix = content_length <= UINT8_MAX ? 2U :
                    content_length <= UINT16_MAX ? 3U : 5U;
    memmove(response + prefix, response + 5U, content_length);
    response[0] = prefix == 2U ? 0xc4U : prefix == 3U ? 0xc5U : 0xc6U;
    for (size_t i = 1U; i < prefix; ++i)
        response[i] = (uint8_t)(content_length >> (8U * (prefix - i - 1U)));
    *length = content_length + prefix;
    return RNS_OK;
}

static void accepted(rns_runtime_destination_t *destination,
                      rns_runtime_link_t *link, void *context) {
    (void)destination;
    rns_hosted_node_t *node = context;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; ++i)
        if (node->links[i] == NULL) { node->links[i] = link; return; }
    /* Runtime has the same global link limit, so this cannot overflow while
     * callers respect the ownership contract. */
}

rns_status_t rns_hosted_node_create(
    rns_hosted_node_t **output, rns_runtime_t *runtime,
    const rns_identity *identity, const rns_hosted_node_options_t *options) {
    if (output == NULL) return RNS_ERROR_INVALID_ARGUMENT;
    *output = NULL;
    if (runtime == NULL || identity == NULL || options == NULL ||
        options->pages_root == NULL ||
        strnlen(options->pages_root, 4097U) > 4096U ||
        (options->files_root != NULL && strnlen(options->files_root, 4097U) > 4096U) ||
        options->max_content_size > RNS_REQUEST_DEFAULT_MAX_RESPONSE - 5U ||
        options->access < RNS_REQUEST_ALLOW_NONE ||
        options->access > RNS_REQUEST_ALLOW_LIST ||
        options->allow_identity_count > RNS_RUNTIME_MAX_REQUEST_ALLOWLIST ||
        (options->allow_identity_count > 0U && options->allow_identity_hashes == NULL))
        return RNS_ERROR_INVALID_ARGUMENT;
    rns_hosted_node_t *node = calloc(1U, sizeof *node);
    if (node == NULL) return RNS_ERROR_NO_MEMORY;
    node->pages_fd = node->files_fd = -1;
    node->runtime = runtime;
    node->identity = *identity;
    node->max_content = options->max_content_size != 0U ? options->max_content_size : 16384U;
    node->pages_fd = open(options->pages_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (options->files_root != NULL)
        node->files_fd = open(options->files_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (node->pages_fd < 0 || (options->files_root != NULL && node->files_fd < 0)) {
        rns_hosted_node_destroy(node);
        return RNS_ERROR_IO;
    }
    node->handler_options.access = options->access;
    node->handler_options.max_response_size = node->max_content + 5U;
    node->handler_options.callback = serve_page;
    node->handler_options.callback_context = node;
    node->handler_options.allow_identity_count = options->allow_identity_count;
    if (options->allow_identity_count != 0U)
        memcpy(node->allowed, options->allow_identity_hashes, options->allow_identity_count * 16U);
    node->handler_options.allow_identity_hashes = node->allowed;
    static const char *const aspects[] = {"node"};
    uint8_t hash[16];
    rns_status_t status = RNS_ERROR_CRYPTO;
    if (rns_destination_hash(identity, "nomadnetwork", aspects, 1U, hash))
        status = rns_runtime_register_link_destination(runtime, hash, identity,
                    NULL, accepted, node, &node->destination);
    if (status != RNS_OK) { rns_hosted_node_destroy(node); return status; }
    *output = node;
    return RNS_OK;
}

rns_status_t rns_hosted_node_publish_page(rns_hosted_node_t *node,
                                         const char *relative_path) {
    if (node == NULL || !safe_relative(relative_path)) return RNS_ERROR_INVALID_ARGUMENT;
    int fd = -1;
    rns_status_t status = open_relative(node->pages_fd, relative_path, true, &fd);
    if (status != RNS_OK) return status;
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < 0) status = RNS_ERROR_IO;
    else if ((uintmax_t)info.st_size > node->max_content) status = RNS_ERROR_OVERFLOW;
    close(fd);
    if (status != RNS_OK) return status;
    char path[RNS_REQUEST_PATH_MAX + 1U] = "/page/";
    memcpy(path + 6U, relative_path, strlen(relative_path) + 1U);
    rns_runtime_request_handler_t *handler = NULL;
    status = rns_runtime_destination_register_request_handler(node->destination,
               path, &node->handler_options, &handler);
    if (status == RNS_OK) node->page_count++;
    return status;
}

const uint8_t *rns_hosted_node_destination(const rns_hosted_node_t *node) {
    return node != NULL ? rns_runtime_destination_hash(node->destination) : NULL;
}
size_t rns_hosted_node_page_count(const rns_hosted_node_t *node) {
    return node != NULL ? node->page_count : 0U;
}
void rns_hosted_node_poll(rns_hosted_node_t *node) {
    if (node == NULL) return;
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; ++i)
        if (node->links[i] != NULL && rns_runtime_link_state(node->links[i]) == RNS_LINK_CLOSED) {
            rns_runtime_link_destroy(node->links[i]);
            node->links[i] = NULL;
        }
}
void rns_hosted_node_destroy(rns_hosted_node_t *node) {
    if (node == NULL) return;
    rns_runtime_destination_destroy(node->destination);
    for (size_t i = 0U; i < RNS_RUNTIME_MAX_LINKS; ++i)
        rns_runtime_link_destroy(node->links[i]);
    if (node->pages_fd >= 0) close(node->pages_fd);
    if (node->files_fd >= 0) close(node->files_fd);
    memset(&node->identity, 0, sizeof node->identity);
    free(node);
}
rns_status_t rns_hosted_node_announce(rns_hosted_node_t *node,
                                     const uint8_t *name, size_t name_length) {
    if (node == NULL || name_length > 128U || (name == NULL && name_length != 0U))
        return RNS_ERROR_INVALID_ARGUMENT;
    static const char *const aspects[] = {"node"};
    return rns_runtime_announce(node->runtime, &node->identity, "nomadnetwork",
                                aspects, 1U, name, name_length);
}
