/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/boards/heltec_status_ui.h"

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

static size_t utf8_sequence_length(uint8_t first) {
    if (first < 0x80U) return 1U;
    if (first >= 0xc2U && first <= 0xdfU) return 2U;
    if (first >= 0xe0U && first <= 0xefU) return 3U;
    if (first >= 0xf0U && first <= 0xf4U) return 4U;
    return 0U;
}

static bool utf8_sequence_valid(const uint8_t *input, size_t available,
                                size_t sequence_length) {
    if (sequence_length == 0U || available < sequence_length) return false;
    for (size_t index = 1U; index < sequence_length; index++) {
        if ((input[index] & 0xc0U) != 0x80U) return false;
    }
    if (sequence_length == 3U && input[0] == 0xe0U && input[1] < 0xa0U) return false;
    if (sequence_length == 3U && input[0] == 0xedU && input[1] >= 0xa0U) return false;
    if (sequence_length == 4U && input[0] == 0xf0U && input[1] < 0x90U) return false;
    if (sequence_length == 4U && input[0] == 0xf4U && input[1] >= 0x90U) return false;
    return true;
}

static bool copy_utf8_preview(char *output, size_t capacity,
                              const uint8_t *input, size_t length);

static void copy_text(char *output, size_t capacity, const char *input) {
    size_t length = 0U;
    if (capacity == 0U) return;
    if (input == NULL) {
        output[0] = '\0';
        return;
    }
    while (length + 1U < capacity && input[length] != '\0') length++;
    (void)copy_utf8_preview(output, capacity, (const uint8_t *)input, length);
}

static bool copy_utf8_preview(char *output, size_t capacity,
                              const uint8_t *input, size_t length) {
    size_t in_offset = 0U;
    size_t out_offset = 0U;
    bool valid = true;
    if (capacity == 0U) return false;
    while (in_offset < length && out_offset + 1U < capacity) {
        size_t sequence_length = utf8_sequence_length(input[in_offset]);
        if (!utf8_sequence_valid(input + in_offset, length - in_offset,
                                 sequence_length)) {
            output[out_offset++] = '?';
            in_offset++;
            valid = false;
            continue;
        }
        if (out_offset + sequence_length >= capacity) break;
        memcpy(output + out_offset, input + in_offset, sequence_length);
        out_offset += sequence_length;
        in_offset += sequence_length;
    }
    output[out_offset] = '\0';
    return valid;
}

static uint16_t glyph_bits(char character) {
    /* Compact 3x5 glyphs, rows packed from least-significant bits upward. */
    if (character >= 'a' && character <= 'z') character = (char)(character - 32);
    switch (character) {
        case 'A': return 0x17daU; case 'B': return 0x0f5fU;
        case 'C': return 0x1c47U; case 'D': return 0x0f6fU;
        case 'E': return 0x1d47U; case 'F': return 0x0547U;
        case 'G': return 0x1e4fU; case 'H': return 0x17daU;
        case 'I': return 0x1c97U; case 'J': return 0x0e92U;
        case 'K': return 0x15daU; case 'L': return 0x1c49U;
        case 'M': return 0x17fbU; case 'N': return 0x17ebU;
        case 'O': return 0x0f6fU; case 'P': return 0x05cfU;
        case 'Q': return 0x1f6fU; case 'R': return 0x15cfU;
        case 'S': return 0x1e1fU; case 'T': return 0x0497U;
        case 'U': return 0x0f69U; case 'V': return 0x0b69U;
        case 'W': return 0x1febU; case 'X': return 0x15b5U;
        case 'Y': return 0x0495U; case 'Z': return 0x1c3fU;
        case '0': return 0x1f6fU; case '1': return 0x049aU;
        case '2': return 0x1d37U; case '3': return 0x1e37U;
        case '4': return 0x04faU; case '5': return 0x1e5fU;
        case '6': return 0x1f5fU; case '7': return 0x0497U;
        case '8': return 0x1f7fU; case '9': return 0x1e7fU;
        case ':': return 0x0082U; case '-': return 0x00e0U;
        case '.': return 0x1000U; case '/': return 0x0124U;
        case ' ': return 0U; default: return 0x1555U;
    }
}

static const uint8_t *large_glyph(char character) {
    /* Seven rows of five pixels, most significant used bit at the left. */
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t blank[7] = {0};
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    if (character >= '0' && character <= '9') return digits[character - '0'];
    return character == '.' ? dot : blank;
}

