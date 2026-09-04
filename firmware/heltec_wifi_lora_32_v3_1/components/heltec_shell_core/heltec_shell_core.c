/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/boards/heltec_shell_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static uint64_t shell_now(const rns_heltec_shell_t *shell) {
    return shell->ops.monotonic_ms(shell->ops.context);
}

static uint64_t deadline_after(uint64_t now, uint64_t delay_ms) {
    return UINT64_MAX - now < delay_ms ? UINT64_MAX : now + delay_ms;
}

static void shell_report(rns_heltec_shell_t *shell,
                         rns_heltec_shell_status_t status,
                         const char *message) {
    if (shell->ops.write_line != NULL)
        shell->ops.write_line(shell->ops.context, status, message);
}

static void clear_confirmation(rns_heltec_shell_t *shell) {
    memset(shell->pending_command, 0, sizeof(shell->pending_command));
    memset(shell->armed_command, 0, sizeof(shell->armed_command));
    shell->confirmation_code = 0U;
    shell->confirmation_deadline_ms = 0U;
    shell->armed_deadline_ms = 0U;
}

static bool name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') return false;
    size_t length = 0U;
    while (name[length] != '\0') {
        unsigned char byte = (unsigned char)name[length];
        if (!(isalnum(byte) || byte == '-' || byte == '_') ||
            length >= RNS_HELTEC_SHELL_COMMAND_MAX) return false;
        length++;
    }
    return true;
}

static bool standard_guarded_name(const char *name) {
    return strcmp(name, "identity-import") == 0 ||
           strcmp(name, "identity-export") == 0 ||
           strcmp(name, "identity-erase") == 0 ||
           strcmp(name, "config-reset") == 0;
}

static const rns_heltec_shell_command_t *find_command(
    const rns_heltec_shell_t *shell, const char *name) {
    for (size_t index = 0U; index < shell->command_count; index++) {
        if (strcmp(shell->commands[index].name, name) == 0)
            return &shell->commands[index];
    }
    return NULL;
}

static uint32_t next_confirmation_code(rns_heltec_shell_t *shell) {
    uint32_t random_value;
    if (shell->ops.random_u32 != NULL)
        random_value = shell->ops.random_u32(shell->ops.context);
    else
        random_value = ++shell->fallback_nonce * 2654435761U;
    return 100000U + random_value % 900000U;
}

static rns_heltec_shell_status_t request_confirmation(
    rns_heltec_shell_t *shell, const char *name, uint64_t now) {
    char message[96];
    clear_confirmation(shell);
    (void)snprintf(shell->pending_command, sizeof(shell->pending_command), "%s", name);
    shell->confirmation_code = next_confirmation_code(shell);
    shell->confirmation_deadline_ms = deadline_after(now, RNS_HELTEC_SHELL_CONFIRM_MS);
    (void)snprintf(message, sizeof(message),
                   "guarded command; enter confirm %06lu, then re-enter command",
                   (unsigned long)shell->confirmation_code);
    shell_report(shell, RNS_HELTEC_SHELL_CONFIRMATION_REQUIRED, message);
    return RNS_HELTEC_SHELL_CONFIRMATION_REQUIRED;
}

