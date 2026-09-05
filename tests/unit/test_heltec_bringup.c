/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bringup.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "reticulum/boards/heltec_status_ui_esp.h"
#include "reticulum/heltec_sx1262.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct rns_heltec_sx1262 { unsigned marker; };
struct rns_heltec_oled_esp { unsigned marker; };
static struct rns_heltec_sx1262 radio;
static struct rns_heltec_oled_esp display;
static rns_heltec_oled_t core;
static jmp_buf done;
static struct {
    bool display_ok, radio_ok, render_ok, endless_rx;
    unsigned loops, limit, receives, receives_this_loop, renders, opens;
    unsigned radio_opens, diagnostics, error_logs;
    rns_status_t stats_status;
    rns_sx1262_state_t stats_state;
    char state[64];
} fake;

bool rns_heltec_oled_esp_open(rns_heltec_oled_esp_t **out) {
    ++fake.opens;
    *out = fake.display_ok ? &display : NULL;
    return fake.display_ok;
}
rns_heltec_oled_t *rns_heltec_oled_esp_core(rns_heltec_oled_esp_t *handle) {
    assert(handle == &display);
    return &core;
}
void rns_heltec_oled_set_diagnostics(rns_heltec_oled_t *handle,
    const char *state, uint32_t heap, uint64_t rx, int16_t rssi,
    int16_t snr, bool signal_valid) {
    assert(handle == &core && heap == 100000U);
    bool valid_stats = fake.radio_ok && fake.stats_status == RNS_OK;
    assert(rx == (valid_stats ? 42U : 0U));
    assert(signal_valid == valid_stats);
    assert(rssi == (valid_stats ? -80 : 0));
    assert(snr == (valid_stats ? 7 : 0));
    (void)snprintf(fake.state, sizeof(fake.state), "%s", state);
    ++fake.diagnostics;
}
bool rns_heltec_oled_render(rns_heltec_oled_t *handle) {
    assert(handle == &core);
    ++fake.renders;
    return fake.render_ok;
}
void rns_sx1262_default_config(rns_sx1262_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->frequency_hz = 868200000U;
    config->bandwidth_hz = 125000U;
    config->spreading_factor = 8U;
    config->coding_rate_denominator = 5U;
    config->preamble_symbols = 18U;
}
rns_status_t rns_heltec_sx1262_open_with_config(
    const rns_sx1262_config_t *config, rns_heltec_sx1262_t **out) {
    assert(config->frequency_hz == 868100000U);
    assert(config->bandwidth_hz == 250000U);
    assert(config->spreading_factor == 11U);
    assert(config->coding_rate_denominator == 5U);
    assert(config->preamble_symbols == 18U);
    ++fake.radio_opens;
    *out = fake.radio_ok ? &radio : NULL;
    return fake.radio_ok ? RNS_OK : RNS_ERROR_IO;
}
rns_status_t rns_heltec_sx1262_receive(rns_heltec_sx1262_t *handle,
    rns_sx1262_packet_t *packet) {
    assert(handle == &radio && packet != NULL);
    ++fake.receives;
    ++fake.receives_this_loop;
    assert(fake.receives_this_loop <= RNS_SX1262_RX_QUEUE_CAPACITY);
    return fake.endless_rx ? RNS_OK : RNS_ERROR_TIMEOUT;
}
rns_status_t rns_heltec_sx1262_get_stats(rns_heltec_sx1262_t *handle,
    rns_sx1262_stats_t *stats) {
    assert(handle == &radio);
    if (fake.stats_status != RNS_OK) return fake.stats_status;
    memset(stats, 0, sizeof(*stats));
    stats->state = fake.stats_state;
    stats->rx_packets = 42U;
    stats->last_rssi_dbm = -80;
    stats->last_snr_db = 7;
    return RNS_OK;
}
/* No send API is linked: adding an RF transmission fails this test's link. */
size_t heap_caps_get_free_size(unsigned caps) {
    assert(caps == MALLOC_CAP_8BIT);
    return 100000U;
}
int64_t esp_timer_get_time(void) { return (int64_t)fake.loops * 50000; }
unsigned uxTaskGetStackHighWaterMark(void *task) {
    assert(task == NULL);
    return 3000U;
}
void test_bringup_log(const char *tag, const char *format, ...) {
    char message[512];
    va_list args;
    assert(tag != NULL);
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (strstr(message, "OLED failed") != NULL) ++fake.error_logs;
}
void vTaskDelay(TickType_t ticks) {
    assert(ticks == 50U);
    fake.receives_this_loop = 0U;
    if (++fake.loops == fake.limit) longjmp(done, 1);
}
static void run_case(bool display_ok, bool radio_ok, bool render_ok,
    bool endless_rx, rns_status_t stats_status, rns_sx1262_state_t stats_state,
    const char *expected_state) {
    memset(&fake, 0, sizeof(fake));
    fake.display_ok = display_ok;
    fake.radio_ok = radio_ok;
    fake.render_ok = render_ok;
    fake.endless_rx = endless_rx;
    fake.stats_status = stats_status;
    fake.stats_state = stats_state;
    fake.limit = 45U;
    if (setjmp(done) == 0) heltec_bringup_run();
    assert(fake.opens == 1U && fake.radio_opens == 1U);
    assert(fake.loops == fake.limit);
    assert(fake.receives == (radio_ok ? fake.limit *
        (endless_rx ? RNS_SX1262_RX_QUEUE_CAPACITY : 1U) : 0U));
    assert(fake.renders == (display_ok ? (render_ok ? 3U : 1U) : 0U));
    assert(fake.diagnostics == fake.renders);
    assert(fake.error_logs == (display_ok && !render_ok ? 1U : 0U));
    if (display_ok) assert(strcmp(fake.state, expected_state) == 0);
}
int main(void) {
    run_case(true, true, true, false, RNS_OK, RNS_SX1262_RECEIVING, "RX ONLY 868.100 SF11");
    run_case(false, true, true, true, RNS_OK, RNS_SX1262_RECEIVING, "RX ONLY 868.100 SF11");
    run_case(true, false, true, false, RNS_OK, RNS_SX1262_RECEIVING, "RADIO ERROR");
    run_case(true, true, false, true, RNS_OK, RNS_SX1262_RECEIVING, "RX ONLY 868.100 SF11");
    run_case(true, true, true, true, RNS_OK, RNS_SX1262_RECEIVING, "RX ONLY 868.100 SF11");
    run_case(true, true, true, false, RNS_ERROR_IO, RNS_SX1262_RECEIVING, "RADIO ERROR");
    run_case(true, true, true, false, RNS_OK, RNS_SX1262_FAULT, "RADIO FAULT");
    run_case(true, true, true, false, RNS_OK, RNS_SX1262_STOPPED, "RADIO STOPPED");
    run_case(true, true, true, false, RNS_OK, RNS_SX1262_SCANNING, "UNEXPECTED CAD");
    run_case(true, true, true, false, RNS_OK, RNS_SX1262_TRANSMITTING, "UNEXPECTED TX");
    puts("Heltec receive-only bringup tests passed");
    return 0;
}