static void draw_large_line(uint8_t *frame, size_t row, const char *text) {
    if (row >= 4U) return;
    for (size_t column = 0U; column < 10U && text[column] != '\0'; column++) {
        const uint8_t *glyph = large_glyph(text[column]);
        for (size_t y = 0U; y < 7U; y++) {
            for (size_t x = 0U; x < 5U; x++) {
                if ((glyph[y] & (1U << (4U - x))) == 0U) continue;
                for (size_t dy = 0U; dy < 2U; dy++) {
                    size_t pixel_y = row * 16U + y * 2U + dy;
                    for (size_t dx = 0U; dx < 2U; dx++) {
                        size_t pixel_x = column * 12U + x * 2U + dx;
                        frame[pixel_x + (pixel_y / 8U) * 128U] |=
                            (uint8_t)(1U << (pixel_y % 8U));
                    }
                }
            }
        }
    }
}

static void draw_character(uint8_t *frame, size_t column, size_t row,
                           char character) {
    uint16_t bits = glyph_bits(character);
    size_t x_base = column * 4U;
    size_t y_base = row * 6U;
    for (size_t y = 0U; y < 5U; y++) {
        for (size_t x = 0U; x < 3U; x++) {
            if ((bits & (uint16_t)(1U << (y * 3U + x))) != 0U) {
                size_t pixel_x = x_base + x;
                size_t pixel_y = y_base + y;
                frame[pixel_x + (pixel_y / 8U) * 128U] |=
                    (uint8_t)(1U << (pixel_y % 8U));
            }
        }
    }
}

static void draw_line(uint8_t *frame, size_t row, const char *text) {
    size_t column = 0U;
    size_t offset = 0U;
    while (text[offset] != '\0' && column < RNS_HELTEC_OLED_TEXT_COLUMNS) {
        uint8_t first = (uint8_t)text[offset];
        size_t sequence_length = utf8_sequence_length(first);
        if (sequence_length == 1U) draw_character(frame, column, row, text[offset]);
        else draw_character(frame, column, row, '?');
        if (sequence_length == 0U) sequence_length = 1U;
        offset += sequence_length;
        column++;
    }
}

static void draw_wrapped(uint8_t *frame, size_t first_row, size_t row_count,
                         const char *text) {
    size_t row = first_row;
    size_t column = 0U;
    size_t offset = 0U;
    while (text[offset] != '\0' && row < first_row + row_count) {
        uint8_t first = (uint8_t)text[offset];
        size_t sequence_length = utf8_sequence_length(first);
        draw_character(frame, column, row,
                       sequence_length == 1U ? text[offset] : '?');
        if (sequence_length == 0U) sequence_length = 1U;
        offset += sequence_length;
        column++;
        if (column >= RNS_HELTEC_OLED_TEXT_COLUMNS) {
            column = 0U;
            row++;
        }
    }
}

static bool send_command(rns_heltec_oled_t *oled, uint8_t command) {
    return oled->ops.write_command(oled->ops.context, &command, 1U);
}

void rns_heltec_oled_settings_default(rns_heltec_oled_settings_t *settings) {
    if (settings == NULL) return;
    settings->enabled = true;
    settings->preview_enabled = true;
    settings->brightness = 0x7fU;
    settings->preview_timeout_ms = 10000U;
    settings->screen = RNS_HELTEC_OLED_SCREEN_STATUS;
}

bool rns_heltec_oled_init(rns_heltec_oled_t *oled,
                          const rns_heltec_oled_ops_t *ops,
                          const rns_heltec_oled_settings_t *settings) {
    static const uint8_t init_commands[] = {
        0xaeU, 0xd5U, 0x80U, 0xa8U, 0x3fU, 0xd3U, 0x00U, 0x40U,
        0x8dU, 0x14U, 0x20U, 0x00U, 0xa1U, 0xc8U, 0xdaU, 0x12U,
        0x81U, 0x7fU, 0xd9U, 0xf1U, 0xdbU, 0x40U, 0xa4U, 0xa6U, 0xafU
    };
    if (oled == NULL || ops == NULL || settings == NULL ||
        ops->configure_bus == NULL ||
        ops->write_command == NULL || ops->write_data == NULL) return false;
    memset(oled, 0, sizeof(*oled));
    oled->ops = *ops;
    oled->settings = *settings;
    if (!rns_heltec_v3_1_prepare_oled(&oled->ops.gpio) ||
        !oled->ops.configure_bus(oled->ops.context,
                                RNS_HELTEC_V3_1_GPIO_OLED_SDA,
                                RNS_HELTEC_V3_1_GPIO_OLED_SCL,
                                RNS_HELTEC_V3_1_OLED_ADDRESS) ||
        !oled->ops.write_command(oled->ops.context, init_commands,
                                 sizeof(init_commands))) {
        oled->failed = true;
        return false;
    }
    oled->ready = true;
    oled->dirty = true;
    rns_heltec_oled_set_settings(oled, settings);
    return !oled->failed;
}

