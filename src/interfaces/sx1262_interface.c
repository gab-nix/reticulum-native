/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "reticulum/sx1262_interface.h"

#include <limits.h>
#include <string.h>

#include "reticulum/hal.h"

typedef enum scheduler_state {
    SCHEDULER_IDLE = 0,
    SCHEDULER_DUTY_WAIT,
    SCHEDULER_CONTENTION,
    SCHEDULER_CAD,
    SCHEDULER_TX_FRAME_1,
    SCHEDULER_TX_FRAME_2
} scheduler_state_t;

typedef struct queued_packet {
    rns_radio_encoded_packet_t encoded;
    uint64_t airtime_us;
    size_t packet_length;
    uint32_t id;
    uint8_t sequence;
    uint8_t cad_attempts;
} queued_packet_t;

typedef struct duty_bucket {
    uint64_t minute;
    uint64_t airtime_us;
    bool valid;
} duty_bucket_t;

/* Sixty-one history minutes plus room for a never-overlapping, pre-reserved
 * whole packet of up to one full duty-cycle hour. The accounting window itself
 * remains exactly the conservative 61 minute buckets promised by the API. */
#define RNS_SX1262_DUTY_STORAGE_BUCKET_COUNT \
    (RNS_SX1262_DUTY_BUCKET_COUNT * 2U + 1U)

struct rns_sx1262_interface {
    const rns_platform_ops_t *platform;
    const rns_sx1262_phy_ops_t *phy_ops;
    void *phy_context;
    const rns_sx1262_clock_ops_t *clock_ops;
    void *clock_context;
    rns_sx1262_packet_result_fn result_callback;
    void *result_context;
    rns_sx1262_scheduler_config_t config;
    queued_packet_t queue[RNS_SX1262_PACKET_QUEUE_CAPACITY];
    duty_bucket_t duty[RNS_SX1262_DUTY_STORAGE_BUCKET_COUNT];
    rns_radio_reassembler_t reassembler;
    uint8_t reassembly_storage[RNS_RADIO_PACKET_MTU];
    size_t queue_head;
    size_t queue_count;
    uint64_t last_now_ms;
    uint64_t ready_at_ms;
    uint64_t sequence_blocked_until_ms[RNS_RADIO_SEQUENCE_COUNT];
    bool sequence_blocked[RNS_RADIO_SEQUENCE_COUNT];
    uint64_t operation_deadline_ms;
    uint32_t next_packet_id;
    uint32_t next_operation_token;
    uint32_t expected_operation_token;
    scheduler_state_t state;
    rns_sx1262_scheduler_stats_t stats;
    bool started;
    bool in_poll;
};

static uint64_t add_saturated(uint64_t value, uint64_t addition) {
    return addition > UINT64_MAX - value ? UINT64_MAX : value + addition;
}

static uint64_t multiply_saturated(uint64_t left, uint64_t right) {
    if (left != 0U && right > UINT64_MAX / left) {
        return UINT64_MAX;
    }
    return left * right;
}

static bool valid_config(const rns_sx1262_scheduler_config_t *config) {
    const bool valid_bandwidth =
        config != NULL &&
        (config->bandwidth_hz == 7800U ||
         config->bandwidth_hz == 10400U ||
         config->bandwidth_hz == 15600U ||
         config->bandwidth_hz == 20800U ||
         config->bandwidth_hz == 31250U ||
         config->bandwidth_hz == 41700U ||
         config->bandwidth_hz == 62500U ||
         config->bandwidth_hz == 125000U ||
         config->bandwidth_hz == 250000U ||
         config->bandwidth_hz == 500000U);
    return valid_bandwidth &&
           config->spreading_factor >= 5U &&
           config->spreading_factor <= 12U &&
           (config->spreading_factor > 6U ||
            config->preamble_symbols >= 12U) &&
           config->coding_rate_denominator >= 5U &&
           config->coding_rate_denominator <= 8U &&
           config->preamble_symbols != 0U &&
           config->duty_cycle_ppm != 0U &&
           config->duty_cycle_ppm <= 1000000U &&
           config->difs_ms <= 3600000U &&
           config->contention_window_ms <= 3600000U &&
           config->cad_busy_backoff_ms <= 3600000U &&
           config->cad_timeout_ms != 0U &&
           config->cad_timeout_ms <= 3600000U &&
           config->tx_timeout_margin_ms <= 3600000U &&
           config->max_cad_attempts != 0U &&
           config->fragment_timeout_ms != 0U &&
           config->sequence_reuse_guard_ms != 0U;
}

static uint32_t effective_bandwidth_hz(uint32_t bandwidth_hz) {
    /* SX126X_LORA_BW_041 is 41.667 kHz; 41700 is the public nominal value. */
    return bandwidth_hz == 41700U ? 41667U : bandwidth_hz;
}

