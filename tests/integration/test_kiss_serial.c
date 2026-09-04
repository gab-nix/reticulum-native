#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "reticulum/kiss.h"
#include "reticulum/packet.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

typedef struct capture {
    uint8_t bytes[64];
    size_t length;
    size_t count;
} capture_t;

static double fake_now;

static double fake_clock(void *context) {
    (void)context;
    return fake_now;
}

static rns_status_t capture_frame(const uint8_t *frame, size_t length,
                                  void *context) {
    capture_t *capture = context;
    assert(length <= sizeof(capture->bytes));
    memcpy(capture->bytes, frame, length);
    capture->length = length;
    capture->count++;
    return RNS_OK;
}

static int new_pty(char path[128]) {
    int master = -1;
    int slave = -1;
    char name[128];
    assert(openpty(&master, &slave, name, NULL, NULL) == 0);
    size_t length = strnlen(name, sizeof(name));
    assert(length > 0U && length < 128U);
    memcpy(path, name, length + 1U);
    int flags = fcntl(master, F_GETFL, 0);
    assert(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(close(slave) == 0);
    return master;
}

static size_t drain_bytes(int descriptor, uint8_t *output, size_t capacity) {
    size_t total = 0U;
    while (total < capacity) {
        ssize_t count = read(descriptor, output + total, capacity - total);
        if (count > 0) total += (size_t)count;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else break;
    }
    return total;
}

static void poll_ok(rns_kiss_endpoint_t *endpoint, capture_t *capture) {
    size_t processed = 0U;
    assert(rns_kiss_endpoint_poll(endpoint, 8U, capture_frame, capture,
                                  &processed) == RNS_OK);
}

int main(void) {
    char first_path[128];
    char second_path[128];
    char directory[] = "/tmp/rns-kiss-XXXXXX";
    char link_path[256];
    uint8_t wire[1024];
    static const uint8_t first[] = {0x01U, RNS_KISS_FEND, RNS_KISS_FESC};
    static const uint8_t second[] = {0x44U};
    int first_master = new_pty(first_path);
    assert(mkdtemp(directory) != NULL);
    int written = snprintf(link_path, sizeof(link_path), "%s/device", directory);
    assert(written > 0 && (size_t)written < sizeof(link_path));
    assert(symlink(first_path, link_path) == 0);

    rns_kiss_options_t options;
    rns_kiss_options_init(&options);
    options.device = link_path;
    options.flow_control = true;
    options.configure_delay_seconds = 0.0;
    options.reconnect_seconds = 1.0;
    options.clock = fake_clock;
    rns_kiss_endpoint_t *endpoint = NULL;
    assert(rns_kiss_endpoint_create(&endpoint, &options) == RNS_OK);
    capture_t capture = {{0U}, 0U, 0U};
    poll_ok(endpoint, &capture);
    size_t startup = drain_bytes(first_master, wire, sizeof(wire));
    assert(startup >= 20U);

    static const uint8_t ready[] = {RNS_KISS_FEND, RNS_KISS_READY_COMMAND,
                                    RNS_KISS_FEND};
    assert(rns_kiss_endpoint_send(endpoint, first, sizeof(first)) == RNS_OK);
    assert(rns_kiss_endpoint_send(endpoint, second, sizeof(second)) == RNS_OK);
    poll_ok(endpoint, &capture);
    size_t first_wire = drain_bytes(first_master, wire, sizeof(wire));
    assert(first_wire >= 8U);
    poll_ok(endpoint, &capture);
    assert(drain_bytes(first_master, wire, sizeof(wire)) == 0U);

    assert(write(first_master, ready, 1U) == 1);
    poll_ok(endpoint, &capture);
    assert(write(first_master, ready + 1U, sizeof(ready) - 1U) ==
           (ssize_t)(sizeof(ready) - 1U));
    poll_ok(endpoint, &capture);
    assert(drain_bytes(first_master, wire, sizeof(wire)) >= 4U);

    /* Saturate the slave-to-master queue so endpoint writes must retain their
     * exact offset across non-blocking backpressure. */
    assert(write(first_master, ready, sizeof(ready)) == (ssize_t)sizeof(ready));
    poll_ok(endpoint, &capture);
    int filler = open(first_path, O_WRONLY | O_NONBLOCK | O_NOCTTY);
    assert(filler >= 0);
    uint8_t fill[4096];
    memset(fill, 0x7f, sizeof(fill));
    while (write(filler, fill, sizeof(fill)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(close(filler) == 0);
    uint8_t large[RNS_MTU];
    memset(large, 0x55, sizeof(large));
    assert(rns_kiss_endpoint_send(endpoint, large, sizeof(large)) == RNS_OK);
    poll_ok(endpoint, &capture);
    rns_kiss_info_t info;
    assert(rns_kiss_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.pending_packets <= 1U);
    size_t backpressure_wire = 0U;
    size_t drained = 0U;
    while ((drained = drain_bytes(first_master, wire, sizeof(wire))) != 0U)
        backpressure_wire += drained;
    for (size_t attempt = 0U; attempt < 8U && info.pending_packets != 0U;
         ++attempt) {
        poll_ok(endpoint, &capture);
        assert(rns_kiss_endpoint_info(endpoint, &info) == RNS_OK);
    }
    assert(info.pending_packets == 0U);
    assert(info.packets_sent == 3U);
    backpressure_wire += drain_bytes(first_master, wire, sizeof(wire));
    assert(backpressure_wire > 0U);

    static const uint8_t malformed[] = {
        RNS_KISS_FEND, 0U, RNS_KISS_FESC, 0x01U, RNS_KISS_FEND
    };
    static const uint8_t incoming[] = {
        RNS_KISS_FEND, 0U, 0x10U, RNS_KISS_FESC, RNS_KISS_TFEND,
        RNS_KISS_FEND
    };
    assert(write(first_master, malformed, sizeof(malformed)) ==
           (ssize_t)sizeof(malformed));
    assert(write(first_master, incoming, 3U) == 3);
    poll_ok(endpoint, &capture);
    assert(capture.count == 0U);
    assert(write(first_master, incoming + 3U, sizeof(incoming) - 3U) ==
           (ssize_t)(sizeof(incoming) - 3U));
    poll_ok(endpoint, &capture);
    assert(capture.count == 1U && capture.length == 2U);
    assert(capture.bytes[0] == 0x10U && capture.bytes[1] == RNS_KISS_FEND);
    assert(rns_kiss_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.malformed_frames == 1U && info.packets_received == 1U);

    assert(close(first_master) == 0);
    size_t processed = 0U;
    assert(rns_kiss_endpoint_poll(endpoint, 8U, capture_frame, &capture,
                                  &processed) == RNS_ERROR_IO);
    assert(rns_kiss_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.state == RNS_KISS_DOWN && info.connections_lost == 1U);

    int second_master = new_pty(second_path);
    assert(unlink(link_path) == 0);
    assert(symlink(second_path, link_path) == 0);
    fake_now += 1.0;
    poll_ok(endpoint, &capture);
    assert(rns_kiss_endpoint_info(endpoint, &info) == RNS_OK);
    assert(info.state == RNS_KISS_UP && info.connections_established == 2U);
    assert(drain_bytes(second_master, wire, sizeof(wire)) >= 20U);

    rns_kiss_endpoint_destroy(endpoint);
    assert(close(second_master) == 0);
    assert(unlink(link_path) == 0);
    assert(rmdir(directory) == 0);
    return 0;
}
