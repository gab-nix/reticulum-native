#include "reticulum/channel.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/* Half-range modular ordering; callers never retain a full half-space. */
static int sequence_before(uint16_t left, uint16_t right) {
    return (int16_t)(left - right) < 0;
}

static uint16_t sequence_distance(uint16_t sequence, uint16_t base) {
    return (uint16_t)(sequence - base);
}

static void event(rns_channel_t *channel, rns_channel_event_t kind, uint16_t sequence) {
    if (channel->config.event != NULL) {
        channel->config.event(kind, sequence, channel->config.event_context);
    }
}

rns_status_t rns_channel_envelope_encode(uint16_t message_type, uint16_t sequence,
                                         const uint8_t *payload, size_t payload_length,
                                         uint8_t *output, size_t capacity, size_t *output_length) {
    size_t total = RNS_CHANNEL_HEADER_BYTES + payload_length;
    if (output == NULL || output_length == NULL || (payload_length != 0 && payload == NULL)) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (payload_length > RNS_CHANNEL_MAX_PAYLOAD_BYTES || capacity < total) {
        return RNS_ERROR_OVERFLOW;
    }
    write_u16(output, message_type);
    write_u16(output + 2, sequence);
    write_u16(output + 4, (uint16_t)payload_length);
    if (payload_length != 0) {
        memcpy(output + RNS_CHANNEL_HEADER_BYTES, payload, payload_length);
    }
    *output_length = total;
    return RNS_OK;
}