void rns_sx1262_scheduler_default_config(
    rns_sx1262_scheduler_config_t *config) {
    if (config == NULL) {
        return;
    }
    *config = (rns_sx1262_scheduler_config_t){
        .bandwidth_hz = 125000U,
        .spreading_factor = 8U,
        .coding_rate_denominator = 5U,
        .preamble_symbols = 18U,
        .explicit_header = true,
        .crc_enabled = true,
        .duty_cycle_ppm = RNS_SX1262_DEFAULT_DUTY_CYCLE_PPM,
        .difs_ms = 100U,
        .contention_window_ms = 100U,
        .cad_busy_backoff_ms = 250U,
        .cad_timeout_ms = 5000U,
        .tx_timeout_margin_ms = 250U,
        .max_cad_attempts = 8U,
        .fragment_timeout_ms = 30000U,
        .sequence_reuse_guard_ms = 60000U};
}

rns_status_t rns_sx1262_airtime_us(
    const rns_sx1262_scheduler_config_t *config, size_t frame_length,
    uint64_t *airtime_us) {
    int64_t payload_numerator;
    uint64_t positive_numerator;
    uint64_t denominator;
    uint64_t payload_symbols;
    uint64_t quarter_symbols;
    uint64_t numerator;
    bool low_data_rate_optimisation;

    if (!valid_config(config) || frame_length == 0U ||
        frame_length > RNS_RADIO_PHY_MTU || airtime_us == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }

    low_data_rate_optimisation =
        ((uint64_t)1U << config->spreading_factor) * 1000000ULL >=
        (uint64_t)effective_bandwidth_hz(config->bandwidth_hz) * 16000ULL;

    payload_numerator = (int64_t)(frame_length * 8U) -
                        (int64_t)(4U * config->spreading_factor) +
                        (config->crc_enabled ? 16 : 0) +
                        (config->explicit_header ? 20 : 0) +
                        (config->spreading_factor > 6U ? 8 : 0);
    positive_numerator =
        payload_numerator > 0 ? (uint64_t)payload_numerator : 0U;
    denominator = 4U *
                  ((config->spreading_factor > 6U &&
                    low_data_rate_optimisation)
                       ? config->spreading_factor - 2U
                       : config->spreading_factor);
    payload_symbols =
        8U + ((positive_numerator + denominator - 1U) / denominator) *
                 config->coding_rate_denominator;

    quarter_symbols =
        (uint64_t)((config->spreading_factor <= 6U &&
                    config->preamble_symbols < 12U)
                       ? 12U
                       : config->preamble_symbols) *
            4U +
        17U +
                      payload_symbols * 4U;
    if (config->spreading_factor <= 6U) {
        quarter_symbols += 8U;
    }
    numerator = multiply_saturated(
        multiply_saturated(quarter_symbols,
                           (uint64_t)1U << config->spreading_factor),
        1000000U);
    if (numerator == UINT64_MAX) {
        return RNS_ERROR_OVERFLOW;
    }
    denominator = (uint64_t)effective_bandwidth_hz(config->bandwidth_hz) * 4U;
    *airtime_us = (numerator + denominator - 1U) / denominator;
    return RNS_OK;
}

static uint64_t current_time(rns_sx1262_interface_t *interface_value) {
    uint64_t now = interface_value->clock_ops->monotonic_ms(
        interface_value->clock_context);
    if (now < interface_value->last_now_ms) {
        interface_value->stats.clock_regressions++;
        return interface_value->last_now_ms;
    }
    interface_value->last_now_ms = now;
    return now;
}

static uint64_t rolling_airtime(rns_sx1262_interface_t *interface_value,
                                uint64_t now_ms) {
    const uint64_t minute = now_ms / 60000U;
    uint64_t total = 0U;
    size_t index;
    for (index = 0U; index < RNS_SX1262_DUTY_STORAGE_BUCKET_COUNT; ++index) {
        duty_bucket_t *bucket = &interface_value->duty[index];
        if (bucket->valid &&
            ((bucket->minute > minute && bucket->minute - minute <= 61U) ||
             (minute >= bucket->minute &&
              minute - bucket->minute <= 60U))) {
            total = add_saturated(total, bucket->airtime_us);
        }
    }
    return total;
}

static uint64_t duty_limit_us(
    const rns_sx1262_interface_t *interface_value) {
    return (3600000000ULL * interface_value->config.duty_cycle_ppm) /
           1000000ULL;
}

static bool duty_available(rns_sx1262_interface_t *interface_value,
                           uint64_t now_ms, uint64_t airtime_us) {
    const uint64_t used = rolling_airtime(interface_value, now_ms);
    const uint64_t limit = duty_limit_us(interface_value);
    interface_value->stats.rolling_airtime_us = used;
    return airtime_us <= limit && used <= limit - airtime_us;
}

