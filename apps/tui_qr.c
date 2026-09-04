#include "tui_qr.h"
#include "qrcodegen.h"
#include <string.h>

bool tui_qr_address(const char *address, size_t length, tui_qr_t *output) {
    if (output == NULL) return false;
    memset(output, 0, sizeof *output);
    if (address == NULL || length != 32u) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((address[i] >= '0' && address[i] <= '9') ||
              (address[i] >= 'a' && address[i] <= 'f') ||
              (address[i] >= 'A' && address[i] <= 'F'))) return false;
    uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(2)];
    uint8_t encoded[qrcodegen_BUFFER_LEN_FOR_VERSION(2)];
    memcpy(temporary, address, length);
    if (!qrcodegen_encodeBinary(temporary, length, encoded, qrcodegen_Ecc_LOW,
        2, 2, qrcodegen_Mask_AUTO, false)) return false;
    for (size_t y = 4; y < 29; ++y)
        for (size_t x = 4; x < 29; ++x)
            output->dark[y][x] = (uint8_t)qrcodegen_getModule(encoded, (int)x - 4, (int)y - 4);
    const char *const glyphs[4] = {" ", "\xe2\x96\x80", "\xe2\x96\x84", "\xe2\x96\x88"};
    for (size_t row = 0; row < TUI_QR_ROWS; ++row) {
        size_t used = 0;
        for (size_t x = 0; x < TUI_QR_MODULES; ++x) {
            unsigned code = output->dark[row * 2u][x];
            if (row * 2u + 1u < TUI_QR_MODULES)
                code |= (unsigned)output->dark[row * 2u + 1u][x] << 1u;
            size_t bytes = strlen(glyphs[code]);
            memcpy(output->rows[row] + used, glyphs[code], bytes); used += bytes;
        }
        output->rows[row][used] = '\0';
    }
    return true;
}

bool tui_qr_fits(int rows, int columns) { return rows >= 23 && columns >= 40; }
