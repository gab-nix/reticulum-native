#include "sx126x.h"
#include "sx126x_driver_version.h"
#include "sx126x_hal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned writes;
    const void *seen_context;
    uint8_t command[8];
    uint16_t command_length;
    uint16_t data_length;
} fake_radio_t;

sx126x_hal_status_t sx126x_hal_write(const void *context,
                                     const uint8_t *command,
                                     const uint16_t command_length,
                                     const uint8_t *data,
                                     const uint16_t data_length) {
    fake_radio_t *radio = (fake_radio_t *)context;

    if (radio == NULL || command == NULL || command_length > sizeof(radio->command)) {
        return SX126X_HAL_STATUS_ERROR;
    }
    if (data_length > 0U && data == NULL) {
        return SX126X_HAL_STATUS_ERROR;
    }
    radio->writes++;
    radio->seen_context = context;
    radio->command_length = command_length;
    radio->data_length = data_length;
    memcpy(radio->command, command, command_length);
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context,
                                    const uint8_t *command,
                                    const uint16_t command_length,
                                    uint8_t *data,
                                    const uint16_t data_length) {
    (void)context;
    (void)command;
    (void)command_length;
    (void)data;
    (void)data_length;
    return SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context) {
    (void)context;
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context) {
    (void)context;
    return SX126X_HAL_STATUS_OK;
}

static int expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void) {
    fake_radio_t radio = {0};
    unsigned before_invalid;

    if (!expect(strcmp(sx126x_driver_version_get_version_string(), "v2.5.0") == 0,
                "vendored driver version")) {
        return 1;
    }

    if (!expect(sx126x_set_standby(&radio, SX126X_STANDBY_CFG_XOSC) == SX126X_STATUS_OK,
                "standby command succeeds through HAL")) {
        return 1;
    }
    if (!expect(radio.writes == 1U && radio.seen_context == &radio,
                "driver forwards the opaque HAL context")) {
        return 1;
    }
    if (!expect(radio.command_length == 2U && radio.command[0] == 0x80U &&
                    radio.command[1] == SX126X_STANDBY_CFG_XOSC && radio.data_length == 0U,
                "driver emits the upstream SetStandby command bytes")) {
        return 1;
    }

    before_invalid = radio.writes;
    if (!expect(sx126x_set_tx(&radio, SX126X_MAX_TIMEOUT_IN_MS + 1U) ==
                    SX126X_STATUS_UNKNOWN_VALUE,
                "driver rejects an out-of-range transmit timeout")) {
        return 1;
    }
    if (!expect(radio.writes == before_invalid,
                "invalid timeout is rejected before reaching the HAL")) {
        return 1;
    }

    return 0;
}