static void reserve_airtime(rns_sx1262_interface_t *interface_value,
                            uint64_t now_ms, uint64_t airtime_us) {
    uint64_t minute = now_ms / 60000U;
    uint64_t offset_us = (now_ms % 60000U) * 1000U;
    uint64_t remaining = airtime_us;
    while (remaining != 0U) {
        const uint64_t capacity = 60000000U - offset_us;
        const uint64_t portion = remaining < capacity ? remaining : capacity;
        duty_bucket_t *bucket =
            &interface_value
                 ->duty[minute % RNS_SX1262_DUTY_STORAGE_BUCKET_COUNT];
        if (!bucket->valid || bucket->minute != minute) {
            *bucket = (duty_bucket_t){.minute = minute, .valid = true};
        }
        bucket->airtime_us = add_saturated(bucket->airtime_us, portion);
        remaining -= portion;
        offset_us = 0U;
        if (remaining != 0U) {
            minute++;
        }
    }
    interface_value->stats.airtime_reserved_us = add_saturated(
        interface_value->stats.airtime_reserved_us, airtime_us);
    interface_value->stats.rolling_airtime_us =
        rolling_airtime(interface_value, now_ms);
}

static queued_packet_t *front_packet(
    rns_sx1262_interface_t *interface_value) {
    return interface_value->queue_count == 0U
               ? NULL
               : &interface_value->queue[interface_value->queue_head];
}

static bool split_sequence_in_queue(
    const rns_sx1262_interface_t *interface_value, uint8_t sequence) {
    size_t offset;
    for (offset = 0U; offset < interface_value->queue_count; ++offset) {
        const queued_packet_t *packet =
            &interface_value->queue[(interface_value->queue_head + offset) %
                                    RNS_SX1262_PACKET_QUEUE_CAPACITY];
        if (packet->encoded.count == 2U && packet->sequence == sequence) {
            return true;
        }
    }
    return false;
}

static rns_status_t choose_split_sequence(
    rns_sx1262_interface_t *interface_value, uint8_t random_byte,
    uint64_t now_ms, uint8_t *sequence_out) {
    const uint8_t first = (uint8_t)(random_byte & 0x0fU);
    size_t offset;
    for (offset = 0U; offset < RNS_RADIO_SEQUENCE_COUNT; ++offset) {
        const uint8_t sequence = (uint8_t)((first + offset) & 0x0fU);
        bool blocked = interface_value->sequence_blocked[sequence];
        if (blocked &&
            interface_value->sequence_blocked_until_ms[sequence] !=
                UINT64_MAX &&
            now_ms >= interface_value->sequence_blocked_until_ms[sequence]) {
            interface_value->sequence_blocked[sequence] = false;
            blocked = false;
        }
        if (!blocked &&
            !split_sequence_in_queue(interface_value, sequence)) {
            *sequence_out = sequence;
            return RNS_OK;
        }
    }
    return RNS_ERROR_INVALID_STATE;
}

static rns_status_t random_delay(
    rns_sx1262_interface_t *interface_value, uint32_t maximum_ms,
    uint32_t *delay_ms) {
    uint32_t random_value;
    uint64_t range;
    uint64_t limit;
    rns_status_t status;
    if (maximum_ms == 0U) {
        *delay_ms = 0U;
        return RNS_OK;
    }
    range = (uint64_t)maximum_ms + 1U;
    limit = ((uint64_t)UINT32_MAX + 1U) / range * range;
    {
        size_t attempt;
        for (attempt = 0U; attempt < 4U; ++attempt) {
            status = interface_value->clock_ops->entropy(
                interface_value->clock_context, (uint8_t *)&random_value,
                sizeof(random_value));
            if (status != RNS_OK) {
                return status;
            }
            if ((uint64_t)random_value < limit) {
                *delay_ms = (uint32_t)((uint64_t)random_value % range);
                return RNS_OK;
            }
        }
    }
    return RNS_ERROR_IO;
}

static void finish_front(rns_sx1262_interface_t *interface_value,
                         rns_sx1262_packet_outcome_t outcome,
                         rns_status_t status) {
    queued_packet_t *packet = front_packet(interface_value);
    rns_sx1262_packet_result_fn callback;
    void *callback_context;
    uint32_t packet_id;
    uint8_t sequence;
    uint64_t now_ms;
    size_t packet_length;
    bool was_split;
    if (packet == NULL) {
        return;
    }
    packet_id = packet->id;
    sequence = packet->sequence;
    packet_length = packet->packet_length;
    was_split = packet->encoded.count == 2U;
    memset(packet, 0, sizeof(*packet));
    interface_value->queue_head =
        (interface_value->queue_head + 1U) %
        RNS_SX1262_PACKET_QUEUE_CAPACITY;
    interface_value->queue_count--;
    interface_value->stats.pending_packets = interface_value->queue_count;
    interface_value->state = SCHEDULER_IDLE;
    interface_value->expected_operation_token = 0U;
    interface_value->operation_deadline_ms = 0U;
    interface_value->ready_at_ms = 0U;
    interface_value->stats.transmitting = false;
    if (outcome == RNS_SX1262_PACKET_SENT) {
        interface_value->stats.packets_sent++;
        interface_value->stats.tx_bytes += packet_length;
    } else {
        interface_value->stats.packets_dropped++;
        interface_value->stats.last_error = status;
    }
    now_ms = current_time(interface_value);
    if (was_split) {
        interface_value->sequence_blocked[sequence] = true;
        interface_value->sequence_blocked_until_ms[sequence] = add_saturated(
            now_ms, interface_value->config.sequence_reuse_guard_ms);
    }
    callback = interface_value->result_callback;
    callback_context = interface_value->result_context;
    if (callback != NULL) {
        /* State is terminal before re-entrant application code runs. */
        callback(callback_context, packet_id, outcome, status);
    }
}