void rns_heltec_oled_set_settings(rns_heltec_oled_t *oled,
                                  const rns_heltec_oled_settings_t *settings) {
    if (oled == NULL || settings == NULL) return;
    oled->settings = *settings;
    if (!oled->settings.preview_enabled) {
        memset(oled->model.preview, 0, sizeof(oled->model.preview));
        oled->preview_deadline_ms = 0U;
    }
    if (oled->settings.screen > RNS_HELTEC_OLED_SCREEN_DIAGNOSTICS)
        oled->settings.screen = RNS_HELTEC_OLED_SCREEN_STATUS;
    oled->dirty = true;
    if (!oled->ready || oled->failed) return;
    uint8_t contrast[] = {0x81U, oled->settings.brightness};
    if (!oled->ops.write_command(oled->ops.context, contrast, sizeof(contrast)) ||
        !send_command(oled, oled->settings.enabled ? 0xafU : 0xaeU)) {
        oled->failed = true;
        oled->ready = false;
    }
}

void rns_heltec_oled_set_preview_enabled(rns_heltec_oled_t *oled,
                                         bool enabled) {
    if (oled == NULL) return;
    oled->settings.preview_enabled = enabled;
    if (!enabled) {
        memset(oled->model.preview, 0, sizeof(oled->model.preview));
        oled->preview_deadline_ms = 0U;
    }
    oled->dirty = true;
}

void rns_heltec_oled_set_status(rns_heltec_oled_t *oled,
                                const char *radio,
                                const char *address_suffix,
                                uint16_t peers,
                                uint16_t routes,
                                uint16_t unread) {
    if (oled == NULL) return;
    copy_text(oled->model.radio, sizeof(oled->model.radio), radio);
    copy_text(oled->model.address_suffix, sizeof(oled->model.address_suffix),
              address_suffix);
    oled->model.peer_count = peers;
    oled->model.route_count = routes;
    oled->model.unread_count = unread;
    oled->dirty = true;
}

void rns_heltec_oled_set_diagnostics(rns_heltec_oled_t *oled,
                                     const char *radio,
                                     uint32_t heap_free_bytes,
                                     uint64_t rx_packets,
                                     int16_t rssi_dbm,
                                     int16_t snr_db,
                                     bool signal_valid) {
    if (oled == NULL) return;
    copy_text(oled->model.radio, sizeof(oled->model.radio), radio);
    oled->model.heap_free_bytes = heap_free_bytes;
    oled->model.rx_packets = rx_packets;
    oled->model.rssi_dbm = rssi_dbm;
    oled->model.snr_db = snr_db;
    oled->model.signal_valid = signal_valid;
    oled->settings.screen = RNS_HELTEC_OLED_SCREEN_DIAGNOSTICS;
    oled->dirty = true;
}

bool rns_heltec_oled_show_preview(rns_heltec_oled_t *oled,
                                  const uint8_t *utf8,
                                  size_t length,
                                  uint64_t now_ms) {
    if (oled == NULL || (utf8 == NULL && length != 0U)) return false;
    if (!oled->settings.preview_enabled) {
        memset(oled->model.preview, 0, sizeof(oled->model.preview));
        oled->preview_deadline_ms = 0U;
        return false;
    }
    bool valid = copy_utf8_preview(oled->model.preview,
                                   sizeof(oled->model.preview), utf8, length);
    if (oled->settings.preview_timeout_ms == 0U)
        oled->preview_deadline_ms = 0U;
    else if (UINT64_MAX - now_ms < oled->settings.preview_timeout_ms)
        oled->preview_deadline_ms = UINT64_MAX;
    else
        oled->preview_deadline_ms = now_ms + oled->settings.preview_timeout_ms;
    oled->dirty = true;
    return valid;
}

