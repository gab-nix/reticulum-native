/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETICULUM_BOARDS_HELTEC_SHELL_CORE_H
#define RETICULUM_BOARDS_HELTEC_SHELL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RNS_HELTEC_SHELL_LINE_MAX = 160,
    RNS_HELTEC_SHELL_TOKEN_MAX = 12,
    RNS_HELTEC_SHELL_COMMAND_MAX = 24,
    RNS_HELTEC_SHELL_REGISTRY_MAX = 32,
    RNS_HELTEC_SHELL_CONFIRM_DIGEST_BYTES = 32,
    RNS_HELTEC_SHELL_CONFIRM_MS = 30000
};

typedef enum {
    RNS_HELTEC_SHELL_OK = 0,
    RNS_HELTEC_SHELL_UNKNOWN_COMMAND,
    RNS_HELTEC_SHELL_INVALID_ARGUMENT,
    RNS_HELTEC_SHELL_LINE_TOO_LONG,
    RNS_HELTEC_SHELL_TOO_MANY_TOKENS,
    RNS_HELTEC_SHELL_CONFIRMATION_REQUIRED,
    RNS_HELTEC_SHELL_CONFIRMATION_INVALID,
    RNS_HELTEC_SHELL_CONFIRMATION_EXPIRED,
    RNS_HELTEC_SHELL_CANCELLED,
    RNS_HELTEC_SHELL_REGISTRY_FULL,
    RNS_HELTEC_SHELL_HANDLER_FAILED
} rns_heltec_shell_status_t;

typedef enum {
    RNS_HELTEC_SHELL_COMMAND_NORMAL = 0,
    RNS_HELTEC_SHELL_COMMAND_GUARDED = 1
} rns_heltec_shell_command_policy_t;

typedef struct {
    /* argc/argv and every pointed-to token remain valid only for the duration
     * of the handler call. A handler retaining values must copy them. */
    int argc;
    const char *const *argv;
} rns_heltec_shell_args_t;

typedef rns_heltec_shell_status_t (*rns_heltec_shell_handler_t)(
    void *context, const rns_heltec_shell_args_t *args);

typedef struct {
    /* Name, summary and handler context remain owned by the caller and must
     * outlive the shell. The registry never copies or frees them. */
    const char *name;
    const char *summary;
    rns_heltec_shell_command_policy_t policy;
    rns_heltec_shell_handler_t handler;
    void *context;
} rns_heltec_shell_command_t;

typedef struct {
    void *context;
    uint64_t (*monotonic_ms)(void *context);
    /* Guarded commands are unavailable unless both random_u32 and sha256 are
     * supplied. The digest callback must accept the bounded invocation
     * serialization only for the duration of this call. */
    uint32_t (*random_u32)(void *context);
    bool (*sha256)(void *context, const uint8_t *data, size_t length,
                   uint8_t out[RNS_HELTEC_SHELL_CONFIRM_DIGEST_BYTES]);
    void (*write_line)(void *context, rns_heltec_shell_status_t status,
                       const char *message);
} rns_heltec_shell_ops_t;

typedef struct {
    rns_heltec_shell_ops_t ops;
    rns_heltec_shell_command_t commands[RNS_HELTEC_SHELL_REGISTRY_MAX];
    size_t command_count;
    char line[RNS_HELTEC_SHELL_LINE_MAX + 1];
    size_t line_length;
    bool discarding_overflow;
    bool last_was_cr;
    uint8_t pending_digest[RNS_HELTEC_SHELL_CONFIRM_DIGEST_BYTES];
    uint8_t armed_digest[RNS_HELTEC_SHELL_CONFIRM_DIGEST_BYTES];
    bool confirmation_pending;
    bool confirmation_armed;
    uint32_t confirmation_code;
    uint64_t confirmation_deadline_ms;
    uint64_t armed_deadline_ms;
} rns_heltec_shell_t;

bool rns_heltec_shell_init(rns_heltec_shell_t *shell,
                           const rns_heltec_shell_ops_t *ops);
rns_heltec_shell_status_t rns_heltec_shell_register(
    rns_heltec_shell_t *shell, const rns_heltec_shell_command_t *command);
rns_heltec_shell_status_t rns_heltec_shell_feed(rns_heltec_shell_t *shell,
                                                uint8_t byte);
rns_heltec_shell_status_t rns_heltec_shell_execute(rns_heltec_shell_t *shell,
                                                   const char *line,
                                                   size_t length);
void rns_heltec_shell_cancel(rns_heltec_shell_t *shell);
void rns_heltec_shell_poll(rns_heltec_shell_t *shell);
bool rns_heltec_shell_confirmation_pending(const rns_heltec_shell_t *shell);

#ifdef __cplusplus
}
#endif
#endif