static rns_status_t receive_frame_callback(const uint8_t *packet,
                                           size_t packet_length,
                                           void *context) {
    struct receive_dispatch {
        rns_sx1262_interface_t *interface_value;
        rns_interface_receive_fn callback;
        void *callback_context;
    } *dispatch = context;
    rns_status_t status;
    dispatch->interface_value->stats.rx_packets++;
    dispatch->interface_value->stats.rx_bytes += packet_length;
    status = dispatch->callback(dispatch->callback_context, packet,
                                packet_length);
    return status;
}

static rns_status_t cancel_active_operation(
    rns_sx1262_interface_t *interface_value) {
    rns_status_t status = interface_value->phy_ops->cancel_operation(
        interface_value->phy_context, &interface_value->config,
        interface_value->expected_operation_token);
    if (status != RNS_OK) {
        rns_status_t recovery =
            interface_value->phy_ops->stop(interface_value->phy_context);
        if (recovery == RNS_OK) {
            recovery = interface_value->phy_ops->start(
                interface_value->phy_context, &interface_value->config);
        }
        if (recovery != RNS_OK) {
            interface_value->started = false;
            interface_value->stats.online = false;
            return recovery;
        }
    }
    return status;
}

static rns_status_t handle_rx_event(
    rns_sx1262_interface_t *interface_value,
    const rns_sx1262_phy_event_t *event, rns_interface_receive_fn receive,
    void *receive_context, uint64_t now_ms) {
    struct receive_dispatch {
        rns_sx1262_interface_t *interface_value;
        rns_interface_receive_fn callback;
        void *callback_context;
    } dispatch = {interface_value, receive, receive_context};
    rns_status_t status;
    if (event->frame == NULL || event->frame_length == 0U ||
        event->frame_length > RNS_RADIO_PHY_MTU) {
        interface_value->stats.rx_malformed++;
        return RNS_ERROR_PROTOCOL;
    }
    interface_value->stats.rx_frames++;
    status = rns_radio_reassembler_feed(
        &interface_value->reassembler, event->frame, event->frame_length,
        now_ms, receive_frame_callback, &dispatch);
    if (status == RNS_ERROR_PROTOCOL || status == RNS_ERROR_INVALID_STATE) {
        interface_value->stats.rx_malformed++;
    } else if (status == RNS_ERROR_OVERFLOW) {
        interface_value->stats.rx_overflows++;
    }
    return status;
}

static uint32_t next_operation_token(
    rns_sx1262_interface_t *interface_value) {
    interface_value->next_operation_token++;
    if (interface_value->next_operation_token == 0U) {
        interface_value->next_operation_token++;
    }
    return interface_value->next_operation_token;
}

static rns_status_t begin_first_frame(
    rns_sx1262_interface_t *interface_value, uint64_t now_ms) {
    queued_packet_t *packet = front_packet(interface_value);
    uint64_t frame_airtime_us;
    uint64_t timeout_ms;
    rns_status_t status;
    if (packet == NULL) {
        interface_value->state = SCHEDULER_IDLE;
        return RNS_OK;
    }
    if (!duty_available(interface_value, now_ms, packet->airtime_us)) {
        if (interface_value->state != SCHEDULER_DUTY_WAIT) {
            interface_value->stats.duty_deferrals++;
        }
        interface_value->state = SCHEDULER_DUTY_WAIT;
        return RNS_OK;
    }
    reserve_airtime(interface_value, now_ms, packet->airtime_us);
    status = rns_sx1262_airtime_us(&interface_value->config,
                                   packet->encoded.lengths[0],
                                   &frame_airtime_us);
    if (status != RNS_OK) {
        finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY, status);
        return status;
    }
    timeout_ms = (frame_airtime_us + 999U) / 1000U;
    interface_value->operation_deadline_ms = add_saturated(
        now_ms, add_saturated(timeout_ms,
                              interface_value->config.tx_timeout_margin_ms));
    interface_value->expected_operation_token =
        next_operation_token(interface_value);
    status = interface_value->phy_ops->transmit(
        interface_value->phy_context, &interface_value->config,
        packet->encoded.frames[0], packet->encoded.lengths[0],
        interface_value->expected_operation_token);
    if (status != RNS_OK) {
        finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY, status);
        return status;
    }
    interface_value->state = SCHEDULER_TX_FRAME_1;
    return RNS_OK;
}

