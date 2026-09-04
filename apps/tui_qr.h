#ifndef NOMAD_TUI_QR_H
#define NOMAD_TUI_QR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Version 2 QR (25 modules), four-module quiet border, paired terminal rows. */
#define TUI_QR_MODULES 33u
#define TUI_QR_ROWS 17u
#define TUI_QR_ROW_BYTES (TUI_QR_MODULES * 3u + 1u)
typedef struct {
    uint8_t dark[TUI_QR_MODULES][TUI_QR_MODULES];
    char rows[TUI_QR_ROWS][TUI_QR_ROW_BYTES];
} tui_qr_t;

/* Only a bounded, exactly 32-byte public hex address is accepted. */
bool tui_qr_address(const char *address, size_t length, tui_qr_t *output);
bool tui_qr_fits(int terminal_rows, int terminal_columns);
#endif
