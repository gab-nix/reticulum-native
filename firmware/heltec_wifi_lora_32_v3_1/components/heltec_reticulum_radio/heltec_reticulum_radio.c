/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/boards/heltec_reticulum_radio.h"

#include <string.h>

#include "reticulum/hal.h"

typedef enum pending_operation {
    PENDING_NONE = 0,
    PENDING_CAD,
    PENDING_TX
} pending_operation_t;

typedef struct heltec_radio_adapter {
    const rns_platform_ops_t *platform;
    const rns_heltec_reticulum_radio_backend_ops_t *backend_ops;
    void *backend_context;
    rns_sx1262_interface_t *scheduler;
    rns_sx1262_config_t radio_config;
    rns_sx1262_packet_t rx_cache;
    uint32_t scheduler_token;
    uint32_t lower_tx_id;
    pending_operation_t pending;
    bool backend_started;
} heltec_radio_adapter_t;

static bool valid_backend(
    const rns_heltec_reticulum_radio_backend_ops_t *ops) {
    return ops != NULL && ops->abort_and_restart != NULL &&
           ops->start_cad != NULL && ops->receive_cad_result != NULL &&
           ops->send_with_id != NULL && ops->receive_tx_result != NULL &&
           ops->receive != NULL && ops->stop != NULL && ops->destroy != NULL;
}

static bool valid_radio_config(
    const rns_heltec_reticulum_radio_config_t *config) {
    return config != NULL && config->scheduler.explicit_header &&
           config->frequency_hz > 425000000U &&
           config->frequency_hz <= 960000000U &&
           config->tx_power_dbm >= -9 && config->tx_power_dbm <= 22 &&
           config->busy_timeout_us != 0U &&
           config->recovery_backoff_polls != 0U;
}

static rns_sx1262_config_t translate_config(
    const rns_heltec_reticulum_radio_config_t *config) {
    return (rns_sx1262_config_t){
        .frequency_hz = config->frequency_hz,
        .bandwidth_hz = config->scheduler.bandwidth_hz,
        .spreading_factor = config->scheduler.spreading_factor,
        .coding_rate_denominator =
            config->scheduler.coding_rate_denominator,
        .preamble_symbols = config->scheduler.preamble_symbols,
        .crc_enabled = config->scheduler.crc_enabled,
        .invert_iq = config->invert_iq,
        .tx_power_dbm = config->tx_power_dbm,
        .busy_timeout_us = config->busy_timeout_us,
        .tx_timeout_margin_ms = config->scheduler.tx_timeout_margin_ms,
        .recovery_backoff_polls = config->recovery_backoff_polls};
}

void rns_heltec_reticulum_radio_default_config(
    rns_heltec_reticulum_radio_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    rns_sx1262_scheduler_default_config(&config->scheduler);
    config->frequency_hz = 868200000U;
    config->tx_power_dbm = 14;
    config->invert_iq = false;
    config->busy_timeout_us = 100000U;
    config->recovery_backoff_polls = 5U;
}

static void clear_pending(heltec_radio_adapter_t *adapter) {
    adapter->pending = PENDING_NONE;
    adapter->scheduler_token = 0U;
    adapter->lower_tx_id = 0U;
}

static rns_status_t phy_start(
    void *context, const rns_sx1262_scheduler_config_t *config) {
    heltec_radio_adapter_t *adapter = context;
    rns_sx1262_config_t translated;
    rns_status_t status;
    if (adapter == NULL || config == NULL || !config->explicit_header) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    translated = adapter->radio_config;
    translated.bandwidth_hz = config->bandwidth_hz;
    translated.spreading_factor = config->spreading_factor;
    translated.coding_rate_denominator = config->coding_rate_denominator;
    translated.preamble_symbols = config->preamble_symbols;
    translated.crc_enabled = config->crc_enabled;
    translated.tx_timeout_margin_ms = config->tx_timeout_margin_ms;
    clear_pending(adapter);
    memset(&adapter->rx_cache, 0, sizeof(adapter->rx_cache));
    status = adapter->backend_ops->abort_and_restart(adapter->backend_context,
                                                     &translated);
    if (status == RNS_OK) {
        adapter->radio_config = translated;
        adapter->backend_started = true;
    }
    return status;
}

static void discard_stale_cad(heltec_radio_adapter_t *adapter) {
    rns_sx1262_cad_result_t ignored;
    while (adapter->backend_ops->receive_cad_result(adapter->backend_context,
                                                    &ignored) == RNS_OK) {
    }
}

static void discard_stale_tx(heltec_radio_adapter_t *adapter) {
    rns_sx1262_tx_result_t ignored;
    while (adapter->backend_ops->receive_tx_result(adapter->backend_context,
                                                   &ignored) == RNS_OK) {
    }
}