static rns_status_t advance_scheduler(
    rns_sx1262_interface_t *interface_value, uint64_t now_ms) {
    queued_packet_t *packet = front_packet(interface_value);
    uint32_t contention_ms;
    rns_status_t status;
    if (packet == NULL) {
        interface_value->state = SCHEDULER_IDLE;
        return RNS_OK;
    }
    if (interface_value->state == SCHEDULER_IDLE) {
        if (!duty_available(interface_value, now_ms, packet->airtime_us)) {
            interface_value->stats.duty_deferrals++;
            interface_value->state = SCHEDULER_DUTY_WAIT;
            return RNS_OK;
        }
        status = random_delay(interface_value,
                              interface_value->config.contention_window_ms,
                              &contention_ms);
        if (status != RNS_OK) {
            finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                         status);
            return status;
        }
        interface_value->ready_at_ms = add_saturated(
            now_ms,
            add_saturated(interface_value->config.difs_ms,
                          contention_ms));
        interface_value->state = SCHEDULER_CONTENTION;
        return RNS_OK;
    }
    if (interface_value->state == SCHEDULER_DUTY_WAIT) {
        if (!duty_available(interface_value, now_ms, packet->airtime_us)) {
            return RNS_OK;
        }
        status = random_delay(interface_value,
                              interface_value->config.contention_window_ms,
                              &contention_ms);
        if (status != RNS_OK) {
            finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                         status);
            return status;
        }
        interface_value->ready_at_ms = add_saturated(
            now_ms,
            add_saturated(interface_value->config.difs_ms,
                          contention_ms));
        interface_value->state = SCHEDULER_CONTENTION;
        return RNS_OK;
    }
    if (interface_value->state == SCHEDULER_CONTENTION &&
        now_ms >= interface_value->ready_at_ms) {
        interface_value->expected_operation_token =
            next_operation_token(interface_value);
        interface_value->operation_deadline_ms = add_saturated(
            now_ms, interface_value->config.cad_timeout_ms);
        status = interface_value->phy_ops->start_cad(
            interface_value->phy_context, &interface_value->config,
            interface_value->expected_operation_token);
        if (status != RNS_OK) {
            interface_value->stats.cad_failures++;
            finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                         status);
            return status;
        }
        packet->cad_attempts++;
        interface_value->state = SCHEDULER_CAD;
        return RNS_OK;
    }
    return RNS_OK;
}

static rns_status_t handle_phy_event(
    rns_sx1262_interface_t *interface_value,
    const rns_sx1262_phy_event_t *event, rns_interface_receive_fn receive,
    void *receive_context, uint64_t now_ms) {
    queued_packet_t *packet = front_packet(interface_value);
    rns_status_t status = event->status == RNS_OK ? RNS_ERROR_IO : event->status;
    if (event->type == RNS_SX1262_PHY_EVENT_RX_FRAME) {
        return handle_rx_event(interface_value, event, receive, receive_context,
                               now_ms);
    }
    if (event->type == RNS_SX1262_PHY_EVENT_CAD_CLEAR &&
        interface_value->state == SCHEDULER_CAD && packet != NULL &&
        event->operation_token == interface_value->expected_operation_token) {
        return begin_first_frame(interface_value, now_ms);
    }
    if (event->type == RNS_SX1262_PHY_EVENT_CAD_BUSY &&
        interface_value->state == SCHEDULER_CAD && packet != NULL &&
        event->operation_token == interface_value->expected_operation_token) {
        interface_value->stats.cad_busy++;
        interface_value->operation_deadline_ms = 0U;
        if (packet->cad_attempts >= interface_value->config.max_cad_attempts) {
            finish_front(interface_value,
                         RNS_SX1262_PACKET_DROPPED_CONTENTION,
                         RNS_ERROR_TIMEOUT);
        } else {
            uint32_t backoff_ms;
            rns_status_t delay_status = random_delay(
                interface_value,
                interface_value->config.cad_busy_backoff_ms,
                &backoff_ms);
            if (delay_status != RNS_OK) {
                finish_front(interface_value,
                             RNS_SX1262_PACKET_DROPPED_PHY, delay_status);
                return delay_status;
            }
            interface_value->ready_at_ms = add_saturated(
                now_ms, add_saturated(interface_value->config.difs_ms,
                                      backoff_ms));
            interface_value->state = SCHEDULER_CONTENTION;
        }
        return RNS_OK;
    }
    if (event->type == RNS_SX1262_PHY_EVENT_CAD_FAILED &&
        interface_value->state == SCHEDULER_CAD &&
        event->operation_token == interface_value->expected_operation_token) {
        interface_value->stats.cad_failures++;
        finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                     status);
        return status;
    }
    if (event->type == RNS_SX1262_PHY_EVENT_TX_FAILED &&
        (interface_value->state == SCHEDULER_TX_FRAME_1 ||
         interface_value->state == SCHEDULER_TX_FRAME_2)) {
        if (event->operation_token != interface_value->expected_operation_token) {
            interface_value->stats.stale_control_events++;
            return RNS_ERROR_PROTOCOL;
        }
        finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY, status);
        return status;
    }
    if (event->type == RNS_SX1262_PHY_EVENT_TX_DONE && packet != NULL &&
        interface_value->state == SCHEDULER_TX_FRAME_1) {
        if (event->operation_token != interface_value->expected_operation_token) {
            interface_value->stats.stale_control_events++;
            return RNS_ERROR_PROTOCOL;
        }
        interface_value->stats.frames_sent++;
        if (packet->encoded.count == 1U) {
            finish_front(interface_value, RNS_SX1262_PACKET_SENT, RNS_OK);
            return RNS_OK;
        }
        /* Atomic split packet: frame two follows TX_DONE with no CAD/delay. */
        interface_value->expected_operation_token =
            next_operation_token(interface_value);
        status = interface_value->phy_ops->transmit(
            interface_value->phy_context, &interface_value->config,
            packet->encoded.frames[1], packet->encoded.lengths[1],
            interface_value->expected_operation_token);
        if (status != RNS_OK) {
            finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                         status);
            return status;
        }
        {
            uint64_t frame_airtime_us;
            status = rns_sx1262_airtime_us(
                &interface_value->config, packet->encoded.lengths[1],
                &frame_airtime_us);
            if (status != RNS_OK) {
                finish_front(interface_value,
                             RNS_SX1262_PACKET_DROPPED_PHY, status);
                return status;
            }
            interface_value->operation_deadline_ms = add_saturated(
                now_ms,
                add_saturated((frame_airtime_us + 999U) / 1000U,
                              interface_value->config.tx_timeout_margin_ms));
        }
        interface_value->state = SCHEDULER_TX_FRAME_2;
        return RNS_OK;
    }
    if (event->type == RNS_SX1262_PHY_EVENT_TX_DONE && packet != NULL &&
        interface_value->state == SCHEDULER_TX_FRAME_2) {
        if (event->operation_token != interface_value->expected_operation_token) {
            interface_value->stats.stale_control_events++;
            return RNS_ERROR_PROTOCOL;
        }
        interface_value->stats.frames_sent++;
        finish_front(interface_value, RNS_SX1262_PACKET_SENT, RNS_OK);
        return RNS_OK;
    }
    interface_value->stats.stale_control_events++;
    return RNS_ERROR_PROTOCOL;
}

