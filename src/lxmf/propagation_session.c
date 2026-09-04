#include "reticulum/lxmf_propagation_session.h"
#include "reticulum/crypto.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PN_BATCH 8u
struct lxmf_pn_session {
    lxmf_pn_session_options_t options;
    rns_identity local, node;
    rns_runtime_link_t *link;
    rns_request_receipt_t *request;
    rns_runtime_resource_transfer_t *transfer;
    lxmf_pn_session_progress_t progress;
    uint8_t ids[LXMF_PN_MAX_ITEMS][32], haves[PN_BATCH][32];
    size_t offset, batch, have_count;
    uint8_t *upload;
    size_t upload_length;
    double deadline;
    bool uploading, response_ready;
};

static bool terminal(lxmf_pn_session_state_t state) {
    return state == LXMF_PN_IDLE || state >= LXMF_PN_COMPLETE;
}

static void transition(lxmf_pn_session_t *s, lxmf_pn_session_state_t state,
                       rns_status_t error) {
    s->progress.state = state;
    s->progress.error = error;
    if (s->options.state_callback != NULL)
        s->options.state_callback(&s->progress, s->options.callback_context);
}

static void cleanup(lxmf_pn_session_t *s) {
    rns_request_receipt_destroy(s->request); s->request = NULL;
    rns_runtime_resource_transfer_destroy(s->transfer); s->transfer = NULL;
    rns_runtime_link_destroy(s->link); s->link = NULL;
    free(s->upload); s->upload = NULL; s->upload_length = 0;
}

rns_status_t lxmf_pn_session_create(lxmf_pn_session_t **output,
    const lxmf_pn_session_options_t *options) {
    uint8_t destination[16];
    const char *aspects[] = {"propagation"};
    if (output == NULL || options == NULL || options->runtime == NULL ||
        options->local_identity == NULL || options->node_identity == NULL ||
        !options->local_identity->has_private ||
        !isfinite(options->timeout_seconds) || options->timeout_seconds < 0 ||
        options->max_response_size > LXMF_PN_MAX_WIRE)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (rns_destination_hash(options->node_identity, "lxmf", aspects, 1,
                             destination) == 0 ||
        memcmp(destination, options->node_destination, 16) != 0)
        return RNS_ERROR_INVALID_ARGUMENT;
    lxmf_pn_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) return RNS_ERROR_NO_MEMORY;
    s->options = *options;
    s->local = *options->local_identity;
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    rns_identity_export_public(options->node_identity, public_key);
    if (!rns_identity_from_public(&s->node, public_key)) {
        rns_hal_secure_zero(s, sizeof(*s)); free(s); return RNS_ERROR_CRYPTO;
    }
    s->options.local_identity = &s->local; s->options.node_identity = &s->node;
    if (s->options.timeout_seconds == 0) s->options.timeout_seconds = 60;
    if (s->options.max_response_size == 0)
        s->options.max_response_size = LXMF_PN_MAX_WIRE;
    *output = s;
    return RNS_OK;
}

void lxmf_pn_session_destroy(lxmf_pn_session_t *s) {
    if (s == NULL) return;
    cleanup(s); rns_hal_secure_zero(s, sizeof(*s)); free(s);
}

static rns_status_t start(lxmf_pn_session_t *s, double now) {
    if (s == NULL || !isfinite(now) || now < 0)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (!terminal(s->progress.state)) return RNS_ERROR_INVALID_STATE;
    cleanup(s);
    memset(&s->progress, 0, sizeof(s->progress));
    s->offset = 0; s->batch = 0; s->have_count = 0; s->response_ready = false;
    s->deadline = now + s->options.timeout_seconds;
    transition(s, LXMF_PN_PATH, RNS_OK);
    rns_path_entry path;
    if (rns_runtime_path_lookup(s->options.runtime,
            s->options.node_destination, &path) != RNS_OK)
        (void)rns_runtime_request_path(s->options.runtime,
                                     s->options.node_destination);
    return RNS_OK;
}

rns_status_t lxmf_pn_session_sync(lxmf_pn_session_t *s, double now) {
    if (s == NULL || s->options.message_callback == NULL)
        return RNS_ERROR_INVALID_ARGUMENT;
    rns_status_t status = start(s, now);
    if (status == RNS_OK) s->uploading = false;
    return status;
}