static rns_status_t phy_start_cad(
    void *context, const rns_sx1262_scheduler_config_t *config,
    uint32_t operation_token) {
    heltec_radio_adapter_t *adapter = context;
    rns_status_t status;
    if (adapter == NULL || config == NULL || !config->explicit_header ||
        operation_token == 0U || !adapter->backend_started ||
        adapter->pending != PENDING_NONE) {
        return RNS_ERROR_INVALID_STATE;
    }
    discard_stale_cad(adapter);
    status = adapter->backend_ops->start_cad(adapter->backend_context);
    if (status == RNS_OK) {
        adapter->pending = PENDING_CAD;
        adapter->scheduler_token = operation_token;
    }
    return status;
}

static rns_status_t phy_transmit(
    void *context, const rns_sx1262_scheduler_config_t *config,
    const uint8_t *frame, size_t frame_length, uint32_t operation_token) {
    heltec_radio_adapter_t *adapter = context;
    uint32_t lower_id = 0U;
    rns_status_t status;
    if (adapter == NULL || config == NULL || !config->explicit_header ||
        frame == NULL || frame_length == 0U ||
        frame_length > RNS_SX1262_MAX_PAYLOAD || operation_token == 0U ||
        !adapter->backend_started || adapter->pending != PENDING_NONE) {
        return RNS_ERROR_INVALID_STATE;
    }
    discard_stale_tx(adapter);
    status = adapter->backend_ops->send_with_id(
        adapter->backend_context, frame, frame_length, &lower_id);
    if (status == RNS_OK && lower_id == 0U) {
        return RNS_ERROR_PROTOCOL;
    }
    if (status == RNS_OK) {
        adapter->pending = PENDING_TX;
        adapter->scheduler_token = operation_token;
        adapter->lower_tx_id = lower_id;
    }
    return status;
}

static rns_status_t phy_poll_event(void *context,
                                   rns_sx1262_phy_event_t *event) {
    heltec_radio_adapter_t *adapter = context;
    rns_sx1262_cad_result_t cad;
    rns_sx1262_tx_result_t tx;
    rns_status_t status;
    if (adapter == NULL || event == NULL || !adapter->backend_started) {
        return RNS_ERROR_INVALID_STATE;
    }
    memset(event, 0, sizeof(*event));

    /* Terminal control results are drained before RX so a sustained receive
       stream cannot starve scheduler deadlines or frame-two transmission. */
    status = adapter->backend_ops->receive_cad_result(adapter->backend_context,
                                                      &cad);
    if (status == RNS_OK) {
        if (adapter->pending == PENDING_CAD) {
            event->operation_token = adapter->scheduler_token;
            event->status = cad.status;
            event->type = cad.outcome == RNS_SX1262_CAD_CLEAR
                              ? RNS_SX1262_PHY_EVENT_CAD_CLEAR
                          : cad.outcome == RNS_SX1262_CAD_BUSY
                              ? RNS_SX1262_PHY_EVENT_CAD_BUSY
                              : RNS_SX1262_PHY_EVENT_CAD_FAILED;
            clear_pending(adapter);
            return RNS_OK;
        }
    } else if (status != RNS_ERROR_NOT_FOUND) {
        return status;
    }

    while ((status = adapter->backend_ops->receive_tx_result(
                adapter->backend_context, &tx)) == RNS_OK) {
        if (adapter->pending != PENDING_TX ||
            tx.id != adapter->lower_tx_id) {
            continue;
        }
        event->operation_token = adapter->scheduler_token;
        event->status = tx.status;
        event->type = tx.outcome == RNS_SX1262_TX_SENT
                          ? RNS_SX1262_PHY_EVENT_TX_DONE
                          : RNS_SX1262_PHY_EVENT_TX_FAILED;
        clear_pending(adapter);
        return RNS_OK;
    }
    if (status != RNS_ERROR_NOT_FOUND) {
        return status;
    }

    /* RX storage is owned by the adapter and remains valid until the next
       poll_event call, which is the scheduler's documented lifetime. */
    memset(&adapter->rx_cache, 0, sizeof(adapter->rx_cache));
    status = adapter->backend_ops->receive(adapter->backend_context,
                                           &adapter->rx_cache);
    if (status != RNS_OK) {
        return status;
    }
    if (adapter->rx_cache.length == 0U ||
        adapter->rx_cache.length > RNS_SX1262_MAX_PAYLOAD) {
        return RNS_ERROR_PROTOCOL;
    }
    event->type = RNS_SX1262_PHY_EVENT_RX_FRAME;
    event->frame = adapter->rx_cache.data;
    event->frame_length = adapter->rx_cache.length;
    event->rssi_dbm = adapter->rx_cache.rssi_dbm;
    event->snr_db = adapter->rx_cache.snr_db;
    event->status = RNS_OK;
    return RNS_OK;
}