void rns_heltec_oled_poll(rns_heltec_oled_t *oled, uint64_t now_ms) {
    if (oled == NULL || oled->preview_deadline_ms == 0U) return;
    if (now_ms >= oled->preview_deadline_ms) {
        oled->model.preview[0] = '\0';
        oled->preview_deadline_ms = 0U;
        oled->dirty = true;
    }
}

bool rns_heltec_oled_render(rns_heltec_oled_t *oled) {
    char line[32];
    if (oled == NULL || !oled->ready || oled->failed) return false;
    if (!oled->settings.enabled || !oled->dirty) return true;
    memset(oled->frame, 0, sizeof(oled->frame));
    if (oled->settings.screen != RNS_HELTEC_OLED_SCREEN_DIAGNOSTICS)
        draw_line(oled->frame, 0U, "RETICULUM");
    if (oled->settings.screen == RNS_HELTEC_OLED_SCREEN_MESSAGE) {
        draw_line(oled->frame, 1U, "MESSAGE PREVIEW");
        draw_wrapped(oled->frame, 2U, 8U, oled->model.preview);
    } else if (oled->settings.screen == RNS_HELTEC_OLED_SCREEN_DIAGNOSTICS) {
        const char *state = "NO RNS";
        if (strstr(oled->model.radio, "FAULT") != NULL) state = "FAULT";
        else if (strstr(oled->model.radio, "ERROR") != NULL) state = "ERROR";
        else if (strstr(oled->model.radio, "RX") != NULL) state = "RX ONLY";
        draw_large_line(oled->frame, 0U, state);
        draw_large_line(oled->frame, 1U,
                         strstr(oled->model.radio, "868.2") != NULL ? "868.2 SF8" : "NO RNS");
        uint64_t count = oled->model.rx_packets;
        if (count > UINT64_C(99999999)) count = UINT64_C(99999999);
        (void)snprintf(line, sizeof(line), "RX%" PRIu64, count);
        draw_large_line(oled->frame, 2U, line);
        uint32_t kib = oled->model.heap_free_bytes / 1024U;
        if (kib > 99999U) kib = 99999U;
        (void)snprintf(line, sizeof(line), "HEAP%" PRIu32 "K", kib);
        draw_large_line(oled->frame, 3U, line);
    } else if (oled->settings.screen == RNS_HELTEC_OLED_SCREEN_ROUTES) {
        draw_line(oled->frame, 1U, "NETWORK");
        (void)snprintf(line, sizeof(line), "PEERS %u",
                       (unsigned)oled->model.peer_count);
        draw_line(oled->frame, 3U, line);
        (void)snprintf(line, sizeof(line), "ROUTES %u",
                       (unsigned)oled->model.route_count);
        draw_line(oled->frame, 4U, line);
        draw_line(oled->frame, 6U, oled->model.radio);
    } else {
        draw_line(oled->frame, 1U, oled->model.radio);
        (void)snprintf(line, sizeof(line), "ID %.27s",
                       oled->model.address_suffix);
        draw_line(oled->frame, 2U, line);
        (void)snprintf(line, sizeof(line), "PEERS %u ROUTES %u",
                       (unsigned)oled->model.peer_count,
                       (unsigned)oled->model.route_count);
        draw_line(oled->frame, 3U, line);
        (void)snprintf(line, sizeof(line), "UNREAD %u",
                       (unsigned)oled->model.unread_count);
        draw_line(oled->frame, 4U, line);
        if (oled->model.preview[0] != '\0') {
            draw_line(oled->frame, 6U, "MESSAGE");
            draw_wrapped(oled->frame, 7U, 3U, oled->model.preview);
        }
    }
    static const uint8_t window[] = {0x21U, 0x00U, 0x7fU, 0x22U, 0x00U, 0x07U};
    if (!oled->ops.write_command(oled->ops.context, window, sizeof(window)) ||
        !oled->ops.write_data(oled->ops.context, oled->frame,
                              sizeof(oled->frame))) {
        oled->failed = true;
        oled->ready = false;
        return false;
    }
    oled->dirty = false;
    return true;
}

bool rns_heltec_oled_is_ready(const rns_heltec_oled_t *oled) {
    return oled != NULL && oled->ready && !oled->failed;
}

bool rns_heltec_oled_has_failed(const rns_heltec_oled_t *oled) {
    return oled != NULL && oled->failed;
}
