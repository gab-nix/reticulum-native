#include "tui_qr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    tui_qr_t qr;
    const char *address = "0123456789abcdef0123456789abcdef";
    assert(tui_qr_address(address, 32u, &qr));
    for (size_t y = 0; y < TUI_QR_MODULES; ++y)
        for (size_t x = 0; x < TUI_QR_MODULES; ++x)
            if (x < 4u || y < 4u || x >= 29u || y >= 29u) assert(qr.dark[y][x] == 0);
    for (size_t row = 0; row < TUI_QR_ROWS; ++row) {
        const unsigned char *p = (const unsigned char *)qr.rows[row];
        for (size_t x = 0; x < TUI_QR_MODULES; ++x) {
            unsigned bits = 0;
            if (*p == ' ') ++p;
            else {
                assert(p[0] == 0xe2 && p[1] == 0x96);
                bits = p[2] == 0x80 ? 1u : p[2] == 0x84 ? 2u : p[2] == 0x88 ? 3u : 4u;
                assert(bits < 4u); p += 3;
            }
            assert((bits & 1u) == qr.dark[row * 2u][x]);
            if (row * 2u + 1u < TUI_QR_MODULES) assert((bits >> 1u) == qr.dark[row * 2u + 1u][x]);
        }
        assert(*p == 0);
    }
    assert(tui_qr_fits(23, 40) && !tui_qr_fits(22, 40) && !tui_qr_fits(23, 39));
    if (argc == 2 && strcmp(argv[1], "--pbm") == 0) {
        printf("P1\n%u %u\n", TUI_QR_MODULES, TUI_QR_MODULES);
        for (size_t y = 0; y < TUI_QR_MODULES; ++y) {
            for (size_t x = 0; x < TUI_QR_MODULES; ++x) printf("%u ", qr.dark[y][x]);
            putchar('\n');
        }
    }
    assert(!tui_qr_address(address, 31u, &qr));
    assert(!tui_qr_address(address, 33u, &qr));
    assert(!tui_qr_address(NULL, 32u, &qr));
    assert(!tui_qr_address("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 32u, &qr));
    assert(!tui_qr_address(address, 32u, NULL));
    return 0;
}