rns_status_t rns_channel_envelope_decode(const uint8_t *envelope, size_t envelope_length,
                                         uint16_t *message_type, uint16_t *sequence,
                                         const uint8_t **payload, size_t *payload_length) {
    size_t declared;
    if (envelope == NULL || message_type == NULL || sequence == NULL || payload == NULL ||
        payload_length == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (envelope_length < RNS_CHANNEL_HEADER_BYTES ||
        envelope_length > RNS_CHANNEL_MAX_ENVELOPE_BYTES) {
        return RNS_ERROR_PROTOCOL;
    }
    declared = read_u16(envelope + 4);
    if (declared != envelope_length - RNS_CHANNEL_HEADER_BYTES) {
        return RNS_ERROR_PROTOCOL;
    }
    *message_type = read_u16(envelope);
    *sequence = read_u16(envelope + 2);
    *payload = envelope + RNS_CHANNEL_HEADER_BYTES;
    *payload_length = declared;
    return RNS_OK;
}

rns_status_t rns_channel_init(rns_channel_t *channel, const rns_channel_config_t *config) {
    if (channel == NULL || config == NULL || config->clock == NULL || config->send == NULL ||
        config->receive == NULL || config->initial_window == 0 ||
        config->initial_window > config->max_window ||
        config->max_window > RNS_CHANNEL_MAX_WINDOW || config->retry_timeout <= 0.0) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    memset(channel, 0, sizeof(*channel));
    channel->config = *config;
    channel->window = config->initial_window;
    return RNS_OK;
}

rns_status_t rns_channel_send(rns_channel_t *channel, uint16_t message_type,
                              const uint8_t *payload, size_t payload_length,
                              uint16_t *sequence) {
    rns_channel_slot_t *slot = NULL;
    size_t encoded_length = 0;
    rns_status_t status;
    if (channel == NULL || sequence == NULL ||
        (payload_length != 0 && payload == NULL)) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    if (channel->outstanding >= channel->window) {
        return RNS_ERROR_OVERFLOW;
    }
    for (size_t i = 0; i < RNS_CHANNEL_MAX_WINDOW; ++i) {
        if (!channel->transmit[i].used) {
            slot = &channel->transmit[i];
            break;
        }
    }
    if (slot == NULL) {
        return RNS_ERROR_OVERFLOW;
    }
    status = rns_channel_envelope_encode(message_type, channel->next_sequence, payload,
                                         payload_length, slot->envelope,
                                         sizeof(slot->envelope), &encoded_length);
    if (status != RNS_OK) {
        return status;
    }
    status = channel->config.send(slot->envelope, encoded_length, channel->config.send_context);
    if (status != RNS_OK) {
        memset(slot, 0, sizeof(*slot));
        return status;
    }
    slot->used = 1;
    slot->sequence = channel->next_sequence;
    slot->length = (uint16_t)encoded_length;
    slot->deadline = channel->config.clock(channel->config.clock_context) +
                     channel->config.retry_timeout;
    *sequence = channel->next_sequence++;
    channel->outstanding++;
    return RNS_OK;
}

rns_status_t rns_channel_mark_delivered(rns_channel_t *channel, uint16_t sequence) {
    if (channel == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < RNS_CHANNEL_MAX_WINDOW; ++i) {
        rns_channel_slot_t *slot = &channel->transmit[i];
        if (slot->used && slot->sequence == sequence) {
            memset(slot, 0, sizeof(*slot));
            channel->outstanding--;
            channel->success_count++;
            if (channel->success_count >= channel->window &&
                channel->window < channel->config.max_window) {
                channel->window++;
                channel->success_count = 0;
            }
            event(channel, RNS_CHANNEL_EVENT_DELIVERED, sequence);
            return RNS_OK;
        }
    }
    return RNS_ERROR_NOT_FOUND;
}

static rns_status_t drain_receive(rns_channel_t *channel) {
    for (;;) {
        rns_channel_slot_t *found = NULL;
        for (size_t i = 0; i < RNS_CHANNEL_MAX_WINDOW; ++i) {
            if (channel->receive[i].used &&
                channel->receive[i].sequence == channel->expected_sequence) {
                found = &channel->receive[i];
                break;
            }
        }
        if (found == NULL) {
            return RNS_OK;
        }
        {
            const uint8_t *payload;
            size_t payload_length;
            uint16_t message_type;
            uint16_t sequence;
            rns_status_t status = rns_channel_envelope_decode(
                found->envelope, found->length, &message_type, &sequence, &payload,
                &payload_length);
            if (status != RNS_OK) {
                return status;
            }
            status = channel->config.receive(message_type, sequence, payload, payload_length,
                                             channel->config.receive_context);
            if (status != RNS_OK) {
                return status;
            }
        }
        memset(found, 0, sizeof(*found));
        channel->expected_sequence++;
    }
}

rns_status_t rns_channel_receive(rns_channel_t *channel, const uint8_t *envelope,
                                 size_t envelope_length) {
    const uint8_t *payload;
    size_t payload_length;
    uint16_t message_type;
    uint16_t sequence;
    uint16_t distance;
    rns_channel_slot_t *slot = NULL;
    rns_status_t status;
    if (channel == NULL) {
        return RNS_ERROR_INVALID_ARGUMENT;
    }
    status = rns_channel_envelope_decode(envelope, envelope_length, &message_type, &sequence,
                                         &payload, &payload_length);
    if (status != RNS_OK) {
        return status;
    }
    (void)message_type;
    (void)payload;
    (void)payload_length;
    if (sequence_before(sequence, channel->expected_sequence)) {
        event(channel, RNS_CHANNEL_EVENT_DUPLICATE, sequence);
        return RNS_OK;
    }
    distance = sequence_distance(sequence, channel->expected_sequence);
    if (distance >= channel->config.max_window) {
        return RNS_ERROR_PROTOCOL;
    }
    for (size_t i = 0; i < RNS_CHANNEL_MAX_WINDOW; ++i) {
        if (channel->receive[i].used && channel->receive[i].sequence == sequence) {
            event(channel, RNS_CHANNEL_EVENT_DUPLICATE, sequence);
            return RNS_OK;
        }
        if (slot == NULL && !channel->receive[i].used) {
            slot = &channel->receive[i];
        }
    }
    if (slot == NULL) {
        return RNS_ERROR_OVERFLOW;
    }
    slot->used = 1;
    slot->sequence = sequence;
    slot->length = (uint16_t)envelope_length;
    memcpy(slot->envelope, envelope, envelope_length);
    if (distance != 0) {
        event(channel, RNS_CHANNEL_EVENT_OUT_OF_ORDER, sequence);
    }
    return drain_receive(channel);
}

size_t rns_channel_tick(rns_channel_t *channel) {
    size_t actions = 0;
    double now;
    if (channel == NULL || channel->config.clock == NULL) {
        return 0;
    }
    now = channel->config.clock(channel->config.clock_context);
    for (size_t i = 0; i < RNS_CHANNEL_MAX_WINDOW; ++i) {
        rns_channel_slot_t *slot = &channel->transmit[i];
        if (!slot->used || now < slot->deadline) {
            continue;
        }
        if (slot->retries >= channel->config.max_retries) {
            uint16_t sequence = slot->sequence;
            memset(slot, 0, sizeof(*slot));
            channel->outstanding--;
            channel->window = channel->window > 1 ? (uint8_t)(channel->window / 2) : 1;
            if (channel->window > channel->config.max_window) {
                channel->window = channel->config.max_window;
            }
            channel->success_count = 0;
            event(channel, RNS_CHANNEL_EVENT_TIMEOUT, sequence);
            actions++;
            continue;
        }
        if (channel->config.send(slot->envelope, slot->length,
                                 channel->config.send_context) == RNS_OK) {
            slot->retries++;
            slot->deadline = now + channel->config.retry_timeout;
            event(channel, RNS_CHANNEL_EVENT_RETRY, slot->sequence);
            actions++;
        } else {
            /* A local send failure must not create a hot retry loop. */
            slot->deadline = now + channel->config.retry_timeout;
        }
    }
    return actions;
}
