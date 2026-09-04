#include "reticulum/boards/heltec_shell_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now;
    uint32_t random;
    size_t handler_calls;
    size_t reports;
    rns_heltec_shell_status_t last_status;
    char output[256];
    char handled_argument[64];
} fake_shell_t;

static uint64_t monotonic_ms(void *context) {
    return ((fake_shell_t *)context)->now;
}

static uint32_t random_u32(void *context) {
    return ((fake_shell_t *)context)->random;
}

static void write_line(void *context, rns_heltec_shell_status_t status,
                       const char *message) {
    fake_shell_t *fake = context;
    fake->reports++;
    fake->last_status = status;
    (void)snprintf(fake->output, sizeof(fake->output), "%s", message);
}

static rns_heltec_shell_status_t handler(void *context,
                                         const rns_heltec_shell_args_t *args) {
    fake_shell_t *fake = context;
    fake->handler_calls++;
    if (args->argc > 1)
        (void)snprintf(fake->handled_argument, sizeof(fake->handled_argument),
                       "%s", args->argv[1]);
    return RNS_HELTEC_SHELL_OK;
}

static void feed_line(rns_heltec_shell_t *shell, const char *line) {
    while (*line != '\0') {
        assert(rns_heltec_shell_feed(shell, (uint8_t)*line) ==
               RNS_HELTEC_SHELL_OK);
        line++;
    }
}

int main(void) {
    fake_shell_t fake = {.now = 1000U, .random = 42U};
    rns_heltec_shell_ops_t ops = {
        .context = &fake,
        .monotonic_ms = monotonic_ms,
        .random_u32 = random_u32,
        .write_line = write_line
    };
    rns_heltec_shell_t shell;
    assert(rns_heltec_shell_init(&shell, &ops));

    rns_heltec_shell_command_t status_command = {
        .name = "status",
        .summary = "show bounded status",
        .policy = RNS_HELTEC_SHELL_COMMAND_NORMAL,
        .handler = handler,
        .context = &fake
    };
    rns_heltec_shell_command_t import_command = {
        .name = "identity-import",
        .summary = "replace identity",
        .policy = RNS_HELTEC_SHELL_COMMAND_GUARDED,
        .handler = handler,
        .context = &fake
    };
    assert(rns_heltec_shell_register(&shell, &status_command) ==
           RNS_HELTEC_SHELL_OK);
    rns_heltec_shell_command_t unsafe_import = import_command;
    unsafe_import.policy = RNS_HELTEC_SHELL_COMMAND_NORMAL;
    assert(rns_heltec_shell_register(&shell, &unsafe_import) ==
           RNS_HELTEC_SHELL_INVALID_ARGUMENT);
    assert(rns_heltec_shell_register(&shell, &import_command) ==
           RNS_HELTEC_SHELL_OK);
    assert(rns_heltec_shell_register(&shell, &status_command) ==
           RNS_HELTEC_SHELL_INVALID_ARGUMENT);

    assert(rns_heltec_shell_execute(&shell, "status radio", 12U) ==
           RNS_HELTEC_SHELL_OK);
    assert(fake.handler_calls == 1U);
    assert(strcmp(fake.handled_argument, "radio") == 0);

    const char *secret = "private-key-material-never-echoed";
    char guarded[96];
    int written = snprintf(guarded, sizeof(guarded), "identity-import %s", secret);
    assert(written > 0);
    assert(rns_heltec_shell_execute(&shell, guarded, (size_t)written) ==
           RNS_HELTEC_SHELL_CONFIRMATION_REQUIRED);
    assert(fake.handler_calls == 1U);
    assert(strstr(fake.output, secret) == NULL);
    assert(strstr(fake.output, "100042") != NULL);

    assert(rns_heltec_shell_execute(&shell, "confirm 999999", 14U) ==
           RNS_HELTEC_SHELL_CONFIRMATION_INVALID);
    assert(rns_heltec_shell_execute(&shell, "confirm 100042", 14U) ==
           RNS_HELTEC_SHELL_OK);
    assert(rns_heltec_shell_confirmation_pending(&shell));
    assert(rns_heltec_shell_execute(&shell, guarded, (size_t)written) ==
           RNS_HELTEC_SHELL_OK);
    assert(fake.handler_calls == 2U);
    assert(strcmp(fake.handled_argument, secret) == 0);
    assert(!rns_heltec_shell_confirmation_pending(&shell));

    assert(rns_heltec_shell_execute(&shell, guarded, (size_t)written) ==
           RNS_HELTEC_SHELL_CONFIRMATION_REQUIRED);
    assert(rns_heltec_shell_execute(&shell, "cancel", 6U) ==
           RNS_HELTEC_SHELL_CANCELLED);
    assert(!rns_heltec_shell_confirmation_pending(&shell));

    assert(rns_heltec_shell_execute(&shell, guarded, (size_t)written) ==
           RNS_HELTEC_SHELL_CONFIRMATION_REQUIRED);
    fake.now += RNS_HELTEC_SHELL_CONFIRM_MS + 1U;
    rns_heltec_shell_poll(&shell);
    assert(fake.last_status == RNS_HELTEC_SHELL_CONFIRMATION_EXPIRED);
    assert(!rns_heltec_shell_confirmation_pending(&shell));

    assert(rns_heltec_shell_execute(&shell, "missing", 7U) ==
           RNS_HELTEC_SHELL_UNKNOWN_COMMAND);
    static const char too_many_tokens[] =
        "status 1 2 3 4 5 6 7 8 9 10 11 12";
    assert(rns_heltec_shell_execute(&shell, too_many_tokens,
           sizeof(too_many_tokens) - 1U) ==
           RNS_HELTEC_SHELL_TOO_MANY_TOKENS);

    for (size_t index = 0U; index <= RNS_HELTEC_SHELL_LINE_MAX; index++)
        (void)rns_heltec_shell_feed(&shell, 'x');
    assert(shell.discarding_overflow);
    assert(rns_heltec_shell_feed(&shell, '\n') ==
           RNS_HELTEC_SHELL_LINE_TOO_LONG);
    assert(shell.line_length == 0U && !shell.discarding_overflow);

    feed_line(&shell, "status uart");
    assert(rns_heltec_shell_feed(&shell, '\r') == RNS_HELTEC_SHELL_OK);
    size_t calls_after_cr = fake.handler_calls;
    assert(rns_heltec_shell_feed(&shell, '\n') == RNS_HELTEC_SHELL_OK);
    assert(fake.handler_calls == calls_after_cr);
    assert(strcmp(fake.handled_argument, "uart") == 0);

    assert(rns_heltec_shell_execute(&shell, NULL, 1U) ==
           RNS_HELTEC_SHELL_INVALID_ARGUMENT);
    const char embedded_nul[] = {'s', 't', 'a', 't', 'u', 's', '\0', 'x'};
    assert(rns_heltec_shell_execute(&shell, embedded_nul, sizeof(embedded_nul)) ==
           RNS_HELTEC_SHELL_INVALID_ARGUMENT);
    return 0;
}