rns_status_t lxmf_pn_session_upload(lxmf_pn_session_t *s,
    const lxmf_pn_upload_t *upload, double now) {
    if (s == NULL || upload == NULL || upload->count == 0 ||
        upload->count > LXMF_PN_MAX_ITEMS || !isfinite(now) || now < 0)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (!terminal(s->progress.state)) return RNS_ERROR_INVALID_STATE;
    size_t capacity = 16;
    for (size_t i = 0; i < upload->count; ++i) {
        if (upload->messages[i].len > LXMF_PN_MAX_WIRE - 5 ||
            capacity > LXMF_PN_MAX_WIRE - 5 - upload->messages[i].len)
            return RNS_ERROR_OVERFLOW;
        capacity += upload->messages[i].len + 5;
    }
    uint8_t *data = malloc(capacity);
    if (data == NULL) return RNS_ERROR_NO_MEMORY;
    size_t length = 0;
    if (lxmf_pn_upload_encode(upload, data, capacity, &length) != LXMF_OK) {
        free(data); return RNS_ERROR_INVALID_ARGUMENT;
    }
    rns_status_t status = start(s, now);
    if (status != RNS_OK) { free(data); return status; }
    s->uploading = true; s->upload = data; s->upload_length = length;
    return RNS_OK;
}

static void response(rns_request_receipt_t *receipt, rns_request_state_t state,
    rns_status_t status, const uint8_t *data, size_t length, void *context) {
    lxmf_pn_session_t *s = context;
    (void)receipt;
    if (state == RNS_REQUEST_PENDING) return;
    if (state != RNS_REQUEST_COMPLETE) {
        transition(s, LXMF_PN_FAILED, status); return;
    }
    lxmf_pn_get_response_t result;
    if (lxmf_pn_get_response_decode(data, length,
            s->progress.state == LXMF_PN_LIST, &result) != LXMF_OK) {
        transition(s, LXMF_PN_FAILED, RNS_ERROR_INVALID_ARGUMENT); return;
    }
    if (result.kind != LXMF_PN_RESPONSE_ITEMS) {
        s->progress.remote_error = result.error;
        transition(s, LXMF_PN_FAILED, RNS_ERROR_INVALID_STATE); return;
    }
    if (s->progress.state == LXMF_PN_LIST) {
        for (size_t i = 0; i < result.count; ++i) {
            for (size_t j = 0; j < i; ++j)
                if (memcmp(result.items[i].data, result.items[j].data, 32) == 0) {
                    transition(s, LXMF_PN_FAILED, RNS_ERROR_INVALID_ARGUMENT);
                    return;
                }
            memcpy(s->ids[i], result.items[i].data, 32);
        }
        s->progress.available = result.count;
    } else if (s->progress.state == LXMF_PN_DOWNLOAD) {
        uint8_t hashes[PN_BATCH][32];
        if (result.count > s->batch) {
            transition(s, LXMF_PN_FAILED, RNS_ERROR_INVALID_ARGUMENT); return;
        }
        /* Validate the entire response before any persistence callback. */
        for (size_t i = 0; i < result.count; ++i) {
            bool wanted = false;
            if (!rns_sha256(result.items[i].data, result.items[i].len,
                           hashes[i])) {
                transition(s, LXMF_PN_FAILED, RNS_ERROR_CRYPTO); return;
            }
            for (size_t j = 0; j < s->batch; ++j)
                if (memcmp(hashes[i], s->ids[s->offset + j], 32) == 0)
                    wanted = true;
            for (size_t j = 0; j < i; ++j)
                if (memcmp(hashes[i], hashes[j], 32) == 0) wanted = false;
            if (!wanted) {
                transition(s, LXMF_PN_FAILED, RNS_ERROR_INVALID_ARGUMENT); return;
            }
        }
        s->have_count = 0;
        for (size_t i = 0; i < result.count; ++i) {
            if (s->options.message_callback(hashes[i], result.items[i].data,
                    result.items[i].len, s->options.callback_context)) {
                memcpy(s->haves[s->have_count++], hashes[i], 32);
                s->progress.received++;
            }
        }
    } else if (s->progress.state == LXMF_PN_ACK) {
        if (result.count != 0) {
            transition(s, LXMF_PN_FAILED, RNS_ERROR_INVALID_ARGUMENT); return;
        }
        s->progress.acknowledged += s->have_count;
    }
    s->response_ready = true;
}

static rns_status_t send_get(lxmf_pn_session_t *s,
    lxmf_pn_session_state_t state, double now) {
    lxmf_pn_get_request_t get = {0};
    uint8_t encoded[640]; size_t length;
    get.wants_null = state != LXMF_PN_DOWNLOAD;
    get.haves_null = state != LXMF_PN_ACK;
    if (state == LXMF_PN_DOWNLOAD) {
        s->batch = s->progress.available - s->offset;
        if (s->batch > PN_BATCH) s->batch = PN_BATCH;
        get.wants_count = s->batch;
        for (size_t i = 0; i < s->batch; ++i)
            get.wants[i] = (lxmf_slice_t){s->ids[s->offset + i], 32};
        get.has_limit = true; get.limit_kb.is_float = true;
        get.limit_kb.real = (double)s->options.max_response_size / 1000.0;
    } else if (state == LXMF_PN_ACK) {
        get.haves_count = s->have_count;
        for (size_t i = 0; i < s->have_count; ++i)
            get.haves[i] = (lxmf_slice_t){s->haves[i], 32};
    }
    if (lxmf_pn_get_request_encode(&get, encoded, sizeof(encoded), &length)
        != LXMF_OK) return RNS_ERROR_OVERFLOW;
    rns_request_options_t options = {0};
    options.timeout_seconds = s->options.timeout_seconds;
    options.max_response_size = s->options.max_response_size;
    options.callback = response; options.callback_context = s;
    transition(s, state, RNS_OK);
    s->deadline = now + s->options.timeout_seconds;
    return rns_runtime_link_request(s->link, LXMF_PN_GET_PATH, encoded,
                                    length, &options, &s->request);
}

