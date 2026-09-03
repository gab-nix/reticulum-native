#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "reticulum/hosted_node.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char root[] = "/tmp/rns-hosted-unit-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char page[512], sidecar[512], linkpath[512], directory[512], nested[512], fifo[512];
    (void)snprintf(page, sizeof page, "%s/index.mu", root);
    (void)snprintf(sidecar, sizeof sidecar, "%s/index.mu.allowed", root);
    (void)snprintf(linkpath, sizeof linkpath, "%s/link.mu", root);
    (void)snprintf(directory, sizeof directory, "%s/nested", root);
    (void)snprintf(nested, sizeof nested, "%s/nested/page.mu", root);
    (void)snprintf(fifo, sizeof fifo, "%s/pipe", root);
    FILE *file = fopen(page, "wb");
    assert(file != NULL && fwrite("Hello", 1U, 5U, file) == 5U);
    assert(fclose(file) == 0);
    assert(mkdir(directory, 0700) == 0);
    file = fopen(nested, "wb");
    assert(file != NULL && fwrite("Hi", 1U, 2U, file) == 2U && fclose(file) == 0);
    assert(mkfifo(fifo, 0600) == 0);
    assert(symlink(page, linkpath) == 0);
    rns_config_t config;
    rns_config_init(&config);
    rns_runtime_t *runtime = NULL;
    assert(rns_runtime_create(&runtime, &config, NULL) == RNS_OK);
    rns_identity identity;
    assert(rns_identity_generate(&identity));
    rns_hosted_node_options_t options = {
        .pages_root = root, .files_root = root, .max_content_size = 5U,
        .access = RNS_REQUEST_ALLOW_ALL};
    rns_hosted_node_t *node = NULL;
    assert(rns_hosted_node_create(&node, runtime, &identity, &options) == RNS_OK);
    assert(rns_hosted_node_destination(node) != NULL);
    uint8_t output[16];
    size_t length = 0U;
    assert(rns_hosted_node_read(node, "/page/index.mu", output, sizeof output, &length) == RNS_OK);
    assert(length == 5U && memcmp(output, "Hello", 5U) == 0);
    assert(rns_hosted_node_read(node, "/file/index.mu", output, 4U, &length) == RNS_ERROR_OVERFLOW && length == 0U);
    assert(rns_hosted_node_read(node, "/file/index.mu", output, sizeof output, &length) == RNS_OK);
    static const char *const bad[] = {
        "../index.mu", "nested/../index.mu", "/index.mu", ".secret",
        "nested//index.mu", "nested/", "index.mu#anchor", "%2e%2e/key", "bad\\key", "bad\nkey"};
    for (size_t i = 0U; i < sizeof bad / sizeof bad[0]; ++i)
        assert(rns_hosted_node_publish_page(node, bad[i]) == RNS_ERROR_INVALID_ARGUMENT);
    assert(rns_hosted_node_publish_page(node, "link.mu") == RNS_ERROR_IO);
    assert(rns_hosted_node_publish_page(node, "nested") == RNS_ERROR_IO);
    assert(rns_hosted_node_publish_page(node, "pipe") == RNS_ERROR_IO);
    assert(rns_hosted_node_publish_page(node, "nested/page.mu") == RNS_OK);
    assert(rns_hosted_node_read(node, "/page/nested/page.mu", output, sizeof output, &length) == RNS_OK);
    assert(length == 2U && memcmp(output, "Hi", 2U) == 0);
    assert(rns_hosted_node_publish_page(node, "index.mu") == RNS_OK);
    assert(rns_hosted_node_publish_page(node, "index.mu") != RNS_OK);
    assert(rns_hosted_node_page_count(node) == 2U);
    assert(chmod(page, 0700) == 0);
    assert(rns_hosted_node_read(node, "/page/index.mu", output, sizeof output, &length) == RNS_ERROR_UNSUPPORTED);
    assert(chmod(page, 0600) == 0);
    file = fopen(sidecar, "wb");
    assert(file != NULL && fclose(file) == 0);
    assert(rns_hosted_node_read(node, "/page/index.mu", output, sizeof output, &length) == RNS_ERROR_UNSUPPORTED);
    assert(unlink(sidecar) == 0);
    file = fopen(page, "ab");
    assert(file != NULL && fwrite("!", 1U, 1U, file) == 1U && fclose(file) == 0);
    assert(rns_hosted_node_read(node, "/page/index.mu", output, sizeof output, &length) == RNS_ERROR_OVERFLOW);
    assert(unlink(page) == 0 && symlink("/etc/passwd", page) == 0);
    assert(rns_hosted_node_read(node, "/page/index.mu", output, sizeof output, &length) == RNS_ERROR_IO);
    assert(rns_hosted_node_announce(node, output, 129U) == RNS_ERROR_INVALID_ARGUMENT);
    rns_hosted_node_poll(node);
    rns_hosted_node_destroy(node);
    rns_runtime_destroy(runtime);
    assert(unlink(page) == 0 && unlink(linkpath) == 0);
    assert(unlink(nested) == 0 && unlink(fifo) == 0);
    assert(rmdir(directory) == 0 && rmdir(root) == 0);
    return 0;
}
