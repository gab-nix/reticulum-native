#define _POSIX_C_SOURCE 200809L

#include "reticulum/config.h"
#include "reticulum/hal.h"
#include "reticulum/runtime.h"
#include "reticulum/status.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file;
    long size;
    char *text;
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0L, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1U);
    if (text == NULL) { fclose(file); return NULL; }
    if (fread(text, 1U, (size_t)size, file) != (size_t)size) {
        free(text); fclose(file); return NULL;
    }
    text[size] = '\0';
    fclose(file);
    *length = (size_t)size;
    return text;
}

static const char *state_name(rns_runtime_interface_state_t state) {
    switch (state) {
        case RNS_RUNTIME_INTERFACE_DISABLED: return "disabled";
        case RNS_RUNTIME_INTERFACE_STARTING: return "starting";
        case RNS_RUNTIME_INTERFACE_UP: return "up";
        case RNS_RUNTIME_INTERFACE_DOWN: return "down";
        case RNS_RUNTIME_INTERFACE_UNSUPPORTED: return "unsupported";
        default: return "unknown";
    }
}

static void print_interfaces(const rns_runtime_t *runtime) {
    for (size_t i = 0U; i < rns_runtime_interface_count(runtime); ++i) {
        rns_runtime_interface_info_t info;
        if (rns_runtime_interface_info(runtime, i, &info) == RNS_OK) {
            fprintf(stderr, "rnsd: interface %s (%s): %s",
                    info.name, rns_config_interface_type_name(info.type), state_name(info.state));
            if (info.last_error != RNS_OK)
                fprintf(stderr, " (%s)", rns_status_string(info.last_error));
            fputc('\n', stderr);
        }
    }
}

static void usage(FILE *stream, const char *program) {
    fprintf(stream, "usage: %s [--check|--once] CONFIG\n", program);
}

int main(int argc, char **argv) {
    bool check = false, once = false;
    const char *path;
    char *text;
    size_t length;
    rns_config_t config;
    rns_config_diagnostic_t diagnostic = {0};
    rns_runtime_t *runtime = NULL;
    rns_status_t status;
    if (argc == 3 && strcmp(argv[1], "--check") == 0) check = true;
    else if (argc == 3 && strcmp(argv[1], "--once") == 0) once = true;
    else if (argc != 2) { usage(stderr, argv[0]); return 64; }
    path = argv[argc - 1];
    text = read_file(path, &length);
    if (text == NULL) { fprintf(stderr, "rnsd: cannot read %s: %s\n", path, strerror(errno)); return 66; }
    rns_config_init(&config);
    status = rns_config_parse(text, length, &config, &diagnostic);
    free(text);
    if (status != RNS_OK) {
        fprintf(stderr, "rnsd: %s:%zu: %s\n", path, diagnostic.line, diagnostic.message);
        return 65;
    }
    if (check) { puts("configuration valid"); return 0; }
    status = rns_runtime_create(&runtime, &config, NULL);
    if (status != RNS_OK) {
        fprintf(stderr, "rnsd: startup failed: %s\n", rns_status_string(status));
        return 69;
    }
    print_interfaces(runtime);
    if (signal(SIGINT, request_stop) == SIG_ERR || signal(SIGTERM, request_stop) == SIG_ERR) {
        fprintf(stderr, "rnsd: cannot install signal handlers\n");
        rns_runtime_destroy(runtime);
        return 71;
    }
    do {
        size_t processed = 0U;
        status = rns_runtime_poll(runtime, 32U, &processed);
        if (status != RNS_OK) {
            fprintf(stderr, "rnsd: interface error: %s\n", rns_status_string(status));
            if (config.panic_on_interface_error) break;
        }
        if (!once && processed == 0U) (void)rns_hal_sleep_ms(10U);
    } while (!once && !stop_requested);
    rns_runtime_destroy(runtime);
    return status == RNS_OK ? 0 : 69;
}