rns_status_t lxmf_pn_session_poll(lxmf_pn_session_t *s, double now) {
    if (s == NULL || !isfinite(now) || now < 0)
        return RNS_ERROR_INVALID_ARGUMENT;
    if (terminal(s->progress.state)) { cleanup(s); return RNS_OK; }
    rns_status_t status = RNS_OK;
    if (now >= s->deadline) status = RNS_ERROR_TIMEOUT;
    else if (s->progress.state == LXMF_PN_PATH) {
        rns_path_entry path;
        if (rns_runtime_path_lookup(s->options.runtime,
                s->options.node_destination, &path) != RNS_OK) return RNS_OK;
        rns_runtime_link_options_t options = {0};
        options.timeout_seconds = s->options.timeout_seconds;
        status = rns_runtime_link_open(s->options.runtime,
            s->options.node_destination, &s->node, &options, &s->link);
        if (status == RNS_OK) {
            transition(s, LXMF_PN_LINK, RNS_OK);
            s->deadline = now + s->options.timeout_seconds;
        }
    } else if (rns_runtime_link_state(s->link) == RNS_LINK_CLOSED) {
        status = RNS_ERROR_INVALID_STATE;
    } else if (s->progress.state == LXMF_PN_LINK &&
               rns_runtime_link_state(s->link) == RNS_LINK_ACTIVE) {
        status = rns_runtime_link_identify(s->link, &s->local);
        if (status == RNS_OK && s->uploading) {
            rns_runtime_resource_options_t options = {0};
            options.timeout_seconds = s->options.timeout_seconds;
            status = rns_runtime_link_send_resource(s->link, s->upload,
                s->upload_length, &options, &s->transfer);
            if (status == RNS_OK) {
                transition(s, LXMF_PN_UPLOAD, RNS_OK);
                s->deadline = now + s->options.timeout_seconds;
            }
        } else if (status == RNS_OK) status = send_get(s, LXMF_PN_LIST, now);
    } else if (s->progress.state == LXMF_PN_UPLOAD) {
        rns_runtime_resource_state_t state =
            rns_runtime_resource_transfer_state(s->transfer);
        size_t previous_parts = s->progress.transferred_parts;
        s->progress.transferred_parts =
            rns_runtime_resource_transfer_sent_parts(s->transfer);
        s->progress.total_parts =
            rns_runtime_resource_transfer_total_parts(s->transfer);
        if (state == RNS_RUNTIME_RESOURCE_COMPLETE)
            transition(s, LXMF_PN_COMPLETE, RNS_OK);
        else if (state >= RNS_RUNTIME_RESOURCE_REJECTED)
            status = RNS_ERROR_INVALID_STATE;
        else if (previous_parts != s->progress.transferred_parts)
            transition(s, LXMF_PN_UPLOAD, RNS_OK);
    } else if (s->response_ready) {
        s->response_ready = false;
        rns_request_receipt_destroy(s->request); s->request = NULL;
        if (s->progress.state == LXMF_PN_DOWNLOAD && s->have_count > 0 &&
            !s->options.retain_on_node) status = send_get(s, LXMF_PN_ACK, now);
        else {
            if (s->progress.state != LXMF_PN_LIST) s->offset += s->batch;
            if (s->offset >= s->progress.available)
                transition(s, LXMF_PN_COMPLETE, RNS_OK);
            else status = send_get(s, LXMF_PN_DOWNLOAD, now);
        }
    }
    if (status != RNS_OK) transition(s, LXMF_PN_FAILED, status);
    if (terminal(s->progress.state)) cleanup(s);
    return status;
}

void lxmf_pn_session_cancel(lxmf_pn_session_t *s) {
    if (s == NULL || terminal(s->progress.state)) return;
    cleanup(s); transition(s, LXMF_PN_CANCELLED, RNS_OK);
}

const lxmf_pn_session_progress_t *lxmf_pn_session_progress(
    const lxmf_pn_session_t *s) { return s != NULL ? &s->progress : NULL; }