static rns_heltec_shell_status_t execute_confirm(rns_heltec_shell_t *shell,
                                                 int argc,
                                                 const char *const *argv,
                                                 uint64_t now) {
    unsigned long supplied = 0UL;
    char trailing = '\0';
    if (argc != 2 || sscanf(argv[1], "%lu%c", &supplied, &trailing) != 1 ||
        supplied > UINT32_MAX) {
        shell_report(shell, RNS_HELTEC_SHELL_CONFIRMATION_INVALID,
                     "confirmation rejected");
        return RNS_HELTEC_SHELL_CONFIRMATION_INVALID;
    }
    if (shell->pending_command[0] == '\0') {
        shell_report(shell, RNS_HELTEC_SHELL_CONFIRMATION_INVALID,
                     "no guarded command is pending");
        return RNS_HELTEC_SHELL_CONFIRMATION_INVALID;
    }
    if (now > shell->confirmation_deadline_ms) {
        clear_confirmation(shell);
        shell_report(shell, RNS_HELTEC_SHELL_CONFIRMATION_EXPIRED,
                     "confirmation expired");
        return RNS_HELTEC_SHELL_CONFIRMATION_EXPIRED;
    }
    if ((uint32_t)supplied != shell->confirmation_code) {
        shell_report(shell, RNS_HELTEC_SHELL_CONFIRMATION_INVALID,
                     "confirmation rejected");
        return RNS_HELTEC_SHELL_CONFIRMATION_INVALID;
    }
    memcpy(shell->armed_command, shell->pending_command,
           sizeof(shell->armed_command));
    memset(shell->pending_command, 0, sizeof(shell->pending_command));
    shell->confirmation_code = 0U;
    shell->confirmation_deadline_ms = 0U;
    shell->armed_deadline_ms = deadline_after(now, RNS_HELTEC_SHELL_CONFIRM_MS);
    shell_report(shell, RNS_HELTEC_SHELL_OK,
                 "confirmation accepted; re-enter guarded command");
    return RNS_HELTEC_SHELL_OK;
}

bool rns_heltec_shell_init(rns_heltec_shell_t *shell,
                           const rns_heltec_shell_ops_t *ops) {
    if (shell == NULL || ops == NULL || ops->monotonic_ms == NULL) return false;
    memset(shell, 0, sizeof(*shell));
    shell->ops = *ops;
    shell->fallback_nonce = 1U;
    return true;
}

rns_heltec_shell_status_t rns_heltec_shell_register(
    rns_heltec_shell_t *shell, const rns_heltec_shell_command_t *command) {
    if (shell == NULL || command == NULL || !name_valid(command->name) ||
        command->summary == NULL || command->handler == NULL ||
        command->policy > RNS_HELTEC_SHELL_COMMAND_GUARDED)
        return RNS_HELTEC_SHELL_INVALID_ARGUMENT;
    if (standard_guarded_name(command->name) &&
        command->policy != RNS_HELTEC_SHELL_COMMAND_GUARDED)
        return RNS_HELTEC_SHELL_INVALID_ARGUMENT;
    if (find_command(shell, command->name) != NULL)
        return RNS_HELTEC_SHELL_INVALID_ARGUMENT;
    if (shell->command_count >= RNS_HELTEC_SHELL_REGISTRY_MAX)
        return RNS_HELTEC_SHELL_REGISTRY_FULL;
    shell->commands[shell->command_count++] = *command;
    return RNS_HELTEC_SHELL_OK;
}

rns_heltec_shell_status_t rns_heltec_shell_execute(rns_heltec_shell_t *shell,
                                                   const char *line,
                                                   size_t length) {
    char buffer[RNS_HELTEC_SHELL_LINE_MAX + 1];
    const char *argv[RNS_HELTEC_SHELL_TOKEN_MAX];
    int argc = 0;
    uint64_t now;
    if (shell == NULL || (line == NULL && length != 0U))
        return RNS_HELTEC_SHELL_INVALID_ARGUMENT;
    if (length > RNS_HELTEC_SHELL_LINE_MAX) {
        shell_report(shell, RNS_HELTEC_SHELL_LINE_TOO_LONG, "line too long");
        return RNS_HELTEC_SHELL_LINE_TOO_LONG;
    }
    for (size_t index = 0U; index < length; index++) {
        if (line[index] == '\0') {
            shell_report(shell, RNS_HELTEC_SHELL_INVALID_ARGUMENT,
                         "command contains an invalid byte");
            return RNS_HELTEC_SHELL_INVALID_ARGUMENT;
        }
    }
    if (length > 0U) memcpy(buffer, line, length);
    buffer[length] = '\0';
    char *cursor = buffer;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0') break;
        if (argc >= RNS_HELTEC_SHELL_TOKEN_MAX) {
            shell_report(shell, RNS_HELTEC_SHELL_TOO_MANY_TOKENS,
                         "too many command arguments");
            return RNS_HELTEC_SHELL_TOO_MANY_TOKENS;
        }
        argv[argc++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) cursor++;
        if (*cursor != '\0') *cursor++ = '\0';
    }
    if (argc == 0) return RNS_HELTEC_SHELL_OK;
    now = shell_now(shell);
    if (strcmp(argv[0], "cancel") == 0) {
        clear_confirmation(shell);
        shell_report(shell, RNS_HELTEC_SHELL_CANCELLED, "guarded command cancelled");
        return RNS_HELTEC_SHELL_CANCELLED;
    }
    if (strcmp(argv[0], "confirm") == 0)
        return execute_confirm(shell, argc, argv, now);

    const rns_heltec_shell_command_t *command = find_command(shell, argv[0]);
    if (command == NULL) {
        shell_report(shell, RNS_HELTEC_SHELL_UNKNOWN_COMMAND, "unknown command");
        return RNS_HELTEC_SHELL_UNKNOWN_COMMAND;
    }
    if (command->policy == RNS_HELTEC_SHELL_COMMAND_GUARDED) {
        bool armed = strcmp(shell->armed_command, command->name) == 0 &&
                     now <= shell->armed_deadline_ms;
        if (!armed) return request_confirmation(shell, command->name, now);
        clear_confirmation(shell);
    }
    rns_heltec_shell_args_t args = {.argc = argc, .argv = argv};
    rns_heltec_shell_status_t status = command->handler(command->context, &args);
    if (status != RNS_HELTEC_SHELL_OK)
        shell_report(shell, status, "command failed");
    return status;
}