rns_status_t rns_sx1262_interface_create(
    const rns_sx1262_scheduler_config_t *config,
    const rns_sx1262_phy_ops_t *phy_ops, void *phy_context,
    const rns_sx1262_clock_ops_t *clock_ops, void *clock_context,
    rns_sx1262_packet_result_fn result_callback, void *result_context,
    rns_sx1262_interface_t **interface_out) {
    const rns_platform_ops_t *platform;
    rns_sx1262_interface_t *created;
    rns_status_t status;
    if (interface_out == NULL || !valid_config(config) || phy_ops == NULL ||
        phy_ops->start == NULL || phy_ops->poll_event == NULL ||
        phy_ops->start_cad == NULL || phy_ops->transmit == NULL ||
        phy_ops->cancel_operation == NULL || phy_ops->stop == NULL ||
        phy_context == NULL || clock_ops == NULL ||
        clock_ops->monotonic_ms == NULL || clock_ops->entropy == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *interface_out = NULL;
    platform = rns_platform_current();
    if (platform == NULL) {
        return RNS_ERROR_INVALID_STATE;
    }
    created = platform->allocate(platform->context, sizeof(*created));
    if (created == NULL) {
        return RNS_ERROR_NO_MEMORY;
    }
    memset(created, 0, sizeof(*created));
    created->platform = platform;
    created->phy_ops = phy_ops;
    created->phy_context = phy_context;
    created->clock_ops = clock_ops;
    created->clock_context = clock_context;
    created->result_callback = result_callback;
    created->result_context = result_context;
    created->config = *config;
    status = rns_radio_reassembler_init(
        &created->reassembler, created->reassembly_storage,
        sizeof(created->reassembly_storage), config->fragment_timeout_ms,
        config->sequence_reuse_guard_ms);
    if (status != RNS_OK) {
        platform->deallocate(platform->context, created);
        return status;
    }
    *interface_out = created;
    return RNS_OK;
}

void rns_sx1262_interface_destroy(rns_sx1262_interface_t *interface_value) {
    const rns_platform_ops_t *platform;
    if (interface_value == NULL) {
        return;
    }
    (void)rns_sx1262_interface_stop(interface_value);
    platform = interface_value->platform;
    memset(interface_value, 0, sizeof(*interface_value));
    platform->deallocate(platform->context, interface_value);
}

rns_status_t rns_sx1262_interface_start(
    rns_sx1262_interface_t *interface_value) {
    rns_status_t status;
    if (interface_value == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (interface_value->started) {
        return RNS_ERROR_INVALID_STATE;
    }
    status = interface_value->phy_ops->start(interface_value->phy_context,
                                              &interface_value->config);
    if (status != RNS_OK) {
        interface_value->stats.last_error = status;
        return status;
    }
    interface_value->last_now_ms = interface_value->clock_ops->monotonic_ms(
        interface_value->clock_context);
    interface_value->started = true;
    interface_value->stats.online = true;
    return RNS_OK;
}

rns_status_t rns_sx1262_interface_send(
    rns_sx1262_interface_t *interface_value, const uint8_t *packet,
    size_t packet_length, uint32_t *packet_id) {
    queued_packet_t queued;
    uint8_t entropy;
    uint8_t sequence;
    uint64_t now_ms;
    uint64_t frame_airtime;
    size_t index;
    size_t tail;
    rns_status_t status;
    if (interface_value == NULL || packet == NULL || packet_length == 0U ||
        packet_length > RNS_RADIO_PACKET_MTU || packet_id == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (!interface_value->started) {
        return RNS_ERROR_INVALID_STATE;
    }
    if (interface_value->queue_count == RNS_SX1262_PACKET_QUEUE_CAPACITY) {
        interface_value->stats.tx_overflows++;
        return RNS_ERROR_OVERFLOW;
    }
    status = interface_value->clock_ops->entropy(
        interface_value->clock_context, &entropy, sizeof(entropy));
    if (status != RNS_OK) {
        interface_value->stats.last_error = status;
        return status;
    }
    now_ms = current_time(interface_value);
    if (packet_length > RNS_RADIO_FRAME_PAYLOAD_MTU) {
        status = choose_split_sequence(interface_value, entropy, now_ms,
                                       &sequence);
        if (status != RNS_OK) {
            interface_value->stats.last_error = status;
            return status;
        }
    } else {
        sequence = (uint8_t)(entropy & 0x0fU);
    }
    memset(&queued, 0, sizeof(queued));
    status = rns_radio_frame_encode(packet, packet_length, sequence,
                                    &queued.encoded);
    if (status != RNS_OK) {
        return status;
    }
    for (index = 0U; index < queued.encoded.count; ++index) {
        status = rns_sx1262_airtime_us(&interface_value->config,
                                       queued.encoded.lengths[index],
                                       &frame_airtime);
        if (status != RNS_OK ||
            frame_airtime > UINT64_MAX - queued.airtime_us) {
            return status == RNS_OK ? RNS_ERROR_OVERFLOW : status;
        }
        queued.airtime_us += frame_airtime;
    }
    if (queued.airtime_us > duty_limit_us(interface_value)) {
        interface_value->stats.last_error = RNS_ERROR_OVERFLOW;
        return RNS_ERROR_OVERFLOW;
    }
    interface_value->next_packet_id++;
    if (interface_value->next_packet_id == 0U) {
        interface_value->next_packet_id++;
    }
    queued.id = interface_value->next_packet_id;
    queued.sequence = sequence;
    queued.packet_length = packet_length;
    tail = (interface_value->queue_head + interface_value->queue_count) %
           RNS_SX1262_PACKET_QUEUE_CAPACITY;
    interface_value->queue[tail] = queued;
    interface_value->queue_count++;
    interface_value->stats.packets_queued++;
    interface_value->stats.pending_packets = interface_value->queue_count;
    *packet_id = queued.id;
    return RNS_OK;
}

rns_status_t rns_sx1262_interface_poll(
    rns_sx1262_interface_t *interface_value, rns_interface_receive_fn receive,
    void *receive_context, size_t budget) {
    size_t handled = 0U;
    uint64_t now_ms;
    rns_status_t result = RNS_OK;
    if (interface_value == NULL || receive == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (!interface_value->started) {
        return RNS_ERROR_INVALID_STATE;
    }
    if (interface_value->in_poll) {
        return RNS_ERROR_INVALID_STATE;
    }
    if (budget == 0U) {
        return RNS_OK;
    }
    interface_value->in_poll = true;
    now_ms = current_time(interface_value);
    (void)rns_radio_reassembler_expire(&interface_value->reassembler, now_ms);
    if ((interface_value->state == SCHEDULER_CAD ||
         interface_value->state == SCHEDULER_TX_FRAME_1 ||
         interface_value->state == SCHEDULER_TX_FRAME_2) &&
        interface_value->operation_deadline_ms != 0U &&
        now_ms >= interface_value->operation_deadline_ms) {
        rns_status_t cancel_status = cancel_active_operation(interface_value);
        finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                     cancel_status == RNS_OK ? RNS_ERROR_TIMEOUT
                                             : cancel_status);
        while (!interface_value->started &&
               interface_value->queue_count != 0U) {
            finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_PHY,
                         cancel_status);
        }
        result = cancel_status == RNS_OK ? RNS_ERROR_TIMEOUT : cancel_status;
        goto poll_done;
    }
    while (handled < budget) {
        rns_sx1262_phy_event_t event;
        rns_status_t status;
        memset(&event, 0, sizeof(event));
        status = interface_value->phy_ops->poll_event(
            interface_value->phy_context, &event);
        if (status == RNS_ERROR_NOT_FOUND) {
            break;
        }
        if (status != RNS_OK) {
            interface_value->stats.last_error = status;
            if (interface_value->queue_count != 0U) {
                if (interface_value->state == SCHEDULER_CAD ||
                    interface_value->state == SCHEDULER_TX_FRAME_1 ||
                    interface_value->state == SCHEDULER_TX_FRAME_2) {
                    (void)cancel_active_operation(interface_value);
                }
                finish_front(interface_value,
                             RNS_SX1262_PACKET_DROPPED_PHY, status);
                while (!interface_value->started &&
                       interface_value->queue_count != 0U) {
                    finish_front(interface_value,
                                 RNS_SX1262_PACKET_DROPPED_PHY, status);
                }
            }
            result = status;
            break;
        }
        handled++;
        status = handle_phy_event(interface_value, &event, receive,
                                  receive_context, now_ms);
        if (status != RNS_OK) {
            result = status;
        }
        if (!interface_value->started) {
            break;
        }
    }
    if (interface_value->started) {
        rns_status_t status = advance_scheduler(interface_value, now_ms);
        if (status != RNS_OK) {
            result = status;
        }
    }
poll_done:
    interface_value->stats.transmitting =
        interface_value->state == SCHEDULER_TX_FRAME_1 ||
        interface_value->state == SCHEDULER_TX_FRAME_2;
    interface_value->stats.rolling_airtime_us =
        rolling_airtime(interface_value, now_ms);
    interface_value->in_poll = false;
    return result;
}

rns_status_t rns_sx1262_interface_get_stats(
    rns_sx1262_interface_t *interface_value,
    rns_sx1262_scheduler_stats_t *stats) {
    if (interface_value == NULL || stats == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *stats = interface_value->stats;
    if (interface_value->started) {
        stats->rolling_airtime_us =
            rolling_airtime(interface_value, current_time(interface_value));
    }
    return RNS_OK;
}

rns_status_t rns_sx1262_interface_stop(
    rns_sx1262_interface_t *interface_value) {
    rns_status_t status;
    if (interface_value == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = RNS_OK;
    if (interface_value->started) {
        status = interface_value->phy_ops->stop(interface_value->phy_context);
    }
    interface_value->started = false;
    interface_value->stats.online = false;
    while (interface_value->queue_count != 0U) {
        finish_front(interface_value, RNS_SX1262_PACKET_DROPPED_STOPPED,
                     status == RNS_OK ? RNS_ERROR_INVALID_STATE : status);
    }
    rns_radio_reassembler_reset(&interface_value->reassembler);
    if (status != RNS_OK) {
        interface_value->stats.last_error = status;
    }
    return status;
}

static rns_status_t adapter_start(void *context) {
    return rns_sx1262_interface_start(context);
}

static rns_status_t adapter_poll(void *context, rns_interface_receive_fn receive,
                                 void *receive_context, size_t budget) {
    return rns_sx1262_interface_poll(context, receive, receive_context, budget);
}

static rns_status_t adapter_send(void *context, const uint8_t *packet,
                                 size_t packet_length) {
    uint32_t ignored_id;
    return rns_sx1262_interface_send(context, packet, packet_length,
                                     &ignored_id);
}

static rns_status_t adapter_get_stats(void *context,
                                      rns_interface_stats_t *stats) {
    rns_sx1262_interface_t *interface_value = context;
    rns_sx1262_scheduler_stats_t scheduler_stats;
    rns_status_t status = rns_sx1262_interface_get_stats(interface_value,
                                                         &scheduler_stats);
    if (status != RNS_OK) {
        return status;
    }
    stats->effective_mtu = RNS_RADIO_PACKET_MTU;
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

static void adapter_stop(void *context) {
    (void)rns_sx1262_interface_stop(context);
}

static void adapter_destroy(void *context) {
    rns_sx1262_interface_destroy(context);
}

static const rns_interface_ops_t ADAPTER_OPS = {
    .start = adapter_start,
    .poll = adapter_poll,
    .send = adapter_send,
    .get_stats = adapter_get_stats,
    .stop = adapter_stop,
    .destroy = adapter_destroy};

rns_status_t rns_sx1262_interface_create_adapter(
    const rns_sx1262_scheduler_config_t *config,
    const rns_sx1262_phy_ops_t *phy_ops, void *phy_context,
    const rns_sx1262_clock_ops_t *clock_ops, void *clock_context,
    rns_sx1262_packet_result_fn result_callback, void *result_context,
    rns_interface_t **interface_out) {
    rns_sx1262_interface_t *scheduler = NULL;
    rns_status_t status;
    if (interface_out == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    *interface_out = NULL;
    status = rns_sx1262_interface_create(
        config, phy_ops, phy_context, clock_ops, clock_context, result_callback,
        result_context, &scheduler);
    if (status != RNS_OK) {
        return status;
    }
    status = rns_interface_create(&ADAPTER_OPS, scheduler, interface_out);
    if (status != RNS_OK) {
        rns_sx1262_interface_destroy(scheduler);
    }
    return status;
}