static rns_status_t phy_cancel(
    void *context, const rns_sx1262_scheduler_config_t *config,
    uint32_t operation_token) {
    heltec_radio_adapter_t *adapter = context;
    rns_status_t status;
    if (adapter == NULL || config == NULL || !config->explicit_header ||
        operation_token == 0U || adapter->pending == PENDING_NONE ||
        operation_token != adapter->scheduler_token) {
        return RNS_ERROR_INVALID_STATE;
    }
    clear_pending(adapter);
    status = adapter->backend_ops->abort_and_restart(
        adapter->backend_context, &adapter->radio_config);
    adapter->backend_started = status == RNS_OK;
    return status;
}

static rns_status_t phy_stop(void *context) {
    heltec_radio_adapter_t *adapter = context;
    rns_status_t status;
    if (adapter == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    clear_pending(adapter);
    memset(&adapter->rx_cache, 0, sizeof(adapter->rx_cache));
    status = adapter->backend_started
                 ? adapter->backend_ops->stop(adapter->backend_context)
                 : RNS_OK;
    adapter->backend_started = false;
    return status;
}

static const rns_sx1262_phy_ops_t PHY_OPS = {
    .start = phy_start,
    .poll_event = phy_poll_event,
    .start_cad = phy_start_cad,
    .transmit = phy_transmit,
    .cancel_operation = phy_cancel,
    .stop = phy_stop};

static rns_status_t interface_start(void *context) {
    heltec_radio_adapter_t *adapter = context;
    return rns_sx1262_interface_start(adapter->scheduler);
}

static rns_status_t interface_poll(void *context,
                                   rns_interface_receive_fn receive,
                                   void *receive_context, size_t budget) {
    heltec_radio_adapter_t *adapter = context;
    return rns_sx1262_interface_poll(adapter->scheduler, receive,
                                     receive_context, budget);
}

static rns_status_t interface_send(void *context, const uint8_t *packet,
                                   size_t packet_length) {
    heltec_radio_adapter_t *adapter = context;
    uint32_t ignored_id;
    return rns_sx1262_interface_send(adapter->scheduler, packet, packet_length,
                                     &ignored_id);
}

static rns_status_t interface_send_with_id(void *context, const uint8_t *packet,
    size_t length, uint32_t *id) {
    heltec_radio_adapter_t *adapter=context;
    return rns_sx1262_interface_send(adapter->scheduler,packet,length,id);
}
static rns_status_t interface_get_stats(void *context,
                                        rns_interface_stats_t *stats) {
    heltec_radio_adapter_t *adapter = context;
    rns_sx1262_scheduler_stats_t scheduler_stats;
    rns_status_t status = rns_sx1262_interface_get_stats(adapter->scheduler,
                                                         &scheduler_stats);
    if (status != RNS_OK) {
        return status;
    }
    stats->effective_mtu = RNS_RADIO_PACKET_MTU;
    stats->radio_telemetry_valid = 1;
    stats->radio_signal_valid = scheduler_stats.signal_valid;
    stats->radio_last_rssi_dbm = scheduler_stats.last_rssi_dbm;
    stats->radio_last_snr_db = scheduler_stats.last_snr_db;
    stats->radio_rx_frames = scheduler_stats.rx_frames;
    stats->radio_tx_frames = scheduler_stats.frames_sent;
    stats->radio_cad_busy = scheduler_stats.cad_busy;
    stats->radio_airtime_us = scheduler_stats.rolling_airtime_us;
    stats->radio_duty_deferrals = scheduler_stats.duty_deferrals;
    stats->bytes_received = scheduler_stats.rx_bytes;
    stats->bytes_sent = scheduler_stats.tx_bytes;
    stats->rx_overflows = scheduler_stats.rx_overflows;
    stats->tx_overflows = scheduler_stats.tx_overflows;
    stats->pending_tx = scheduler_stats.pending_packets;
    stats->online = scheduler_stats.online ? 1 : 0;
    stats->outbound = 1;
    stats->broadcast = 1;
    return RNS_OK;
}

static void interface_stop(void *context) {
    heltec_radio_adapter_t *adapter = context;
    (void)rns_sx1262_interface_stop(adapter->scheduler);
}

static void interface_destroy(void *context) {
    heltec_radio_adapter_t *adapter = context;
    const rns_platform_ops_t *platform = adapter->platform;
    rns_sx1262_interface_destroy(adapter->scheduler);
    adapter->scheduler = NULL;
    (void)adapter->backend_ops->destroy(adapter->backend_context);
    memset(adapter, 0, sizeof(*adapter));
    platform->deallocate(platform->context, adapter);
}

static const rns_interface_ops_t INTERFACE_OPS = {
    .start = interface_start,
    .poll = interface_poll,
    .send = interface_send,
    .send_with_id = interface_send_with_id,
    .get_stats = interface_get_stats,
    .stop = interface_stop,
    .destroy = interface_destroy};

rns_status_t rns_heltec_reticulum_radio_create_with_backend(
    const rns_heltec_reticulum_radio_config_t *config,
    const rns_heltec_reticulum_radio_backend_ops_t *backend_ops,
    void *backend_context, const rns_sx1262_clock_ops_t *clock_ops,
    void *clock_context, rns_sx1262_packet_result_fn result_callback,
    void *result_context, rns_interface_t **interface_out) {
    const rns_platform_ops_t *platform;
    heltec_radio_adapter_t *adapter;
    rns_status_t status;
    if (interface_out == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *interface_out = NULL;
    if (!valid_radio_config(config) ||
        !valid_backend(backend_ops) || backend_context == NULL ||
        clock_ops == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    platform = rns_platform_current();
    if (platform == NULL) {
        return RNS_ERROR_INVALID_STATE;
    }
    adapter = platform->allocate(platform->context, sizeof(*adapter));
    if (adapter == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->platform = platform;
    adapter->backend_ops = backend_ops;
    adapter->backend_context = backend_context;
    adapter->radio_config = translate_config(config);
    status = rns_sx1262_interface_create(
        &config->scheduler, &PHY_OPS, adapter, clock_ops, clock_context,
        result_callback, result_context, &adapter->scheduler);
    if (status == RNS_OK) {
        status = rns_interface_create(&INTERFACE_OPS, adapter, interface_out);
    }
    if (status != RNS_OK) {
        if (adapter->scheduler != NULL) {
            rns_sx1262_interface_destroy(adapter->scheduler);
        }
        platform->deallocate(platform->context, adapter);
    }
    return status;
}

#ifdef ESP_PLATFORM
static rns_status_t esp_restart(void *context,
                                const rns_sx1262_config_t *config) {
    return rns_heltec_sx1262_abort_and_restart(context, config);
}
static rns_status_t esp_start_cad(void *context) {
    return rns_heltec_sx1262_start_cad(context);
}
static rns_status_t esp_receive_cad(void *context,
                                    rns_sx1262_cad_result_t *result) {
    return rns_heltec_sx1262_receive_cad_result(context, result);
}
static rns_status_t esp_send(void *context, const uint8_t *frame,
                             size_t frame_length, uint32_t *frame_id) {
    return rns_heltec_sx1262_send_with_id(context, frame, frame_length,
                                          frame_id);
}
static rns_status_t esp_receive_tx(void *context,
                                   rns_sx1262_tx_result_t *result) {
    return rns_heltec_sx1262_receive_tx_result(context, result);
}
static rns_status_t esp_receive(void *context, rns_sx1262_packet_t *packet) {
    return rns_heltec_sx1262_receive(context, packet);
}
static rns_status_t esp_stop(void *context) {
    return rns_heltec_sx1262_stop(context);
}
static rns_status_t esp_destroy(void *context) {
    return rns_heltec_sx1262_close(context);
}
static const rns_heltec_reticulum_radio_backend_ops_t ESP_BACKEND_OPS = {
    .abort_and_restart = esp_restart,
    .start_cad = esp_start_cad,
    .receive_cad_result = esp_receive_cad,
    .send_with_id = esp_send,
    .receive_tx_result = esp_receive_tx,
    .receive = esp_receive,
    .stop = esp_stop,
    .destroy = esp_destroy};

rns_status_t rns_heltec_reticulum_radio_create(
    const rns_heltec_reticulum_radio_config_t *config,
    const rns_sx1262_clock_ops_t *clock_ops, void *clock_context,
    rns_sx1262_packet_result_fn result_callback, void *result_context,
    rns_interface_t **interface_out) {
    rns_heltec_sx1262_t *radio = NULL;
    rns_sx1262_config_t translated;
    rns_status_t status;
    if (!valid_radio_config(config) || interface_out == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    translated = translate_config(config);
    status = rns_heltec_sx1262_open_stopped_with_config(&translated, &radio);
    if (status != RNS_OK) {
        return status;
    }
    status = rns_heltec_reticulum_radio_create_with_backend(
        config, &ESP_BACKEND_OPS, radio, clock_ops, clock_context,
        result_callback, result_context, interface_out);
    if (status != RNS_OK) {
        (void)rns_heltec_sx1262_close(radio);
    }
    return status;
}
#endif