rns_heltec_shell_status_t rns_heltec_shell_feed(rns_heltec_shell_t *shell,
                                                uint8_t byte) {
    if (shell == NULL) return RNS_HELTEC_SHELL_INVALID_ARGUMENT;
    if (byte == '\n' && shell->last_was_cr) {
        shell->last_was_cr = false;
        return RNS_HELTEC_SHELL_OK;
    }
    shell->last_was_cr = byte == '\r';
    if (byte == '\r' || byte == '\n') {
        rns_heltec_shell_status_t status;
        if (shell->discarding_overflow) {
            shell->discarding_overflow = false;
            shell->line_length = 0U;
            shell_report(shell, RNS_HELTEC_SHELL_LINE_TOO_LONG, "line too long");
            return RNS_HELTEC_SHELL_LINE_TOO_LONG;
        }
        status = rns_heltec_shell_execute(shell, shell->line, shell->line_length);
        memset(shell->line, 0, sizeof(shell->line));
        shell->line_length = 0U;
        return status;
    }
    if (byte == 0x08U || byte == 0x7fU) {
        if (!shell->discarding_overflow && shell->line_length > 0U)
            shell->line[--shell->line_length] = '\0';
        return RNS_HELTEC_SHELL_OK;
    }
    if (byte < 0x20U) return RNS_HELTEC_SHELL_OK;
    if (shell->discarding_overflow) return RNS_HELTEC_SHELL_OK;
    if (shell->line_length >= RNS_HELTEC_SHELL_LINE_MAX) {
        memset(shell->line, 0, sizeof(shell->line));
        shell->line_length = 0U;
        shell->discarding_overflow = true;
        return RNS_HELTEC_SHELL_LINE_TOO_LONG;
    }
    shell->line[shell->line_length++] = (char)byte;
    shell->line[shell->line_length] = '\0';
    return RNS_HELTEC_SHELL_OK;
}

void rns_heltec_shell_cancel(rns_heltec_shell_t *shell) {
    if (shell == NULL) return;
    clear_confirmation(shell);
}

void rns_heltec_shell_poll(rns_heltec_shell_t *shell) {
    uint64_t now;
    if (shell == NULL) return;
    now = shell_now(shell);
    if ((shell->pending_command[0] != '\0' &&
         now > shell->confirmation_deadline_ms) ||
        (shell->armed_command[0] != '\0' && now > shell->armed_deadline_ms)) {
        clear_confirmation(shell);
        shell_report(shell, RNS_HELTEC_SHELL_CONFIRMATION_EXPIRED,
                     "confirmation expired");
    }
}

bool rns_heltec_shell_confirmation_pending(const rns_heltec_shell_t *shell) {
    return shell != NULL && (shell->pending_command[0] != '\0' ||
                             shell->armed_command[0] != '\0');
}
