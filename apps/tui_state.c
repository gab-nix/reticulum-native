#include "tui_state.h"

#include "tui_text.h"
#include "tui_paths.h"

#include "reticulum/config.h"
#include "reticulum/destination.h"
#include "reticulum/hal.h"
#include "reticulum/lxmf.h"

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TUI_ROUTER_INTERVAL_MS 1000u
#define TUI_ROUTER_BATCH 2u
#define TUI_RUNTIME_BATCH 32u
#define TUI_ANNOUNCE_RETRY_MS 1000u
#define TUI_NODE_LIFETIME 3600.0

static void apply_propagation_route(tui_state_t *state);

static const uint8_t *message_peer(const tui_state_t *state,
                                   const lxmf_store_message_t *message) {
    return memcmp(message->source, state->local, LXMF_DESTINATION_LENGTH) == 0
               ? message->destination : message->source;
}

static size_t contact_index(const tui_state_t *state,
                            const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
    for (size_t i = 0u; i < state->contact_count; ++i)
        if (memcmp(state->contacts[i].peer, peer, LXMF_DESTINATION_LENGTH) == 0) return i;
    return state->contact_count;
}

static void copy_preview(lxmf_slice_t input, char *output, size_t capacity) {
    if (capacity == 0u) return;
    (void)tui_text_sanitize(input.data, input.len, output, capacity);
}

static void copy_media_metadata(const lxmf_media_view_t *media,
                                lxmf_media_format_kind_t *kind,
                                uint8_t *integer_format, char *text_format,
                                size_t text_capacity, size_t *size) {
    *kind = media->format_kind;
    *integer_format = media->integer_format;
    *size = media->data.len;
    text_format[0] = '\0';
    if (media->format_kind == LXMF_MEDIA_FORMAT_TEXT)
        copy_preview(media->text_format, text_format, text_capacity);
}

static void metadata_from_fields(tui_message_metadata_t *metadata,
                                 const lxmf_standard_fields_t *fields) {
    metadata->state = fields->present_mask == 0u ? TUI_METADATA_NONE
                                                 : TUI_METADATA_AVAILABLE;
    metadata->present_mask = fields->present_mask;
    metadata->renderer = fields->renderer;
    memcpy(metadata->reply_to, fields->reply_to, sizeof metadata->reply_to);
    copy_preview(fields->reply_quote, metadata->reply_quote,
                 sizeof metadata->reply_quote);
    memcpy(metadata->reaction_to, fields->reaction_to,
           sizeof metadata->reaction_to);
    copy_preview(fields->reaction_content, metadata->reaction,
                 sizeof metadata->reaction);
    memcpy(metadata->thread, fields->thread, sizeof metadata->thread);
    metadata->attachment_count = fields->attachment_count;
    for (size_t i = 0u; i < fields->attachment_count; ++i) {
        tui_attachment_metadata_t *attachment = &metadata->attachments[i];
        copy_preview(fields->attachments[i].name, attachment->display_name,
                     sizeof attachment->display_name);
        size_t safe_length = 0u;
        if (lxmf_attachment_safe_name(
                fields->attachments[i].name, (uint8_t *)attachment->safe_name,
                sizeof attachment->safe_name - 1u, &safe_length) == LXMF_OK)
            attachment->safe_name[safe_length] = '\0';
        attachment->size = fields->attachments[i].data.len;
    }
    copy_media_metadata(&fields->image, &metadata->image_format_kind,
                        &metadata->image_integer_format,
                        metadata->image_text_format,
                        sizeof metadata->image_text_format,
                        &metadata->image_size);
    copy_media_metadata(&fields->audio, &metadata->audio_format_kind,
                        &metadata->audio_integer_format,
                        metadata->audio_text_format,
                        sizeof metadata->audio_text_format,
                        &metadata->audio_size);
}

static lxmf_status_t read_message_fields(tui_state_t *state,
                                         const lxmf_store_message_t *message,
                                         uint8_t **owned_packed,
                                         bool *had_packed,
                                         lxmf_standard_fields_t *fields) {
    *owned_packed = NULL;
    *had_packed = message->packed.data != NULL && message->packed.len != 0u;
    const uint8_t *packed = message->packed.data;
    size_t packed_length = message->packed.len;
    if (packed == NULL || packed_length == 0u) {
        lxmf_status_t status = lxmf_store_packed_size(
            &state->store, message->message_id, &packed_length);
        if (status != LXMF_OK) return status;
        if (packed_length == 0u || packed_length > LXMF_STORE_MAX_PACKED)
            return LXMF_ERR_BOUNDS;
        *owned_packed = malloc(packed_length);
        if (*owned_packed == NULL) return LXMF_ERR_BOUNDS;
        size_t actual = 0u;
        status = lxmf_store_read_packed(&state->store, message->message_id,
                                        *owned_packed, packed_length, &actual);
        if (status != LXMF_OK || actual != packed_length) {
            free(*owned_packed);
            *owned_packed = NULL;
            return status != LXMF_OK ? status : LXMF_ERR_FORMAT;
        }
        packed = *owned_packed;
        *had_packed = true;
    }
    lxmf_message_t decoded;
    lxmf_status_t status = lxmf_unpack(packed, packed_length, NULL, NULL,
                                       &decoded);
    if (status == LXMF_OK)
        status = lxmf_standard_fields_parse(decoded.fields_msgpack.data,
                                            decoded.fields_msgpack.len, fields);
    return status;
}

static void load_message_metadata(tui_state_t *state, tui_message_t *copy,
                                  const lxmf_store_message_t *message) {
    uint8_t *owned = NULL;
    bool had_packed = false;
    lxmf_standard_fields_t fields;
    lxmf_status_t status = read_message_fields(state, message, &owned,
                                               &had_packed, &fields);
    if (status == LXMF_OK)
        metadata_from_fields(&copy->metadata, &fields);
    else if (status == LXMF_ERR_FORMAT)
        copy->metadata.state = had_packed ? TUI_METADATA_MALFORMED
                                         : TUI_METADATA_MISSING_PACKED;
    else
        copy->metadata.state = TUI_METADATA_UNAVAILABLE;
    free(owned);
}

/* Returns the contact index, or contact_count when the table is full. */
static size_t ensure_contact(tui_state_t *state,
                             const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
    size_t index = contact_index(state, peer);
    if (index < state->contact_count) return index;
    if (state->contact_count >= TUI_MAX_CONTACTS) return state->contact_count;
    tui_contact_t *contact = &state->contacts[state->contact_count];
    memset(contact, 0, sizeof *contact);
    memcpy(contact->peer, peer, LXMF_DESTINATION_LENGTH);
    contact->trust = TUI_TRUST_UNKNOWN;
    state->filter_dirty = true;
    return state->contact_count++;
}

static void restore_selected_draft(tui_state_t *state) {
    tui_editor_clear(&state->composer);
    if (state->selected >= state->contact_count) return;
    const tui_contact_t *contact = &state->contacts[state->selected];
    size_t length = contact->draft_len;
    if (length > TUI_COMPOSER_CAPACITY) length = TUI_COMPOSER_CAPACITY;
    (void)tui_editor_insert(&state->composer, contact->draft, length);
}

void tui_state_cancel_reference(tui_state_t *state) {
    if (state == NULL) return;
    rns_hal_secure_zero(&state->compose_reference,
                        sizeof state->compose_reference);
    tui_editor_clear(&state->reaction);
    if (state->field == TUI_FIELD_REACTION)
        state->field = TUI_FIELD_NONE;
}

static size_t copy_quote_utf8(lxmf_slice_t input, uint8_t *output,
                              size_t capacity) {
    size_t read = 0u;
    size_t written = 0u;
    if (output == NULL || (input.data == NULL && input.len != 0u)) return 0u;
    while (read < input.len && written < capacity) {
        uint8_t byte = input.data[read];
        if (byte < 0x80u) {
            output[written++] = byte < 0x20u || byte == 0x7fu ? ' ' : byte;
            ++read;
            continue;
        }
        size_t width = tui_utf8_length(input.data + read, input.len - read);
        if (width == 0u) {
            output[written++] = '?';
            ++read;
        } else if (width <= capacity - written) {
            memcpy(output + written, input.data + read, width);
            written += width;
            read += width;
        } else {
            break;
        }
    }
    return written;
}

static const tui_message_t *viewport_reference_message(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return NULL;
    tui_state_refresh(state);
    size_t end = state->thread_count > state->scroll
                     ? state->thread_count - state->scroll : 0u;
    if (state->thread_layout_valid && state->thread_count != 0u)
        return tui_state_thread_message(state, state->thread_visible_last);
    return end == 0u ? NULL : tui_state_thread_message(state, end - 1u);
}

static bool begin_reference(tui_state_t *state,
                            tui_compose_reference_kind_t kind) {
    const tui_message_t *message = viewport_reference_message(state);
    if (message == NULL) {
        tui_state_set_status(state, "No visible message to reference");
        return false;
    }
    tui_state_cancel_reference(state);
    state->compose_reference.kind = kind;
    memcpy(state->compose_reference.peer, state->contacts[state->selected].peer,
           LXMF_DESTINATION_LENGTH);
    memcpy(state->compose_reference.message_id, message->value.message_id,
           LXMF_MESSAGE_ID_LENGTH);
    state->compose_reference.quote_length = copy_quote_utf8(
        message->value.content, state->compose_reference.quote,
        sizeof state->compose_reference.quote);
    copy_preview(message->value.content, state->compose_reference.preview,
                 sizeof state->compose_reference.preview);
    state->field = kind == TUI_COMPOSE_REFERENCE_REACTION
                       ? TUI_FIELD_REACTION : TUI_FIELD_COMPOSE;
    tui_state_set_status(state, "%s target selected: %.8s",
                         kind == TUI_COMPOSE_REFERENCE_REACTION
                             ? "Reaction" : "Reply",
                         state->compose_reference.preview);
    return true;
}

bool tui_state_begin_reply(tui_state_t *state) {
    return begin_reference(state, TUI_COMPOSE_REFERENCE_REPLY);
}

bool tui_state_begin_reaction(tui_state_t *state) {
    return begin_reference(state, TUI_COMPOSE_REFERENCE_REACTION);
}

/* Copies one stored message into the in-memory history and its conversation. */
static bool ingest_message(tui_state_t *state, const lxmf_store_message_t *message) {
    if (state->message_count >= TUI_MAX_MESSAGES ||
        message->content.len > LXMF_STORE_MAX_CONTENT ||
        (message->content.len != 0u && message->content.data == NULL)) return false;
    tui_message_t *copy = &state->messages[state->message_count];
    copy->value = *message;
    if (message->content.len != 0u)
        memcpy(copy->content, message->content.data, message->content.len);
    copy->value.content.data = copy->content;
    /* Full representations belong to the journal, not to this preview cache.
     * Callback and compose-buffer spans expire when their owner returns. */
    copy->value.packed = (lxmf_slice_t){NULL, 0u};
    load_message_metadata(state, copy, message);
    const uint8_t *peer = message_peer(state, message);
    size_t index = ensure_contact(state, peer);
    if (index == state->contact_count) return false;
    state->message_count++;
    tui_contact_t *contact = &state->contacts[index];
    contact->messages++;
    if (message->timestamp > contact->latest) contact->latest = message->timestamp;
    if (memcmp(message->source, state->local, LXMF_DESTINATION_LENGTH) != 0)
        contact->unread++;
    state->filter_dirty = true;
    return true;
}

void tui_state_set_status(tui_state_t *state, const char *format, ...) {
    va_list arguments;
    if (state == NULL || format == NULL) return;
    char next[TUI_STATUS_MAX];
    va_start(arguments, format);
    (void)vsnprintf(next, sizeof next, format, arguments);
    va_end(arguments);
    if (strcmp(state->status, next) == 0) return;
    (void)snprintf(state->status, sizeof state->status, "%s", next);

    bool follow_newest = state->log_count == 0u;
    if (state->log_count != 0u) {
        size_t newest = (state->log_head + state->log_count - 1u) %
                        TUI_LOG_CAPACITY;
        follow_newest = state->log_selected_sequence ==
                        state->logs[newest].sequence;
    }
    size_t slot;
    if (state->log_count < TUI_LOG_CAPACITY) {
        slot = (state->log_head + state->log_count) % TUI_LOG_CAPACITY;
        state->log_count++;
    } else {
        slot = state->log_head;
        state->log_head = (state->log_head + 1u) % TUI_LOG_CAPACITY;
    }
    state->logs[slot].sequence = ++state->log_next_sequence;
    (void)snprintf(state->logs[slot].text, sizeof state->logs[slot].text,
                   "%s", next);
    if (follow_newest || state->log_selected_sequence == 0u)
        state->log_selected_sequence = state->logs[slot].sequence;
    else if (state->log_count == TUI_LOG_CAPACITY &&
             state->log_selected_sequence < state->logs[state->log_head].sequence)
        state->log_selected_sequence = state->logs[state->log_head].sequence;
}

size_t tui_state_log_count(const tui_state_t *state) {
    return state != NULL ? state->log_count : 0u;
}

const tui_log_entry_t *tui_state_log_entry(const tui_state_t *state,
                                            size_t index) {
    if (state == NULL || index >= state->log_count) return NULL;
    return &state->logs[(state->log_head + index) % TUI_LOG_CAPACITY];
}

size_t tui_state_log_position(const tui_state_t *state) {
    if (state == NULL || state->log_count == 0u) return 0u;
    for (size_t i = 0u; i < state->log_count; ++i) {
        const tui_log_entry_t *entry = tui_state_log_entry(state, i);
        if (entry != NULL && entry->sequence == state->log_selected_sequence)
            return i;
    }
    return state->log_count - 1u;
}

void tui_state_log_move(tui_state_t *state, int delta) {
    if (state == NULL || state->log_count == 0u) return;
    size_t position = tui_state_log_position(state);
    if (delta < 0) {
        size_t amount = (size_t)(-(delta + 1)) + 1u;
        position = amount > position ? 0u : position - amount;
    } else if (delta > 0) {
        size_t amount = (size_t)delta;
        position = amount >= state->log_count - position
                       ? state->log_count - 1u
                       : position + amount;
    }
    const tui_log_entry_t *entry = tui_state_log_entry(state, position);
    if (entry != NULL) state->log_selected_sequence = entry->sequence;
}

void tui_state_log_clear(tui_state_t *state) {
    if (state == NULL) return;
    state->log_head = 0u;
    state->log_count = 0u;
    state->log_selected_sequence = 0u;
}

/* ---------------------------------------------------------------- filtering */

static bool contact_matches(const tui_state_t *state, size_t index) {
    const tui_contact_t *contact = &state->contacts[index];
    if (contact->trust != state->tab) return false;
    const char *needle = tui_editor_text(&state->search);
    if (tui_editor_empty(&state->search)) return true;
    char peer[TUI_ADDRESS_DIGITS + 1u];
    tui_hex_format(contact->peer, LXMF_DESTINATION_LENGTH, peer);
    if (tui_text_contains((const uint8_t *)peer, strlen(peer), needle) ||
        tui_text_contains((const uint8_t *)contact->note, strlen(contact->note), needle))
        return true;
    for (size_t i = 0u; i < state->message_count; ++i) {
        const lxmf_store_message_t *message = &state->messages[i].value;
        if (memcmp(message_peer(state, message), contact->peer,
                   LXMF_DESTINATION_LENGTH) == 0 &&
            tui_text_contains(message->content.data, message->content.len, needle))
            return true;
    }
    return false;
}

static bool thread_matches(const tui_state_t *state, const tui_message_t *message) {
    if (state->selected >= state->contact_count) return false;
    if (memcmp(message_peer(state, &message->value),
               state->contacts[state->selected].peer, LXMF_DESTINATION_LENGTH) != 0)
        return false;
    return tui_text_contains(message->value.content.data, message->value.content.len,
                             tui_editor_text(&state->search));
}

void tui_state_refresh(tui_state_t *state) {
    if (state == NULL || !state->filter_dirty) return;
    state->visible_count = 0u;
    for (size_t i = 0u; i < state->contact_count; ++i)
        if (contact_matches(state, i)) state->visible[state->visible_count++] = i;
    state->thread_count = 0u;
    for (size_t i = 0u; i < state->message_count; ++i)
        if (thread_matches(state, &state->messages[i]))
            state->thread[state->thread_count++] = i;
    size_t scroll_limit = state->thread_layout_valid
        ? state->thread_scroll_limit : state->thread_count;
    if (state->scroll > scroll_limit) state->scroll = scroll_limit;
    state->filter_dirty = false;
}

const tui_contact_t *tui_state_contact(const tui_state_t *state, size_t index) {
    return state != NULL && index < state->contact_count ? &state->contacts[index] : NULL;
}

const tui_contact_t *tui_state_selected_contact(const tui_state_t *state) {
    return state != NULL ? tui_state_contact(state, state->selected) : NULL;
}

size_t tui_state_thread_count(const tui_state_t *state) {
    return state != NULL ? state->thread_count : 0u;
}

const tui_message_t *tui_state_thread_message(const tui_state_t *state, size_t index) {
    if (state == NULL || index >= state->thread_count) return NULL;
    return &state->messages[state->thread[index]];
}

bool tui_state_outgoing(const tui_state_t *state, const tui_message_t *message) {
    return state != NULL && message != NULL &&
           memcmp(message->value.source, state->local, LXMF_DESTINATION_LENGTH) == 0;
}

bool tui_state_set_attachment_directory(tui_state_t *state,
                                        const char *directory) {
    if (state == NULL || directory == NULL || directory[0] != '/' ||
        strlen(directory) >= sizeof state->attachment_directory) return false;
    (void)snprintf(state->attachment_directory,
                   sizeof state->attachment_directory, "%s", directory);
    return true;
}

static bool write_attachment(tui_state_t *state, const char *name,
                             lxmf_slice_t data) {
    int directory = open(state->attachment_directory,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        tui_state_set_status(state, "Attachment directory is unavailable");
        return false;
    }
    int file = openat(directory, name,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
    if (file < 0) {
        int error = errno;
        (void)close(directory);
        tui_state_set_status(state, error == EEXIST
            ? "Attachment already exists; no file was overwritten"
            : "Could not create attachment file");
        return false;
    }
    size_t written = 0u;
    bool ok = true;
    while (written < data.len) {
        ssize_t count = write(file, data.data + written, data.len - written);
        if (count > 0) written += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else { ok = false; break; }
    }
    if (ok && fsync(file) != 0) ok = false;
    if (close(file) != 0) ok = false;
    if (!ok) {
        (void)unlinkat(directory, name, 0);
        tui_state_set_status(state,
                             "Attachment write failed; partial file removed");
    } else {
        tui_state_set_status(state, "Saved attachment %s", name);
    }
    (void)close(directory);
    return ok;
}

bool tui_state_save_latest_attachment(tui_state_t *state) {
    if (state == NULL) return false;
    if (state->attachment_directory[0] == '\0') {
        tui_state_set_status(state,
            "Set RETICULUM_ATTACHMENT_DIR before saving attachments");
        return false;
    }
    tui_state_refresh(state);
    const tui_message_t *message = NULL;
    for (size_t i = state->thread_count; i > 0u; --i) {
        const tui_message_t *candidate =
            &state->messages[state->thread[i - 1u]];
        if (candidate->metadata.attachment_count != 0u) {
            message = candidate;
            break;
        }
    }
    if (message == NULL) {
        tui_state_set_status(state, "No attachment is available in this conversation");
        return false;
    }
    uint8_t *owned = NULL;
    bool had_packed = false;
    lxmf_standard_fields_t fields;
    lxmf_status_t status = read_message_fields(state, &message->value, &owned,
                                               &had_packed, &fields);
    if (status != LXMF_OK || fields.attachment_count == 0u) {
        free(owned);
        tui_state_set_status(state, had_packed
            ? "Attachment metadata is malformed"
            : "Original packed message is unavailable; attachment cannot be saved");
        return false;
    }
    uint8_t safe[LXMF_STANDARD_MAX_NAME_BYTES + 1u];
    size_t safe_length = 0u;
    status = lxmf_attachment_safe_name(fields.attachments[0].name, safe,
                                       sizeof safe - 1u, &safe_length);
    if (status != LXMF_OK) {
        free(owned);
        tui_state_set_status(state, "Attachment filename is invalid");
        return false;
    }
    safe[safe_length] = '\0';
    bool result = write_attachment(state, (const char *)safe,
                                   fields.attachments[0].data);
    free(owned);
    return result;
}

/* -------------------------------------------------------------- persistence */

static bool load_identity(const char *path, rns_identity *identity) {
    uint8_t bytes[64];
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t count = fread(bytes, 1u, sizeof bytes, file);
    int extra = fgetc(file);
    (void)fclose(file);
    return count == sizeof bytes && extra == EOF &&
           rns_identity_from_private(identity, bytes);
}

static lxmf_peer_trust_t to_store_trust(tui_trust_t trust) {
    if (trust == TUI_TRUST_TRUSTED) return LXMF_PEER_TRUST_TRUSTED;
    if (trust == TUI_TRUST_UNTRUSTED) return LXMF_PEER_TRUST_UNTRUSTED;
    return LXMF_PEER_TRUST_UNKNOWN;
}

static tui_trust_t from_store_trust(lxmf_peer_trust_t trust) {
    if (trust == LXMF_PEER_TRUST_TRUSTED) return TUI_TRUST_TRUSTED;
    if (trust == LXMF_PEER_TRUST_UNTRUSTED) return TUI_TRUST_UNTRUSTED;
    return TUI_TRUST_UNKNOWN;
}

static bool load_message(void *context, const lxmf_store_message_t *message) {
    return ingest_message(context, message);
}

static bool load_peer(void *context, const lxmf_peer_t *peer) {
    tui_state_t *state = context;
    size_t index = ensure_contact(state, peer->address);
    if (index == state->contact_count) return true;
    tui_contact_t *contact = &state->contacts[index];
    contact->trust = from_store_trust(peer->trust);
    contact->pinned = peer->pinned;
    contact->blocked = peer->blocked;
    contact->unread = peer->unread_count;
    contact->draft_len = peer->draft_len;
    if (contact->draft_len > LXMF_PEER_DRAFT_MAX)
        contact->draft_len = LXMF_PEER_DRAFT_MAX;
    memcpy(contact->draft, peer->draft, contact->draft_len);
    contact->draft[contact->draft_len] = '\0';
    size_t note_length = peer->note_len;
    if (note_length >= sizeof contact->note) note_length = sizeof contact->note - 1u;
    memcpy(contact->note, peer->note, note_length);
    contact->note[note_length] = '\0';
    state->filter_dirty = true;
    return true;
}

void tui_state_persist_contacts(tui_state_t *state) {
    if (state == NULL || state->peer_store.implementation == NULL) return;
    for (size_t i = 0u; i < state->contact_count; ++i) {
        const tui_contact_t *contact = &state->contacts[i];
        lxmf_peer_t peer = {0};
        bool inserted = false;
        lxmf_status_t status = lxmf_peer_store_get(&state->peer_store,
                                                   contact->peer, &peer);
        if (status != LXMF_OK && status != LXMF_ERR_FORMAT) return;
        if (status == LXMF_ERR_FORMAT)
            memcpy(peer.address, contact->peer, sizeof peer.address);
        peer.trust = to_store_trust(contact->trust);
        peer.blocked = contact->blocked;
        peer.pinned = contact->pinned;
        peer.unread_count = contact->unread > UINT32_MAX ? UINT32_MAX
                                                         : (uint32_t)contact->unread;
        peer.note_len = strlen(contact->note);
        memcpy(peer.note, contact->note, peer.note_len);
        if (contact->draft_dirty) {
            peer.draft_len = contact->draft_len;
            memcpy(peer.draft, contact->draft, peer.draft_len);
            peer.draft[peer.draft_len] = '\0';
        }
        if (lxmf_peer_store_put(&state->peer_store, &peer, &inserted) != LXMF_OK) return;
    }
    (void)lxmf_peer_store_save(&state->peer_store);
}

/* ------------------------------------------------------------ runtime hooks */

static tui_message_t *find_message(tui_state_t *state,
                                   const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                                   size_t *position) {
    for (size_t i = 0u; i < state->message_count; ++i) {
        if (memcmp(state->messages[i].value.message_id, id,
                   LXMF_MESSAGE_ID_LENGTH) != 0)
            continue;
        if (position != NULL) *position = i;
        return &state->messages[i];
    }
    return NULL;
}

static void recalculate_contact(tui_state_t *state, size_t contact) {
    tui_contact_t *entry = &state->contacts[contact];
    entry->messages = 0u;
    entry->latest = 0.0;
    for (size_t i = 0u; i < state->message_count; ++i) {
        const lxmf_store_message_t *message = &state->messages[i].value;
        if (memcmp(message_peer(state, message), entry->peer,
                   LXMF_DESTINATION_LENGTH) != 0)
            continue;
        entry->messages++;
        if (message->timestamp > entry->latest) entry->latest = message->timestamp;
    }
}

static void remove_cached_message(tui_state_t *state, size_t position) {
    uint8_t peer[LXMF_DESTINATION_LENGTH];
    lxmf_store_message_t *value = &state->messages[position].value;
    memcpy(peer, message_peer(state, value), sizeof peer);
    bool incoming = memcmp(value->source, state->local,
                           LXMF_DESTINATION_LENGTH) != 0;
    if (position + 1u < state->message_count)
        memmove(&state->messages[position], &state->messages[position + 1u],
                (state->message_count - position - 1u) * sizeof state->messages[0]);
    state->message_count--;
    /* memmove relocates the inline buffers but not the pointers into them. */
    for (size_t i = position; i < state->message_count; ++i)
        state->messages[i].value.content.data = state->messages[i].content;
    memset(&state->messages[state->message_count], 0, sizeof state->messages[0]);
    size_t contact = contact_index(state, peer);
    if (contact < state->contact_count) {
        if (incoming && state->contacts[contact].unread > 0u)
            state->contacts[contact].unread--;
        recalculate_contact(state, contact);
    }
    state->filter_dirty = true;
}

void tui_state_apply_router_event(tui_state_t *state,
                                  const lxmf_router_event_t *event) {
    if (state == NULL || event == NULL) return;
    size_t position = 0u;
    tui_message_t *message = find_message(state, event->message_id, &position);
    if (event->state == LXMF_DELIVERY_FAILED &&
        (event->result == LXMF_ERR_BLOCKED || event->result == LXMF_ERR_BOUNDS) &&
        (message == NULL ||
         memcmp(message->value.source, state->local, LXMF_SOURCE_LENGTH) != 0)) {
        /* The router removed a deferred message when policy changed. Reflect
         * that removal without calling a blocked sender's signature invalid. */
        if (message != NULL &&
            message->value.signature_state == LXMF_SIGNATURE_UNVERIFIED)
            remove_cached_message(state, position);
        tui_state_set_status(state, event->result == LXMF_ERR_BLOCKED
                                      ? "Incoming message blocked by saved preference"
                                      : "Incoming message exceeds the configured size limit");
        return;
    }
    if (message != NULL) {
        message->value.status = event->state;
        state->filter_dirty = true;
    }
    if (event->state == LXMF_DELIVERY_QUEUED) {
        /* Background discovery for an old conversation is not the result of
         * the user's current Network/Browser/Settings action. */
        if (state->screen != TUI_SCREEN_CONVERSATIONS) return;
        const tui_contact_t *selected = tui_state_selected_contact(state);
        if (message == NULL || selected == NULL ||
            memcmp(selected->peer, message_peer(state, &message->value),
                   LXMF_DESTINATION_LENGTH) != 0) return;
        lxmf_stamp_job_progress_t progress;
        if (event->queue_reason == LXMF_QUEUE_REASON_STAMP &&
            state->router_ready &&
            lxmf_router_stamp_progress(&state->router, event->message_id,
                                       &progress) == LXMF_OK) {
            if (progress.prepared_rounds < LXMF_STAMP_WORKBLOCK_ROUNDS)
                tui_state_set_status(
                    state, "Preparing delivery stamp: %u%%",
                    (unsigned)(progress.prepared_rounds * 100u /
                               LXMF_STAMP_WORKBLOCK_ROUNDS));
            else
                tui_state_set_status(
                    state, "Searching for delivery stamp; %llu attempts",
                    (unsigned long long)progress.attempts);
        } else if (event->method == LXMF_DELIVERY_METHOD_PROPAGATED &&
                   event->queue_reason == LXMF_QUEUE_REASON_PEER_IDENTITY)
            tui_state_set_status(state,
                "Queued via propagated; need recipient identity (requesting announce)");
        else
            tui_state_set_status(state, "Queued via %s; waiting for %s",
                                 lxmf_delivery_method_string(event->method),
                                 lxmf_queue_reason_string(event->queue_reason));
    } else if (event->state == LXMF_DELIVERY_SENDING) {
        if (event->queue_reason == LXMF_QUEUE_REASON_STORAGE)
            tui_state_set_status(state,
                "Upload completed; cannot save state (%s). Waiting for storage recovery",
                lxmf_status_string(event->result));
        else
            tui_state_set_status(state, "Sending via %s",
                                 lxmf_delivery_method_string(event->method));
    } else if (event->state == LXMF_DELIVERY_SENT) {
        if (event->method == LXMF_DELIVERY_METHOD_PROPAGATED)
            tui_state_set_status(state,
                "Uploaded to propagation node; recipient delivery is not yet confirmed");
        else
            tui_state_set_status(state, "Sent via %s; awaiting delivery proof",
                                 lxmf_delivery_method_string(event->method));
    } else if (event->state == LXMF_DELIVERY_DELIVERED) {
        tui_state_set_status(state, "Delivered via %s",
                             lxmf_delivery_method_string(event->method));
    } else {
        if (event->queue_reason == LXMF_QUEUE_REASON_IDENTITY_TIMEOUT)
            tui_state_set_status(state,
                "Recipient identity discovery timed out; message kept. Retry after an announce or path refresh.");
        else if (event->method == LXMF_DELIVERY_METHOD_PROPAGATED &&
            event->queue_reason == LXMF_QUEUE_REASON_RETRY_EXHAUSTED)
            tui_state_set_status(state,
                "Propagation upload stopped after bounded retries: %s",
                lxmf_status_string(event->result));
        else
            tui_state_set_status(state, "Delivery via %s failed: %s",
                                 lxmf_delivery_method_string(event->method),
                                 lxmf_status_string(event->result));
    }
}

void tui_state_apply_signature(tui_state_t *state,
                               const uint8_t id[LXMF_MESSAGE_ID_LENGTH],
                               lxmf_signature_state_t signature) {
    size_t position = 0u;
    if (state == NULL || id == NULL) return;
    tui_message_t *message = find_message(state, id, &position);
    if (message == NULL) return;
    if (signature != LXMF_SIGNATURE_FAILED) {
        message->value.signature_state = signature;
        state->filter_dirty = true;
        tui_state_set_status(state, signature == LXMF_SIGNATURE_VERIFIED
                                        ? "Previously unverified message is now verified"
                                        : "Message sender is not verified yet");
        return;
    }
    remove_cached_message(state, position);
    tui_state_set_status(state, "Rejected message with an invalid signature");
}

static void on_delivery(void *context,
                        const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
                        lxmf_delivery_status_t status, lxmf_status_t result) {
    tui_state_t *state = context;
    tui_message_t *message = find_message(state, message_id, NULL);
    if (message != NULL) message->value.status = status;
    state->send_attempted = true;
    state->send_ok = result == LXMF_OK;
    state->filter_dirty = true;
}

static void on_router_event(void *context, const lxmf_router_event_t *event) {
    tui_state_apply_router_event(context, event);
}

bool tui_state_source_blocked(const tui_state_t *state,
                               const uint8_t source[LXMF_SOURCE_LENGTH]) {
    if (state == NULL || source == NULL) return false;
    size_t index = contact_index(state, source);
    if (index < state->contact_count) return state->contacts[index].blocked;
    /* The durable directory can be larger than the visible contact cache. */
    lxmf_peer_t peer;
    return lxmf_peer_store_get(&state->peer_store, source, &peer) == LXMF_OK &&
           peer.blocked;
}

static bool source_blocked(void *context,
                             const uint8_t source[LXMF_SOURCE_LENGTH]) {
    return tui_state_source_blocked(context, source);
}

static void on_signature(void *context,
                         const uint8_t message_id[LXMF_MESSAGE_ID_LENGTH],
                         lxmf_signature_state_t signature) {
    tui_state_apply_signature(context, message_id, signature);
}

static void on_announce(rns_runtime_t *runtime, const rns_node_result *announce,
                        void *context) {
    tui_state_t *state = context;
    (void)runtime;
    if (!rns_node_registry_consider_announce(&state->nodes, announce)) return;
    apply_propagation_route(state);
    if (!state->router_ready) return;
    lxmf_router_verify_result_t verified;
    (void)lxmf_router_verify_pending(&state->router,
                                     announce->destination_hash, &verified);
}

const rns_identity *tui_state_resolve_peer(void *context,
                                        const uint8_t destination[LXMF_DESTINATION_LENGTH]) {
    tui_state_t *state = context;
    if (state == NULL || destination == NULL) return NULL;
    for (size_t i = 0u; i < state->nodes.count; ++i) {
        const rns_node_record *node = &state->nodes.records[i];
        if (node->has_message_destination &&
            memcmp(node->message_destination, destination, LXMF_DESTINATION_LENGTH) == 0 &&
            rns_identity_from_public(&state->resolved_identity, node->public_key))
            return &state->resolved_identity;
    }
    rns_path_entry path;
    static const char *const aspects[] = {"delivery"};
    uint8_t expected[LXMF_DESTINATION_LENGTH];
    if (state->runtime != NULL &&
        rns_runtime_path_lookup(state->runtime, destination, &path) == RNS_OK &&
        path.has_identity &&
        rns_identity_from_public(&state->resolved_identity, path.identity_public_key) &&
        rns_destination_hash(&state->resolved_identity, "lxmf", aspects, 1u, expected) &&
        memcmp(expected, destination, sizeof expected) == 0)
        return &state->resolved_identity;
    uint8_t name_hash[10];
    if (state->runtime != NULL &&
        rns_destination_name_hash("lxmf", aspects, 1u, name_hash) &&
        rns_runtime_recall_identity(state->runtime, destination, name_hash,
                                    &state->resolved_identity) == RNS_OK)
        return &state->resolved_identity;
    return NULL;
}

static bool resolve_peer_ratchet(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE]) {
    tui_state_t *state = context;
    for (size_t i = 0u; i < state->nodes.count; ++i) {
        const rns_node_record *node = &state->nodes.records[i];
        if (node->has_message_destination && node->has_ratchet &&
            memcmp(node->message_destination, destination,
                   LXMF_DESTINATION_LENGTH) == 0) {
            memcpy(ratchet_public, node->ratchet, RNS_RATCHET_PUBLIC_SIZE);
            return true;
        }
    }
    return false;
}

bool tui_state_peer_stamp_cost(
    const tui_state_t *state,
    const uint8_t destination[LXMF_DESTINATION_LENGTH], uint8_t *cost) {
    if (state == NULL || destination == NULL || cost == NULL) return false;
    *cost = 0u;
    for (size_t i = 0u; i < state->nodes.count; ++i) {
        const rns_node_record *node = &state->nodes.records[i];
        if (node->kind == RNS_NODE_KIND_LXMF &&
            node->has_message_destination && node->lxmf_app_data_valid &&
            node->lxmf_has_stamp_cost && node->lxmf_stamp_cost > 0u &&
            node->lxmf_stamp_cost < UINT8_MAX &&
            memcmp(node->message_destination, destination,
                   LXMF_DESTINATION_LENGTH) == 0) {
            *cost = node->lxmf_stamp_cost;
            return true;
        }
    }
    return false;
}

tui_propagation_state_t tui_state_propagation_state(
    const tui_state_t *state, rns_node_record *record, uint8_t *stamp_cost) {
    if (record != NULL) memset(record, 0, sizeof *record);
    if (stamp_cost != NULL) *stamp_cost = 0u;
    if (state == NULL || !state->settings.has_propagation_node)
        return TUI_PROPAGATION_NOT_SELECTED;
    const rns_node_record *node = rns_node_registry_get(
        &state->nodes, state->settings.propagation_node);
    if (node == NULL || !node->propagation || !node->lxmf_pn_app_data_valid)
        return TUI_PROPAGATION_WAITING_ANNOUNCE;
    if (!node->reachable) {
        /* Saved discovery records are not live reachability evidence. A
         * matching, unexpired saved path is nevertheless enough to attempt an
         * authenticated connection, without waiting for another announce. */
        rns_path_entry path;
        rns_identity identity;
        uint8_t expected[LXMF_DESTINATION_LENGTH];
        static const char *const aspects[] = {"propagation"};
        if (state->runtime == NULL ||
            rns_runtime_path_lookup(state->runtime, node->destination, &path) != RNS_OK ||
            !path.has_identity || path.unresponsive ||
            path.announce_timebase != node->announce_timebase ||
            memcmp(path.identity_public_key, node->public_key,
                   sizeof node->public_key) != 0 ||
            !rns_identity_from_public(&identity, node->public_key) ||
            !rns_destination_hash(&identity, "lxmf", aspects, 1u, expected) ||
            memcmp(expected, node->destination, sizeof expected) != 0)
            return TUI_PROPAGATION_STALE;
    }
    if (!node->lxmf_pn_enabled) return TUI_PROPAGATION_DISABLED;
    if (node->lxmf_pn_stamp_cost == 0u ||
        node->lxmf_pn_stamp_cost == UINT8_MAX)
        return TUI_PROPAGATION_INVALID_COST;
    if (record != NULL) *record = *node;
    if (stamp_cost != NULL) *stamp_cost = node->lxmf_pn_stamp_cost;
    return TUI_PROPAGATION_READY;
}

static void apply_propagation_route(tui_state_t *state) {
    if (state == NULL || !state->router_ready) return;
    rns_node_record node;
    uint8_t cost = 0u;
    if (tui_state_propagation_state(state, &node, &cost) ==
            TUI_PROPAGATION_READY &&
        rns_identity_from_public(&state->resolved_propagation_identity,
                                 node.public_key)) {
        (void)lxmf_router_set_propagation_node(
            &state->router, &state->resolved_propagation_identity,
            node.destination, cost);
    } else {
        (void)lxmf_router_set_propagation_node(&state->router, NULL, NULL, 0u);
    }
}

bool tui_state_use_propagation_node(tui_state_t *state,
                                    const rns_node_record *record) {
    if (state == NULL || record == NULL || !record->reachable ||
        !record->propagation || !record->lxmf_pn_app_data_valid ||
        !record->lxmf_pn_enabled || record->lxmf_pn_stamp_cost == 0u ||
        record->lxmf_pn_stamp_cost == UINT8_MAX)
        return false;
    tui_settings_t previous = state->settings;
    state->settings.has_propagation_node = true;
    memcpy(state->settings.propagation_node, record->destination,
           sizeof state->settings.propagation_node);
    if (!tui_state_save_settings(state)) {
        state->settings = previous;
        return false;
    }
    apply_propagation_route(state);
    tui_state_set_status(state,
        "Verified propagation node selected (cost %u); Sync Now is available",
        (unsigned)record->lxmf_pn_stamp_cost);
    return true;
}

static const char *propagation_sync_phase(lxmf_pn_session_state_t phase) {
    switch (phase) {
        case LXMF_PN_IDLE: return "idle";
        case LXMF_PN_PATH: return "finding path";
        case LXMF_PN_LINK: return "authenticating link";
        case LXMF_PN_LIST: return "listing messages";
        case LXMF_PN_DOWNLOAD: return "downloading";
        case LXMF_PN_ACK: return "acknowledging";
        case LXMF_PN_UPLOAD: return "uploading";
        case LXMF_PN_COMPLETE: return "complete";
        case LXMF_PN_FAILED: return "failed";
        case LXMF_PN_CANCELLED: return "cancelled";
    }
    return "unknown";
}

static bool propagation_sync_equal(
    const lxmf_router_propagation_sync_status_t *left,
    const lxmf_router_propagation_sync_status_t *right) {
    return left->state == right->state && left->result == right->result &&
           left->transport_error == right->transport_error &&
           left->remote_error == right->remote_error &&
           left->available == right->available &&
           left->received == right->received &&
           left->acknowledged == right->acknowledged &&
           left->accepted == right->accepted &&
           left->duplicates == right->duplicates &&
           left->rejected == right->rejected &&
           left->retain_on_node == right->retain_on_node &&
           left->active == right->active &&
           left->waiting_for_upload == right->waiting_for_upload;
}

void tui_state_apply_propagation_sync(
    tui_state_t *state,
    const lxmf_router_propagation_sync_status_t *status) {
    if (state == NULL || status == NULL ||
        propagation_sync_equal(&state->propagation_sync, status)) return;
    state->propagation_sync = *status;
    if (status->active && status->waiting_for_upload) {
        tui_state_set_status(state,
            "Propagation sync queued; starts after the current upload (cancel available)");
        return;
    }
    if (status->active) {
        tui_state_set_status(state, "Propagation sync: %s (%zu/%zu messages)",
            propagation_sync_phase(status->state), status->received,
            status->available);
        return;
    }
    if (status->state == LXMF_PN_COMPLETE) {
        if (status->rejected != 0u)
            tui_state_set_status(state,
                "Sync complete with %zu rejected and kept on node: %s",
                status->rejected, lxmf_status_string(status->result));
        else
            tui_state_set_status(state,
                "Sync complete: %zu accepted, %zu duplicates, %zu acknowledged",
                status->accepted, status->duplicates, status->acknowledged);
    } else if (status->state == LXMF_PN_CANCELLED) {
        tui_state_set_status(state, "Propagation sync cancelled; messages remain on node");
    } else if (status->state == LXMF_PN_FAILED) {
        tui_state_set_status(state, "Propagation sync failed: %s (transport: %s)",
            lxmf_status_string(status->result),
            rns_status_string(status->transport_error));
    }
}

bool tui_state_propagation_sync_start(tui_state_t *state) {
    if (state == NULL || !state->router_ready || state->runtime == NULL) {
        if (state != NULL)
            tui_state_set_status(state,
                "Cannot sync while the messaging runtime is offline");
        return false;
    }
    tui_propagation_state_t route =
        tui_state_propagation_state(state, NULL, NULL);
    if (route != TUI_PROPAGATION_READY) {
        if (route == TUI_PROPAGATION_STALE ||
            route == TUI_PROPAGATION_WAITING_ANNOUNCE) {
            rns_status_t refresh = rns_runtime_request_path(
                state->runtime, state->settings.propagation_node);
            if (refresh != RNS_OK) {
                tui_state_set_status(state, "Could not refresh propagation path: %s",
                                     rns_status_string(refresh));
                return false;
            }
        }
        tui_state_set_status(state,
            route == TUI_PROPAGATION_NOT_SELECTED
                ? "Select a verified propagation node before syncing"
                : route == TUI_PROPAGATION_DISABLED
                ? "Selected propagation node announced that it is disabled"
                : route == TUI_PROPAGATION_INVALID_COST
                ? "Selected propagation node has an invalid advertised stamp cost"
                : "Sync needs current propagation information; path refresh requested");
        return false;
    }
    if (!tui_state_link_ready(state)) {
        tui_state_set_status(state,
            "Cannot sync while every configured interface is down");
        return false;
    }
    apply_propagation_route(state);
    lxmf_status_t result =
        lxmf_router_propagation_sync_start(&state->router, false);
    if (result != LXMF_OK) {
        if (result == LXMF_ERR_PENDING) {
            lxmf_router_propagation_sync_status_t active = {0};
            (void)lxmf_router_propagation_sync_status(&state->router, &active);
            tui_state_set_status(state, active.active
                ? "Propagation sync is already running; cancel it or wait"
                : "Propagation upload is running; retry sync after it finishes");
        } else {
            tui_state_set_status(state, "Could not start propagation sync: %s",
                                 lxmf_status_string(result));
        }
        return false;
    }
    lxmf_router_propagation_sync_status_t status;
    if (lxmf_router_propagation_sync_status(&state->router, &status) == LXMF_OK)
        tui_state_apply_propagation_sync(state, &status);
    return true;
}

bool tui_state_propagation_sync_cancel(tui_state_t *state) {
    if (state == NULL) return false;
    if (!state->router_ready) {
        tui_state_set_status(state,
            "Cannot cancel sync while the messaging runtime is offline");
        return false;
    }
    lxmf_router_propagation_sync_status_t status;
    if (lxmf_router_propagation_sync_status(&state->router, &status) != LXMF_OK ||
        !status.active) {
        tui_state_set_status(state, "No propagation sync is active");
        return false;
    }
    lxmf_status_t result = lxmf_router_propagation_sync_cancel(&state->router);
    if (result != LXMF_OK) {
        tui_state_set_status(state, "Could not cancel propagation sync: %s",
                             lxmf_status_string(result));
        return false;
    }
    if (lxmf_router_propagation_sync_status(&state->router, &status) == LXMF_OK)
        tui_state_apply_propagation_sync(state, &status);
    return true;
}

void tui_state_toggle_delivery_method(tui_state_t *state) {
    if (state == NULL) return;
    if (state->compose_delivery_method == LXMF_DELIVERY_METHOD_PROPAGATED) {
        state->compose_delivery_method = LXMF_DELIVERY_METHOD_DIRECT;
        tui_state_set_status(state, "Delivery mode set to direct");
        return;
    }
    state->compose_delivery_method = LXMF_DELIVERY_METHOD_PROPAGATED;
    uint8_t cost = 0u;
    tui_propagation_state_t route =
        tui_state_propagation_state(state, NULL, &cost);
    if (route == TUI_PROPAGATION_READY)
        tui_state_set_status(state,
            "Delivery mode set to propagation-node upload (cost %u)",
            (unsigned)cost);
    else if (route == TUI_PROPAGATION_NOT_SELECTED)
        tui_state_set_status(state,
            "Propagated delivery selected; choose a verified node in Network or Settings");
    else
        tui_state_set_status(state,
            "Propagated delivery selected; messages wait for a fresh enabled node announce");
}

static bool resolve_peer_stamp_cost(
    void *context, const uint8_t destination[LXMF_DESTINATION_LENGTH],
    uint8_t *cost) {
    return tui_state_peer_stamp_cost(context, destination, cost);
}

static uint64_t wall_clock_seconds(void *context) {
    uint64_t milliseconds = 0u;
    (void)context;
    return rns_hal_wallclock_ms(&milliseconds) == RNS_OK
               ? milliseconds / UINT64_C(1000)
               : 0u;
}

static lxmf_status_t send_via_runtime(void *context, const uint8_t *packet,
                                      size_t length) {
    tui_state_t *state = context;
    if (state->runtime == NULL) return LXMF_ERR_ARGUMENT;
    /*
     * Route by the learned path instead of shouting on the first interface:
     * a peer more than one hop away needs a transport header or no node will
     * forward the packet.
     */
    return rns_runtime_send_routed(state->runtime, packet, length) == RNS_OK
               ? LXMF_OK : LXMF_ERR_CRYPTO;
}

static void on_message(void *context, const lxmf_store_message_t *message) {
    tui_state_t *state = context;
    if (!ingest_message(state, message))
        tui_state_set_status(state, "Incoming message saved; conversation list is full");
    else tui_state_set_status(state, "Incoming LXMF message received");
}

static void on_packet(rns_runtime_t *runtime, const uint8_t *packet, size_t length,
                      const rns_node_result *result, void *context) {
    tui_state_t *state = context;
    if (state->router_ready &&
        lxmf_router_receive_packet(&state->router, packet, length) == LXMF_OK)
        (void)rns_runtime_prove_packet(runtime, result, &state->identity, true);
}

static char *read_text_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        (unsigned long)size > 65536UL ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1u);
    if (text == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(text, 1u, (size_t)size, file) != (size_t)size) {
        free(text);
        (void)fclose(file);
        return NULL;
    }
    text[size] = '\0';
    (void)fclose(file);
    *length = (size_t)size;
    return text;
}

static void start_runtime(tui_state_t *state, const char *config_path) {
    size_t length = 0u;
    char *text = NULL;
    rns_config_t config;
    rns_config_diagnostic_t diagnostic = {0};
    state->config_attempted = true;
    rns_config_init(&state->parsed_config);
    memset(&state->config_diagnostic, 0, sizeof state->config_diagnostic);
    state->config_valid = false;
    size_t path_length = strnlen(config_path, sizeof state->config_path);
    if (path_length == 0u || path_length >= sizeof state->config_path) {
        tui_state_set_status(state, "Network configuration path is invalid or too long");
        return;
    }
    memcpy(state->config_path, config_path, path_length + 1u);
    text = read_text_file(config_path, &length);
    if (text == NULL) {
        tui_state_set_status(state, "Cannot read network configuration %s", config_path);
        return;
    }
    rns_config_init(&config);
    rns_status_t parsed = rns_config_parse(text, length, &config, &diagnostic);
    free(text);
    if (parsed != RNS_OK) {
        state->config_diagnostic = diagnostic;
        /*
         * The parser only fills the diagnostic once it is reading lines, so a
         * file that is not text at all reports the bare status instead.
         */
        if (diagnostic.message[0] != '\0')
            tui_state_set_status(state, "Configuration %s line %zu: %s", config_path,
                                 diagnostic.line, diagnostic.message);
        else
            tui_state_set_status(state, "Configuration %s is not a text configuration (%s)",
                                 config_path, rns_status_string(parsed));
        return;
    }
    state->parsed_config = config;
    state->config_valid = true;
    if (config.interface_count == 0u) {
        tui_state_set_status(state, "Configuration %s defines no interfaces", config_path);
        return;
    }
    rns_runtime_options_t options = {0};
    options.path_capacity = RNS_NODE_REGISTRY_MAX;
    options.packet_callback = on_packet;
    options.announce_callback = on_announce;
    options.callback_context = state;
    rns_status_t status = rns_runtime_create(&state->runtime, &config, &options);
    if (status == RNS_OK) {
        size_t restored_paths = 0u;
        tui_paths_load_result_t restored = tui_paths_load(
            state->runtime, state->path_store_path, &restored_paths);
        if (restored == TUI_PATHS_LOADED && restored_paths != 0u)
            tui_state_set_status(state, "Restored %zu saved network paths",
                                 restored_paths);
        else if (restored == TUI_PATHS_INVALID)
            tui_state_set_status(state,
                "Ignored invalid saved path snapshot; network remains active");
        else if (restored == TUI_PATHS_IO_ERROR)
            tui_state_set_status(state,
                "Could not read saved paths; network remains active");
        rns_node_record propagation_record;
        uint8_t propagation_cost = 0u;
        const rns_identity *propagation_identity = NULL;
        if (tui_state_propagation_state(state, &propagation_record,
                                        &propagation_cost) ==
                TUI_PROPAGATION_READY &&
            rns_identity_from_public(&state->resolved_propagation_identity,
                                     propagation_record.public_key))
            propagation_identity = &state->resolved_propagation_identity;
        if (propagation_identity != NULL && !propagation_record.reachable) {
            /* Registry timestamps are monotonic and cannot be reused across
             * boots. The matching path snapshot already rebases its remaining
             * lifetime with offline wall time deducted. Retain only this
             * selected cached record until that deadline, without marking it
             * reachable or changing its last-announced timestamp. */
            rns_path_entry saved_path;
            if (rns_runtime_path_lookup(state->runtime,
                    propagation_record.destination, &saved_path) == RNS_OK)
                for (size_t i = 0u; i < state->nodes.count; ++i)
                    if (memcmp(state->nodes.records[i].destination,
                               propagation_record.destination,
                               sizeof propagation_record.destination) == 0)
                        state->nodes.records[i].expires_at = saved_path.expires_at;
        }
        lxmf_router_config_t router = {
            .identity = &state->identity,
            .store = &state->store,
            .ticket_store = state->ticket_store,
            .ratchet_store = state->ratchet_store,
            .wall_clock = wall_clock_seconds,
            .wall_clock_context = state,
            .inbound_stamp_cost = state->settings.has_stamp_cost
                                      ? state->settings.stamp_cost
                                      : 0u,
            .is_source_blocked = source_blocked,
            .source_policy_context = state,
            .runtime = state->runtime,
            .resolve_identity = tui_state_resolve_peer,
            .resolve_context = state,
            .resolve_ratchet = resolve_peer_ratchet,
            .ratchet_context = state,
            .resolve_stamp_cost = resolve_peer_stamp_cost,
            .stamp_cost_context = state,
            .stamp_work_units = LXMF_STAMP_POLL_MAX_UNITS,
            .send_packet = send_via_runtime,
            .send_context = state,
            .message_callback = on_message,
            .message_context = state,
            .delivery_callback = on_delivery,
            .delivery_context = state,
            .signature_callback = on_signature,
            .signature_context = state,
            .event_callback = on_router_event,
            .event_context = state,
            .preferred_delivery_method = LXMF_DELIVERY_METHOD_DIRECT,
            .accept_inbound_links = true,
            .propagation_node_identity = propagation_identity,
            .propagation_stamp_cost = propagation_cost
        };
        if (propagation_identity != NULL)
            memcpy(router.propagation_node_destination,
                   propagation_record.destination,
                   sizeof router.propagation_node_destination);
        state->router_ready = lxmf_router_init(&state->router, &router) == LXMF_OK;
        if (state->router_ready &&
            rns_runtime_register_destination(state->runtime, state->local) != RNS_OK)
            state->router_ready = false;
    }
    if (status != RNS_OK) {
        tui_state_set_status(state, "Network startup failed: %s", rns_status_string(status));
        return;
    }
    if (!state->router_ready) {
        tui_state_set_status(state, "Network started but LXMF delivery is unavailable");
        return;
    }
    size_t down = 0u;
    size_t total = rns_runtime_interface_count(state->runtime);
    rns_status_t reason = RNS_OK;
    char failing[RNS_CONFIG_NAME_MAX] = {0};
    for (size_t i = 0u; i < total; ++i) {
        rns_runtime_interface_info_t info;
        if (rns_runtime_interface_info(state->runtime, i, &info) != RNS_OK) continue;
        if (info.state == RNS_RUNTIME_INTERFACE_UP && info.last_error == RNS_OK) continue;
        if (down++ == 0u) {
            reason = info.last_error;
            (void)snprintf(failing, sizeof failing, "%s", info.name);
        }
    }
    if (down == total)
        tui_state_set_status(state, "No interface is up (%s: %s)", failing,
                             rns_status_string(reason));
    else if (down > 0u)
        tui_state_set_status(state, "Network runtime active; %zu of %zu interfaces down",
                             down, total);
    else tui_state_set_status(state, "Network runtime active");
}

bool tui_state_link_ready(const tui_state_t *state) {
    if (state == NULL || state->runtime == NULL) return false;
    for (size_t i = 0u; i < rns_runtime_interface_count(state->runtime); ++i) {
        rns_runtime_interface_info_t info;
        if (rns_runtime_interface_info(state->runtime, i, &info) != RNS_OK) continue;
        if (info.state == RNS_RUNTIME_INTERFACE_UP && info.last_error == RNS_OK) return true;
    }
    return false;
}

/* ------------------------------------------------------------- lifecycle */

/* Written in Micron so the built-in page exercises the same parser as a
 * remote one: sections, a divider, formatting escapes and links. */
static const uint8_t tui_home_page[] =
    ">Nomad Browser\n"
    "\n"
    "`cNative Micron rendering is active.\n"
    "`a\n"
    "-\n"
    "Select a node on the `!network`! screen and press Enter to browse it.\n"
    "\n"
    "Links on a remote page are `_underlined`_. `!j`! and `!k`! move between\n"
    "them, `!Enter`! follows one, and `!Backspace`! goes back.\n";

static void rrc_from_settings(tui_rrc_model_t *rrc,
                              const tui_settings_t *settings) {
    (void)snprintf(rrc->hub_address, sizeof rrc->hub_address, "%s",
                   settings->rrc_hub_address);
    (void)snprintf(rrc->hub_identity, sizeof rrc->hub_identity, "%s",
                   settings->rrc_public_identity);
    (void)snprintf(rrc->nick, sizeof rrc->nick, "%s", settings->rrc_nick);
    (void)snprintf(rrc->room, sizeof rrc->room, "%s",
                   settings->rrc_last_room);
    (void)snprintf(rrc->outgoing, sizeof rrc->outgoing, "%s",
                   settings->rrc_draft);
    rrc->auto_reconnect = settings->rrc_auto_reconnect;
}

static void settings_from_rrc(tui_settings_t *settings,
                              const tui_rrc_model_t *rrc) {
    (void)snprintf(settings->rrc_hub_address,
                   sizeof settings->rrc_hub_address, "%s", rrc->hub_address);
    (void)snprintf(settings->rrc_public_identity,
                   sizeof settings->rrc_public_identity, "%s",
                   rrc->hub_identity);
    (void)snprintf(settings->rrc_nick, sizeof settings->rrc_nick, "%s",
                   rrc->nick);
    (void)snprintf(settings->rrc_last_room,
                   sizeof settings->rrc_last_room, "%s", rrc->room);
    (void)snprintf(settings->rrc_draft, sizeof settings->rrc_draft, "%s",
                   rrc->outgoing);
    settings->rrc_auto_reconnect = rrc->auto_reconnect;
}

static bool save_rrc_settings(tui_state_t *state,
                              const tui_settings_t *changed) {
    if (!tui_settings_valid(changed)) return false;
    if (state->settings_path[0] != '\0' &&
        !tui_settings_save(state->settings_path, changed)) return false;
    state->settings = *changed;
    state->settings_load_error = false;
    return true;
}

int tui_state_open(tui_state_t *state, const char *identity_path,
                   const char *store_path, const char *destination_hex,
                   const char *config_path) {
    static const char *const aspects[] = {"delivery"};
    int written;
    if (state == NULL || identity_path == NULL || store_path == NULL) return -1;
    memset(state, 0, sizeof *state);
    tui_editor_init(&state->composer, TUI_COMPOSER_CAPACITY);
    tui_editor_init(&state->reaction, TUI_REACTION_CAPACITY);
    tui_editor_init(&state->search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->node_search, TUI_SEARCH_CAPACITY);
    tui_editor_init(&state->address, TUI_ADDRESS_DIGITS);
    tui_editor_init(&state->setting, LXMF_DISPLAY_NAME_MAX);
    tui_editor_init(&state->browser_editor, RNS_MICRON_FORM_VALUE_MAX);
    tui_editor_init(&state->host_editor, TUI_SETTINGS_HOST_PAGES_MAX);
    tui_rrc_init(&state->rrc);
    tui_settings_defaults(&state->settings);
    rns_config_init(&state->parsed_config);
    state->tab = TUI_TRUST_UNKNOWN;
    state->compose_delivery_method = LXMF_DELIVERY_METHOD_DIRECT;
    state->screen = TUI_SCREEN_CONVERSATIONS;
    state->filter_dirty = true;
    state->messages = calloc(TUI_MAX_MESSAGES, sizeof *state->messages);
    if (state->messages == NULL) return -1;
    const char *attachment_directory = getenv("RETICULUM_ATTACHMENT_DIR");
    bool attachment_directory_invalid = attachment_directory != NULL &&
        !tui_state_set_attachment_directory(state, attachment_directory);
    if (!load_identity(identity_path, &state->identity) ||
        !rns_destination_hash(&state->identity, "lxmf", aspects, 1u, state->local))
        goto fail;
    if (lxmf_store_open(&state->store, store_path) != LXMF_OK ||
        lxmf_store_list(&state->store, load_message, state) != LXMF_OK) goto fail;
    written = snprintf(state->settings_path, sizeof state->settings_path,
                       "%s.settings", store_path);
    if (written <= 0 || (size_t)written >= sizeof state->settings_path) goto fail;
    if (!tui_settings_load(state->settings_path, &state->settings, NULL)) {
        tui_settings_defaults(&state->settings);
        state->settings_load_error = true;
    }
    rrc_from_settings(&state->rrc, &state->settings);
    written = snprintf(state->peer_store_path, sizeof state->peer_store_path,
                       "%s.peers", store_path);
    if (written <= 0 || (size_t)written >= sizeof state->peer_store_path ||
        lxmf_peer_store_open(&state->peer_store, state->peer_store_path) != LXMF_OK ||
        lxmf_peer_store_list(&state->peer_store, load_peer, state) != LXMF_OK) goto fail;
    written = snprintf(state->ticket_store_path,
                       sizeof state->ticket_store_path, "%s.tickets",
                       store_path);
    if (written <= 0 || (size_t)written >= sizeof state->ticket_store_path ||
        lxmf_ticket_store_open(&state->ticket_store,
                               state->ticket_store_path) != LXMF_OK)
        goto fail;
    written = snprintf(state->ratchet_store_path,
                       sizeof state->ratchet_store_path, "%s.ratchets",
                       store_path);
    if (written <= 0 || (size_t)written >= sizeof state->ratchet_store_path ||
        rns_ratchet_store_open(&state->ratchet_store,
                               state->ratchet_store_path, &state->identity,
                               0u, 0u) != RNS_OK)
        goto fail;
    if (destination_hex != NULL) {
        uint8_t peer[LXMF_DESTINATION_LENGTH];
        if (!tui_hex_parse(destination_hex, peer, sizeof peer)) goto fail;
        (void)ensure_contact(state, peer);
    }
    tui_state_set_status(state, attachment_directory_invalid
        ? "Ignored invalid RETICULUM_ATTACHMENT_DIR; attachment saving is disabled"
        : "Offline outbox - network delivery is not connected yet");

    rns_node_registry_init(&state->nodes, TUI_NODE_LIFETIME);
    written = snprintf(state->node_store_path, sizeof state->node_store_path,
                       "%s.nodes", store_path);
    if (written > 0 && (size_t)written < sizeof state->node_store_path) {
        if (rns_node_registry_load(&state->nodes, state->node_store_path,
                                   TUI_NODE_LIFETIME))
            for (size_t i = 0u; i < state->nodes.count; ++i)
                state->nodes.records[i].reachable = false;
    }
    else state->node_store_path[0] = '\0';
    written = snprintf(state->path_store_path, sizeof state->path_store_path,
                       "%s.paths", store_path);
    if (written <= 0 || (size_t)written >= sizeof state->path_store_path)
        state->path_store_path[0] = '\0';

    if (config_path != NULL) start_runtime(state, config_path);
    if (state->settings.host_enabled && tui_host_start(&state->host, state->runtime,
        &state->identity, &state->settings) != RNS_OK)
        tui_state_set_status(state, "Hosted node could not start: %s", rns_status_string(state->host.error));
    state->startup_announce_pending = state->runtime != NULL &&
                                      state->settings.announce_at_start;
    if (state->runtime != NULL &&
        rns_browser_create(&state->browser, state->runtime, NULL) != RNS_OK)
        tui_state_set_status(state, "Page browser unavailable; messaging remains active");

    rns_micron_history_init(&state->history);
    (void)snprintf(state->url, sizeof state->url, "nomad://local/home");
    (void)snprintf(state->page_url, sizeof state->page_url, "%s", state->url);
    if (!rns_micron_parse(&state->page, tui_home_page, sizeof tui_home_page - 1u) ||
        !rns_micron_history_push(&state->history, state->url)) goto fail;
    rns_micron_form_init(&state->form, &state->page);
    restore_selected_draft(state);
    if (state->settings_load_error)
        tui_state_set_status(state,
                             "Settings file is invalid; safe defaults are active");
    tui_state_refresh(state);
    return 0;
fail:
    tui_state_close(state);
    return -1;
}

void tui_state_close(tui_state_t *state) {
    if (state == NULL) return;
    if (state->node_store_path[0] != '\0')
        (void)rns_node_registry_save(&state->nodes, state->node_store_path);
    if (state->runtime != NULL && state->path_store_path[0] != '\0') {
        size_t saved_paths = 0u;
        (void)tui_paths_save(state->runtime, state->path_store_path,
                             &saved_paths);
    }
    settings_from_rrc(&state->settings, &state->rrc);
    if (state->settings_path[0] != '\0' && !state->settings_load_error)
        (void)tui_settings_save(state->settings_path, &state->settings);
    rns_browser_destroy(state->browser);
    state->browser = NULL;
    tui_rrc_close(&state->rrc);
    tui_host_stop(&state->host);
    if (state->router_ready) lxmf_router_destroy(&state->router);
    state->router_ready = false;
    rns_runtime_destroy(state->runtime);
    state->runtime = NULL;
    tui_interfaces_update(&state->interfaces, NULL, 0u);
    tui_state_persist_contacts(state);
    rns_ratchet_store_close(state->ratchet_store);
    state->ratchet_store = NULL;
    lxmf_ticket_store_close(state->ticket_store);
    state->ticket_store = NULL;
    lxmf_peer_store_close(&state->peer_store);
    lxmf_store_close(&state->store);
    rns_node_registry_destroy(&state->nodes);
    free(state->messages);
    state->messages = NULL;
}

/* ------------------------------------------------------------------- polling */

static void poll_browser(tui_state_t *state) {
    if (state->browser == NULL) return;
    (void)rns_browser_poll(state->browser);
    rns_browser_state_t current = rns_browser_state(state->browser);
    if (current == state->browser_state) return;
    state->browser_state = current;
    if (current == RNS_BROWSER_COMPLETE) {
        const rns_micron_page *page = rns_browser_page(state->browser);
        if (page != NULL) {
            state->page = *page;
            const char *page_url = rns_browser_page_url(state->browser);
            (void)snprintf(state->page_url, sizeof state->page_url, "%s",
                           page_url != NULL ? page_url : state->url);
            rns_micron_form_init(&state->form, &state->page);
        }
        state->link_selected = 0u;
        state->page_scroll = 0u;
        const char *fragment = strchr(state->url, '#');
        if (fragment != NULL && fragment[1] != '\0') {
            size_t fragment_length = strlen(fragment);
            if (!tui_state_browser_jump_anchor(state, fragment,
                                                fragment_length))
                tui_state_set_status(state,
                                     "Remote page loaded; anchor is unavailable");
            else
                tui_state_set_status(state, "Remote Nomad page loaded at %s",
                                     fragment);
        } else {
            tui_state_set_status(state, rns_browser_loaded_from_cache(state->browser)
                ? "Nomad page loaded from session cache" : "Remote Nomad page loaded");
        }
    } else if (current == RNS_BROWSER_FAILED) {
        tui_state_set_status(state, "Page load failed: %s",
                             rns_status_string(rns_browser_error(state->browser)));
    }
}

static bool announce_at(tui_state_t *state, uint64_t now) {
    static const char *const delivery[] = {"delivery"};
    uint8_t app_data[RNS_NODE_APP_DATA_MAX];
    size_t app_data_length = 0u;
    lxmf_status_t encoded = tui_settings_encode_announce(
        &state->settings, app_data, sizeof app_data, &app_data_length);
    if (encoded != LXMF_OK) {
        state->has_announce_result = true;
        state->last_announce_result = RNS_ERROR_INVALID_ARGUMENT;
        tui_state_set_status(state, "Cannot encode LXMF announce (%d)", encoded);
        return false;
    }
    if (state->runtime == NULL || !state->router_ready) {
        state->has_announce_result = true;
        state->last_announce_result = RNS_ERROR_INVALID_STATE;
        tui_state_set_status(state, "Cannot announce while the messaging runtime is offline");
        return false;
    }
    uint64_t wall_seconds = wall_clock_seconds(state);
    uint8_t ratchet_private[RNS_RATCHET_PRIVATE_SIZE];
    uint8_t ratchet_public[RNS_RATCHET_PUBLIC_SIZE];
    uint8_t ratchet_id[RNS_RATCHET_ID_SIZE];
    rns_status_t result = rns_ratchet_store_current(
        state->ratchet_store, wall_seconds, ratchet_private, ratchet_public,
        ratchet_id, NULL);
    if (result == RNS_OK)
        result = rns_runtime_announce_with_ratchet(
            state->runtime, &state->identity, "lxmf", delivery, 1u,
            ratchet_public, app_data, app_data_length);
    rns_hal_secure_zero(ratchet_private, sizeof ratchet_private);
    rns_hal_secure_zero(ratchet_public, sizeof ratchet_public);
    rns_hal_secure_zero(ratchet_id, sizeof ratchet_id);
    state->has_announce_result = true;
    state->last_announce_result = result;
    if (result != RNS_OK) {
        tui_state_set_status(state, "LXMF announce failed: %s",
                             rns_status_string(result));
        return false;
    }
    state->last_announce_ms = now;
    state->next_announce_ms = now + tui_settings_interval_ms(&state->settings);
    tui_state_set_status(state, "LXMF delivery destination announced");
    return true;
}

bool tui_state_announce(tui_state_t *state) {
    uint64_t now = 0u;
    if (state == NULL) return false;
    if (rns_hal_monotonic_ms(&now) != RNS_OK) {
        state->has_announce_result = true;
        state->last_announce_result = RNS_ERROR_IO;
        tui_state_set_status(state, "Cannot read the monotonic clock for announce");
        return false;
    }
    return announce_at(state, now);
}

void tui_state_poll(tui_state_t *state) {
    uint64_t now = 0u;
    size_t processed = 0u;
    if (state == NULL || state->runtime == NULL) return;
    (void)rns_runtime_poll(state->runtime, TUI_RUNTIME_BATCH, &processed);
    tui_state_interface_refresh(state);
    poll_browser(state);
    if (rns_hal_monotonic_ms(&now) == RNS_OK) {
        (void)rns_node_registry_expire(&state->nodes, (double)now / 1000.0);
        tui_rrc_poll(&state->rrc, now);
        tui_host_poll(&state->host, &state->settings, now);
        apply_propagation_route(state);
        bool network_ready = tui_state_link_ready(state);
        if (network_ready && !state->network_ready_last_poll &&
            state->settings.announce_at_start) {
            /* A nonblocking TCP client normally becomes usable after startup.
             * Announce on that transition and again after a full disconnect,
             * instead of consuming the one startup attempt while connecting. */
            state->startup_announce_pending = true;
            state->next_announce_ms = 0U;
        }
        state->network_ready_last_poll = network_ready;
        if (state->next_announce_ms == 0u &&
            !state->startup_announce_pending)
            state->next_announce_ms = now + tui_settings_interval_ms(&state->settings);
        if (tui_settings_announce_due(state->startup_announce_pending,
                                      state->next_announce_ms, now)) {
            if (!network_ready) {
                state->next_announce_ms = now + TUI_ANNOUNCE_RETRY_MS;
            } else if (announce_at(state, now)) {
                state->startup_announce_pending = false;
            } else {
                /* Transient send/interface failures retry promptly instead of
                 * postponing discovery for the normal six-hour interval. */
                state->next_announce_ms = now + TUI_ANNOUNCE_RETRY_MS;
            }
        }
        if (state->router_ready &&
            now - state->router_polled_ms >= TUI_ROUTER_INTERVAL_MS) {
            lxmf_router_poll_result_t delivery = {0};
            lxmf_status_t poll_status =
                lxmf_router_poll(&state->router, TUI_ROUTER_BATCH, &delivery);
            if (poll_status == LXMF_OK)
                state->router_polled_ms = now;
            lxmf_router_propagation_sync_status_t sync;
            if (lxmf_router_propagation_sync_status(&state->router,
                                                     &sync) == LXMF_OK)
                tui_state_apply_propagation_sync(state, &sync);
        }
    }
}

/* ---------------------------------------------------------------- selection */

void tui_state_select_offset(tui_state_t *state, int delta) {
    if (state == NULL) return;
    tui_state_refresh(state);
    if (state->visible_count == 0u) return;
    size_t position = 0u;
    while (position < state->visible_count && state->visible[position] != state->selected)
        ++position;
    if (position == state->visible_count) position = 0u;
    else if (delta < 0)
        position = position == 0u ? state->visible_count - 1u : position - 1u;
    else position = (position + 1u) % state->visible_count;
    tui_state_cancel_reference(state);
    state->selected = state->visible[position];
    restore_selected_draft(state);
    state->contacts[state->selected].unread = 0u;
    state->scroll = 0u;
    state->thread_layout_valid = false;
    state->filter_dirty = true;
}

void tui_state_set_tab(tui_state_t *state, tui_trust_t tab) {
    if (state == NULL) return;
    tui_state_cancel_reference(state);
    state->tab = tab;
    state->scroll = 0u;
    state->thread_layout_valid = false;
    state->filter_dirty = true;
    tui_state_refresh(state);
    if (state->visible_count > 0u) {
        state->selected = state->visible[0];
        restore_selected_draft(state);
    }
    state->filter_dirty = true;
}

void tui_state_scroll_by(tui_state_t *state, int lines) {
    if (state == NULL) return;
    if (state->screen == TUI_SCREEN_BROWSER) {
        /* The thread list grows upward, a page downward, so the same key
         * moves the viewport the opposite way here. */
        size_t limit = state->page.line_count != 0u
                           ? (size_t)state->page.line_count - 1u : 0u;
        if (lines > 0) {
            size_t back = (size_t)lines;
            state->page_scroll = state->page_scroll > back ? state->page_scroll - back
                                                           : 0u;
        } else {
            state->page_scroll += (size_t)(-lines);
        }
        if (state->page_scroll > limit) state->page_scroll = limit;
        return;
    }
    tui_state_refresh(state);
    if (lines < 0) {
        size_t back = (size_t)(-lines);
        state->scroll = state->scroll > back ? state->scroll - back : 0u;
    } else {
        state->scroll += (size_t)lines;
        size_t limit = state->thread_layout_valid
            ? state->thread_scroll_limit : state->thread_count;
        if (state->scroll > limit) state->scroll = limit;
    }
}

bool tui_state_open_conversation(tui_state_t *state,
                                 const uint8_t peer[LXMF_DESTINATION_LENGTH]) {
    if (state == NULL || peer == NULL) return false;
    size_t index = ensure_contact(state, peer);
    if (index == state->contact_count) {
        tui_state_set_status(state, "Conversation limit reached");
        return false;
    }
    tui_state_save_draft(state);
    tui_state_cancel_reference(state);
    state->selected = index;
    state->scroll = 0u;
    state->thread_layout_valid = false;
    state->tab = state->contacts[index].trust;
    state->screen = TUI_SCREEN_CONVERSATIONS;
    state->overlay = TUI_OVERLAY_NONE;
    state->field = TUI_FIELD_COMPOSE;
    restore_selected_draft(state);
    state->filter_dirty = true;
    return true;
}

/* ------------------------------------------------------------------ sending */

static lxmf_status_t queue_outbound(tui_state_t *state, lxmf_slice_t content,
                                    lxmf_slice_t fields) {
    if (state == NULL || state->selected >= state->contact_count ||
        (content.len != 0u && content.data == NULL) ||
        (fields.len != 0u && fields.data == NULL)) return LXMF_ERR_ARGUMENT;
    lxmf_message_t source = {0};
    lxmf_message_t decoded;
    uint8_t *packed = NULL;
    uint8_t *ticket_fields = NULL;
    size_t ticket_capacity = 0u, packed_capacity = 0u;
    size_t packed_length = 0u;
    memcpy(source.destination, state->contacts[state->selected].peer,
           LXMF_DESTINATION_LENGTH);
    memcpy(source.source, state->local, LXMF_DESTINATION_LENGTH);
    uint64_t now_ms = 0u;
    if (rns_hal_wallclock_ms(&now_ms) != RNS_OK) return LXMF_ERR_CRYPTO;
    if (now_ms <= state->last_compose_timestamp_ms) {
        if (state->last_compose_timestamp_ms == UINT64_MAX)
            return LXMF_ERR_BOUNDS;
        now_ms = state->last_compose_timestamp_ms + 1u;
    }
    source.timestamp = (double)now_ms / 1000.0;
    source.content = content;
    source.fields_msgpack = fields;
    lxmf_status_t status = LXMF_OK;
    const tui_contact_t *recipient = &state->contacts[state->selected];
    if (recipient->trust == TUI_TRUST_TRUSTED && !recipient->blocked && state->ticket_store != NULL) {
        lxmf_ticket_entry_t entry = {0};
        status = lxmf_ticket_store_issue(state->ticket_store, source.destination,
                                         now_ms / 1000u, &entry, NULL);
        if (status == LXMF_OK) {
            if (fields.len > LXMF_MAX_MESSAGE_SIZE - 40u) status = LXMF_ERR_BOUNDS;
            else {
                ticket_capacity = fields.len + 40u;
                ticket_fields = malloc(ticket_capacity);
                if (ticket_fields == NULL) status = LXMF_ERR_BOUNDS;
                else {
                    lxmf_ticket_field_t ticket = {.present = true, .expires_at = entry.expires_at};
                    memcpy(ticket.ticket, entry.ticket, sizeof ticket.ticket);
                    size_t length = 0u;
                    status = lxmf_fields_merge_ticket(fields.data, fields.len, &ticket,
                        ticket_fields, ticket_capacity, &length);
                    if (status == LXMF_OK) source.fields_msgpack = (lxmf_slice_t){ticket_fields, length};
                    rns_hal_secure_zero(&ticket, sizeof ticket);
                }
            }
        } else if (status == LXMF_ERR_PENDING) status = LXMF_OK;
        rns_hal_secure_zero(&entry, sizeof entry);
        if (status != LXMF_OK) goto done;
    }
    packed_capacity = lxmf_pack_bound(&source);
    if (packed_capacity == 0u || packed_capacity > LXMF_STORE_MAX_PACKED) {
        status = LXMF_ERR_BOUNDS; goto done;
    }
    packed = malloc(packed_capacity);
    if (packed == NULL) { status = LXMF_ERR_BOUNDS; goto done; }
    status = lxmf_pack(&source, lxmf_identity_signer, &state->identity,
                                     packed, packed_capacity, &packed_length);
    if (status != LXMF_OK) goto done;
    status = lxmf_unpack(packed, packed_length, NULL, NULL, &decoded);
    if (status != LXMF_OK) goto done;

    lxmf_store_message_t stored = {0};
    bool inserted = false;
    memcpy(stored.message_id, decoded.message_id, sizeof stored.message_id);
    memcpy(stored.destination, source.destination, sizeof stored.destination);
    memcpy(stored.source, source.source, sizeof stored.source);
    stored.timestamp = source.timestamp;
    stored.status = LXMF_DELIVERY_QUEUED;
    stored.content = source.content;
    stored.packed = (lxmf_slice_t){packed, packed_length};
    stored.delivery.desired_method = state->compose_delivery_method;
    status = lxmf_store_put(&state->store, &stored, &inserted);
    state->send_attempted = false;
    state->send_ok = false;
    if (status != LXMF_OK) goto done;
    if (!inserted) {
        status = LXMF_ERR_FORMAT;
        goto done;
    }
    state->last_compose_timestamp_ms = now_ms;

    (void)ingest_message(state, &stored);
    if (state->router_ready) {
        bool propagation_waiting =
            stored.delivery.desired_method == LXMF_DELIVERY_METHOD_PROPAGATED &&
            tui_state_propagation_state(state, NULL, NULL) !=
                TUI_PROPAGATION_READY;
        if (propagation_waiting) {
            state->send_attempted = true;
            (void)lxmf_router_send_message(&state->router,
                                           decoded.message_id);
        } else {
            state->send_attempted = true;
            state->send_ok = lxmf_router_send_message(&state->router,
                                                      decoded.message_id) == LXMF_OK;
        }
    }
done:
    if (ticket_fields != NULL) {
        rns_hal_secure_zero(ticket_fields, ticket_capacity);
        free(ticket_fields);
    }
    if (packed != NULL) {
        rns_hal_secure_zero(packed, packed_capacity);
        free(packed);
    }
    return status;
}

lxmf_status_t tui_state_queue_message(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count ||
        tui_editor_empty(&state->composer)) return LXMF_ERR_ARGUMENT;
    static const uint8_t empty_fields[] = {0x80u};
    uint8_t fields[LXMF_STANDARD_MAX_QUOTE_BYTES + 96u];
    lxmf_slice_t encoded = {empty_fields, sizeof empty_fields};
    size_t fields_length = 0u;
    if (state->compose_reference.kind == TUI_COMPOSE_REFERENCE_REPLY) {
        if (memcmp(state->compose_reference.peer,
                   state->contacts[state->selected].peer,
                   LXMF_DESTINATION_LENGTH) != 0) return LXMF_ERR_ARGUMENT;
        lxmf_standard_fields_t reply = {0};
        reply.present_mask = LXMF_STANDARD_REPLY_TO |
                             LXMF_STANDARD_REPLY_QUOTE;
        memcpy(reply.reply_to, state->compose_reference.message_id,
               LXMF_MESSAGE_ID_LENGTH);
        reply.reply_quote = (lxmf_slice_t){state->compose_reference.quote,
                                           state->compose_reference.quote_length};
        lxmf_status_t field_status = lxmf_standard_fields_merge(
            empty_fields, sizeof empty_fields, &reply, reply.present_mask, 0u,
            fields, sizeof fields, &fields_length);
        if (field_status != LXMF_OK) return field_status;
        encoded = (lxmf_slice_t){fields, fields_length};
    }
    lxmf_status_t status = queue_outbound(
        state, (lxmf_slice_t){(const uint8_t *)tui_editor_text(&state->composer),
                              tui_editor_length(&state->composer)}, encoded);
    if (status == LXMF_OK) tui_state_cancel_reference(state);
    return status;
}

lxmf_status_t tui_state_queue_reaction(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count ||
        state->compose_reference.kind != TUI_COMPOSE_REFERENCE_REACTION ||
        tui_editor_empty(&state->reaction) ||
        memcmp(state->compose_reference.peer,
               state->contacts[state->selected].peer,
               LXMF_DESTINATION_LENGTH) != 0) return LXMF_ERR_ARGUMENT;
    static const uint8_t empty_fields[] = {0x80u};
    uint8_t fields[LXMF_STANDARD_MAX_REACTION_BYTES + 96u];
    size_t fields_length = 0u;
    lxmf_standard_fields_t reaction = {0};
    reaction.present_mask = LXMF_STANDARD_REACTION;
    memcpy(reaction.reaction_to, state->compose_reference.message_id,
           LXMF_MESSAGE_ID_LENGTH);
    reaction.reaction_content = (lxmf_slice_t){
        (const uint8_t *)tui_editor_text(&state->reaction),
        tui_editor_length(&state->reaction)};
    lxmf_status_t status = lxmf_standard_fields_merge(
        empty_fields, sizeof empty_fields, &reaction, reaction.present_mask, 0u,
        fields, sizeof fields, &fields_length);
    if (status == LXMF_OK) {
        /* NomadNet 1.2.0 preserves the standard reaction field but does not
         * render it. Mirroring the short reaction in normal content keeps it
         * visible to that stock client while retaining LXMF semantics. */
        status = queue_outbound(state, reaction.reaction_content,
                                (lxmf_slice_t){fields, fields_length});
    }
    if (status == LXMF_OK) tui_state_cancel_reference(state);
    return status;
}

void tui_state_save_draft(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    contact->draft_len = tui_editor_length(&state->composer);
    if (contact->draft_len > TUI_COMPOSER_CAPACITY)
        contact->draft_len = TUI_COMPOSER_CAPACITY;
    if (contact->draft_len != 0u)
        memcpy(contact->draft, tui_editor_text(&state->composer), contact->draft_len);
    contact->draft[contact->draft_len] = '\0';
    contact->draft_dirty = true;
    tui_state_persist_contacts(state);
}

/* -------------------------------------------------------- contact mutations */

void tui_state_set_trust(tui_state_t *state, tui_trust_t trust) {
    if (state == NULL || state->selected >= state->contact_count) return;
    state->contacts[state->selected].trust = trust;
    tui_state_set_tab(state, trust);
    tui_state_persist_contacts(state);
    tui_state_set_status(state, trust == TUI_TRUST_TRUSTED ? "Trusted contact saved"
                                                           : "Untrusted contact saved");
}

void tui_state_toggle_pin(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    contact->pinned = !contact->pinned;
    tui_state_persist_contacts(state);
    tui_state_set_status(state, "Pin saved");
}

void tui_state_toggle_block(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    contact->blocked = !contact->blocked;
    tui_state_persist_contacts(state);
    tui_state_set_status(state, "Block preference saved");
}

void tui_state_toggle_note(tui_state_t *state) {
    if (state == NULL || state->selected >= state->contact_count) return;
    tui_contact_t *contact = &state->contacts[state->selected];
    (void)snprintf(contact->note, sizeof contact->note, "%s",
                   contact->note[0] != '\0' ? "" : "Local note");
    state->filter_dirty = true;
    tui_state_persist_contacts(state);
    tui_state_set_status(state, "Contact note saved");
}

/* ----------------------------------------------------------------------- RRC */

void tui_state_rrc_move(tui_state_t *state, int delta) {
    if (state == NULL || state->field != TUI_FIELD_NONE) return;
    tui_rrc_move(&state->rrc, delta);
}

void tui_state_rrc_activate(tui_state_t *state, uint64_t now_ms) {
    if (state == NULL) return;
    tui_rrc_item_t selected = state->rrc.selected;
    size_t capacity = tui_rrc_edit_capacity(selected);
    if (capacity != 0u) {
        const char *value = tui_rrc_edit_value(&state->rrc, selected);
        tui_editor_init(&state->setting, capacity);
        if (value != NULL)
            (void)tui_editor_insert(&state->setting, value, strlen(value));
        state->field = TUI_FIELD_RRC;
        return;
    }
    switch (selected) {
        case TUI_RRC_ITEM_CONNECT:
            (void)tui_rrc_connect_toggle(&state->rrc, state->runtime,
                                          &state->identity, now_ms);
            break;
        case TUI_RRC_ITEM_RECONNECT: {
            bool previous = state->rrc.auto_reconnect;
            tui_settings_t changed = state->settings;
            state->rrc.auto_reconnect = !previous;
            settings_from_rrc(&changed, &state->rrc);
            if (!save_rrc_settings(state, &changed)) {
                state->rrc.auto_reconnect = previous;
                (void)snprintf(state->rrc.status, sizeof state->rrc.status,
                               "%s", "Could not save reconnect preference");
            } else {
                (void)snprintf(state->rrc.status, sizeof state->rrc.status,
                               "%s", state->rrc.session == NULL
                                   ? "Reconnect preference saved"
                                   : "Reconnect preference saved; reconnect to apply");
            }
            break;
        }
        case TUI_RRC_ITEM_JOIN: (void)tui_rrc_join(&state->rrc); break;
        case TUI_RRC_ITEM_PART: (void)tui_rrc_part(&state->rrc); break;
        case TUI_RRC_ITEM_SEND:
            if (tui_rrc_send(&state->rrc)) {
                tui_settings_t changed = state->settings;
                settings_from_rrc(&changed, &state->rrc);
                if (!save_rrc_settings(state, &changed))
                    (void)snprintf(state->rrc.status, sizeof state->rrc.status,
                                   "%s", "Message sent; draft cleanup was not saved");
            }
            break;
        case TUI_RRC_ITEM_HUB_ADDRESS:
        case TUI_RRC_ITEM_HUB_IDENTITY:
        case TUI_RRC_ITEM_NICK:
        case TUI_RRC_ITEM_ROOM:
        case TUI_RRC_ITEM_MESSAGE:
        case TUI_RRC_ITEM_COUNT:
            break;
    }
}

bool tui_state_rrc_apply(tui_state_t *state) {
    if (state == NULL || state->field != TUI_FIELD_RRC) return false;
    char previous[RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES + 1u];
    const char *current = tui_rrc_edit_value(&state->rrc, state->rrc.selected);
    if (current == NULL || strlen(current) >= sizeof previous) return false;
    (void)snprintf(previous, sizeof previous, "%s", current);
    if (!tui_rrc_edit_apply(&state->rrc, state->rrc.selected,
                            tui_editor_text(&state->setting),
                            tui_editor_length(&state->setting)))
        return false;
    tui_settings_t changed = state->settings;
    settings_from_rrc(&changed, &state->rrc);
    if (!save_rrc_settings(state, &changed)) {
        (void)tui_rrc_edit_apply(&state->rrc, state->rrc.selected, previous,
                                 strlen(previous));
        (void)snprintf(state->rrc.status, sizeof state->rrc.status, "%s",
                       "Could not save RRC setting");
        return false;
    }
    tui_editor_clear(&state->setting);
    state->field = TUI_FIELD_NONE;
    return true;
}

bool tui_state_rrc_update_draft(tui_state_t *state) {
    if (state == NULL || state->field != TUI_FIELD_RRC ||
        state->rrc.selected != TUI_RRC_ITEM_MESSAGE) return false;
    size_t length = tui_editor_length(&state->setting);
    if (length > RNS_RRC_DEFAULT_MAX_MESSAGE_BYTES ||
        !tui_utf8_valid((const uint8_t *)tui_editor_text(&state->setting),
                        length)) return false;
    tui_settings_t changed = state->settings;
    memcpy(changed.rrc_draft, tui_editor_text(&state->setting), length);
    changed.rrc_draft[length] = '\0';
    if (!save_rrc_settings(state, &changed)) return false;
    memcpy(state->rrc.outgoing, changed.rrc_draft, length + 1u);
    return true;
}

void tui_state_rrc_cancel(tui_state_t *state) {
    if (state == NULL) return;
    tui_editor_clear(&state->setting);
    state->field = TUI_FIELD_NONE;
    (void)snprintf(state->rrc.status, sizeof state->rrc.status,
                   "%s", "RRC edit cancelled");
}

/* ---------------------------------------------------------------- settings */

void tui_state_host_move(tui_state_t *state, int delta) {
    if (state == NULL || state->screen != TUI_SCREEN_NODE || state->field != TUI_FIELD_NONE) return;
    int selected = (int)state->host_selected + (delta < 0 ? -1 : 1);
    if (selected < 0) selected = (int)TUI_HOST_COUNT - 1;
    if (selected >= (int)TUI_HOST_COUNT) selected = 0;
    state->host_selected = (tui_host_item_t)selected;
}

void tui_state_host_activate(tui_state_t *state) {
    if (state == NULL || state->screen != TUI_SCREEN_NODE || state->field != TUI_FIELD_NONE) return;
    if (state->host_selected == TUI_HOST_ANNOUNCE) {
        uint64_t now;
        if (rns_hal_monotonic_ms(&now) != RNS_OK) { tui_state_set_status(state, "Clock unavailable"); return; }
        rns_status_t status = tui_host_announce(&state->host, &state->settings, now);
        tui_state_set_status(state, "Hosted node announce: %s", rns_status_string(status)); return;
    }
    if (state->host_selected == TUI_HOST_TOGGLE) {
        if (state->host.node != NULL) {
            tui_host_stop(&state->host); state->settings.host_enabled = false;
            bool saved = tui_settings_save(state->settings_path, &state->settings);
            tui_state_set_status(state, saved ? "Hosting stopped and disabled" :
                "Hosting stopped; save failed, previous startup configuration remains");
            return;
        }
        rns_status_t status = tui_host_start(&state->host, state->runtime, &state->identity, &state->settings);
        if (status != RNS_OK) {
            tui_state_set_status(state, "Host failed: %s; check root/pages (executables disabled)", rns_status_string(status)); return;
        }
        tui_settings_t changed = state->settings; changed.host_enabled = true;
        if (!tui_settings_save(state->settings_path, &changed)) {
            tui_host_stop(&state->host); tui_state_set_status(state, "Host not started: settings could not be saved"); return;
        }
        state->settings = changed; state->settings_load_error = false;
        tui_state_set_status(state, "Hosting enabled: listed static pages are now served"); return;
    }
    if (state->host.node != NULL) { tui_state_set_status(state, "Stop hosting before changing root/pages/access"); return; }
    if (state->host_selected == TUI_HOST_ACCESS) {
        tui_settings_t changed = state->settings; changed.host_identified_only = !changed.host_identified_only;
        if (!tui_settings_save(state->settings_path, &changed)) { tui_state_set_status(state, "Could not save host access policy"); return; }
        state->settings = changed; state->settings_load_error = false;
        tui_state_set_status(state, changed.host_identified_only ? "Hosting requires identified links" : "Hosting permits anonymous links; page allowlists still apply");
        return;
    }
    if (state->host_selected != TUI_HOST_ROOT && state->host_selected != TUI_HOST_PAGES) return;
    const char *value = state->host_selected == TUI_HOST_ROOT ? state->settings.host_pages_root : state->settings.host_pages;
    tui_editor_init(&state->host_editor, state->host_selected == TUI_HOST_ROOT ? TUI_SETTINGS_HOST_ROOT_MAX : TUI_SETTINGS_HOST_PAGES_MAX);
    (void)tui_editor_insert(&state->host_editor, value, strlen(value)); state->field = TUI_FIELD_HOST;
}

bool tui_state_host_apply(tui_state_t *state) {
    if (state == NULL || state->screen != TUI_SCREEN_NODE || state->field != TUI_FIELD_HOST || state->host.node != NULL) return false;
    tui_settings_t changed = state->settings;
    const char *value = tui_editor_text(&state->host_editor);
    if (state->host_selected == TUI_HOST_ROOT) {
        if (strlen(value) >= sizeof changed.host_pages_root) return false;
        memcpy(changed.host_pages_root, value, strlen(value) + 1u);
    } else if (state->host_selected == TUI_HOST_PAGES) {
        if (strlen(value) >= sizeof changed.host_pages) return false;
        memcpy(changed.host_pages, value, strlen(value) + 1u);
    }
    else return false;
    if (!tui_settings_valid(&changed) || !tui_settings_save(state->settings_path, &changed)) {
        tui_state_set_status(state, "Host setting invalid or could not be saved; use an absolute root"); return false;
    }
    state->settings = changed; state->settings_load_error = false;
    tui_editor_clear(&state->host_editor); state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Host settings saved; only explicitly listed pages will be published"); return true;
}

bool tui_state_save_settings(tui_state_t *state) {
    if (state == NULL || state->settings_path[0] == '\0') return false;
    if (!tui_settings_save(state->settings_path, &state->settings)) {
        tui_state_set_status(state, "Could not save settings");
        return false;
    }
    state->settings_load_error = false;
    if (state->router_ready)
        (void)lxmf_router_set_inbound_stamp_cost(
            &state->router, state->settings.has_stamp_cost
                                ? state->settings.stamp_cost
                                : 0u);
    apply_propagation_route(state);
    return true;
}

void tui_state_setting_move(tui_state_t *state, int delta) {
    if (state == NULL || state->field != TUI_FIELD_NONE) return;
    int selected = (int)state->setting_selected + (delta < 0 ? -1 : 1);
    if (selected < 0) selected = (int)TUI_SETTING_COUNT - 1;
    if (selected >= (int)TUI_SETTING_COUNT) selected = 0;
    state->setting_selected = (tui_setting_item_t)selected;
}

static void begin_setting_edit(tui_state_t *state, const char *value) {
    tui_editor_init(&state->setting, LXMF_DISPLAY_NAME_MAX);
    if (value != NULL)
        (void)tui_editor_insert(&state->setting, value, strlen(value));
    state->field = TUI_FIELD_SETTING;
}

void tui_state_setting_activate(tui_state_t *state) {
    char value[TUI_ADDRESS_DIGITS + 1u];
    if (state == NULL) return;
    switch (state->setting_selected) {
        case TUI_SETTING_DISPLAY_NAME:
            begin_setting_edit(state, state->settings.display_name);
            break;
        case TUI_SETTING_STAMP_COST:
            if (state->settings.has_stamp_cost)
                (void)snprintf(value, sizeof value, "%u",
                               (unsigned)state->settings.stamp_cost);
            else
                (void)snprintf(value, sizeof value, "%s", "off");
            begin_setting_edit(state, value);
            break;
        case TUI_SETTING_ANNOUNCE_AT_START: {
            bool previous = state->settings.announce_at_start;
            state->settings.announce_at_start = !state->settings.announce_at_start;
            if (tui_state_save_settings(state))
                tui_state_set_status(state, "Announce-at-start setting saved");
            else
                state->settings.announce_at_start = previous;
            break;
        }
        case TUI_SETTING_ANNOUNCE_INTERVAL:
            (void)snprintf(value, sizeof value, "%u",
                           state->settings.announce_interval_minutes);
            begin_setting_edit(state, value);
            break;
        case TUI_SETTING_PROPAGATION_NODE:
            if (state->settings.has_propagation_node)
                tui_hex_format(state->settings.propagation_node,
                               LXMF_DESTINATION_LENGTH, value);
            else
                (void)snprintf(value, sizeof value, "%s", "none");
            begin_setting_edit(state, value);
            break;
        case TUI_SETTING_PROPAGATION_SYNC:
            if (state->propagation_sync.active)
                (void)tui_state_propagation_sync_cancel(state);
            else
                (void)tui_state_propagation_sync_start(state);
            break;
        case TUI_SETTING_ANNOUNCE_NOW:
            (void)tui_state_announce(state);
            break;
        case TUI_SETTING_COUNT:
            break;
    }
}

void tui_state_setting_cancel(tui_state_t *state) {
    if (state == NULL) return;
    tui_editor_clear(&state->setting);
    state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Settings edit cancelled");
}

static bool parse_decimal(const char *text, unsigned long maximum,
                          unsigned long *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || end == NULL || *end != '\0' ||
        parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

bool tui_state_setting_apply(tui_state_t *state) {
    tui_settings_t changed;
    const char *text;
    unsigned long number = 0u;
    if (state == NULL || state->field != TUI_FIELD_SETTING) return false;
    changed = state->settings;
    text = tui_editor_text(&state->setting);
    switch (state->setting_selected) {
        case TUI_SETTING_DISPLAY_NAME:
            changed.display_name_len = tui_editor_length(&state->setting);
            memcpy(changed.display_name, text, changed.display_name_len + 1u);
            break;
        case TUI_SETTING_STAMP_COST:
            if (text[0] == '\0' || strcmp(text, "0") == 0 ||
                strcmp(text, "off") == 0) {
                changed.has_stamp_cost = false;
                changed.stamp_cost = 0u;
            } else if (parse_decimal(text, 254u, &number) && number >= 1u) {
                changed.has_stamp_cost = true;
                changed.stamp_cost = (uint8_t)number;
            } else {
                tui_state_set_status(state, "Stamp cost must be off or 1-254");
                return false;
            }
            break;
        case TUI_SETTING_ANNOUNCE_INTERVAL:
            if (!parse_decimal(text, UINT32_MAX, &number) ||
                number < TUI_SETTINGS_MIN_ANNOUNCE_MINUTES) {
                tui_state_set_status(state, "Announce interval must be at least 30 minutes");
                return false;
            }
            changed.announce_interval_minutes = (uint32_t)number;
            break;
        case TUI_SETTING_PROPAGATION_NODE:
            if (text[0] == '\0' || strcmp(text, "none") == 0) {
                changed.has_propagation_node = false;
                memset(changed.propagation_node, 0,
                       sizeof changed.propagation_node);
            } else if (tui_hex_parse(text, changed.propagation_node,
                                     sizeof changed.propagation_node)) {
                changed.has_propagation_node = true;
            } else {
                tui_state_set_status(state,
                                     "Propagation node must be none or 32 hex characters");
                return false;
            }
            break;
        case TUI_SETTING_ANNOUNCE_AT_START:
        case TUI_SETTING_PROPAGATION_SYNC:
        case TUI_SETTING_ANNOUNCE_NOW:
        case TUI_SETTING_COUNT:
            return false;
    }
    if (!tui_settings_valid(&changed) ||
        !tui_settings_save(state->settings_path, &changed)) {
        tui_state_set_status(state, "Could not validate or save settings");
        return false;
    }
    state->settings = changed;
    state->settings_load_error = false;
    if (state->router_ready)
        (void)lxmf_router_set_inbound_stamp_cost(
            &state->router, changed.has_stamp_cost ? changed.stamp_cost : 0u);
    apply_propagation_route(state);
    state->next_announce_ms = 0u;
    tui_editor_clear(&state->setting);
    state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Settings saved");
    return true;
}

/* ------------------------------------------------------------------ network */

size_t tui_state_node_count(const tui_state_t *state) {
    return state != NULL
        ? rns_node_registry_count_filter(&state->nodes,
              tui_editor_text(&state->node_search)) : 0u;
}

size_t tui_state_node_list(const tui_state_t *state, rns_node_record *out,
                           size_t capacity) {
    if (state == NULL || out == NULL || capacity == 0u) return 0u;
    return rns_node_registry_sorted_filter(&state->nodes, out, capacity,
                                            tui_editor_text(&state->node_search));
}

bool tui_state_node_serves_pages(const rns_node_record *node) {
    return node != NULL && node->kind == RNS_NODE_KIND_NOMAD;
}

bool tui_state_selected_node(const tui_state_t *state, rns_node_record *record) {
    if (state == NULL || record == NULL || !state->has_node_selection) return false;
    size_t capacity = tui_state_node_count(state);
    rns_node_record *sorted = capacity == 0U ? NULL
        : malloc(capacity * sizeof *sorted);
    if (capacity != 0U && sorted == NULL) return false;
    size_t count = tui_state_node_list(state, sorted, capacity);
    for (size_t i = 0u; i < count; ++i) {
        if (memcmp(sorted[i].destination, state->node_selection,
                   LXMF_DESTINATION_LENGTH) != 0) continue;
        *record = sorted[i];
        free(sorted);
        return true;
    }
    free(sorted);
    return false;
}

size_t tui_state_node_position(const tui_state_t *state) {
    if (state == NULL || !state->has_node_selection) return 0u;
    size_t capacity = tui_state_node_count(state);
    rns_node_record *sorted = capacity == 0U ? NULL
        : malloc(capacity * sizeof *sorted);
    if (capacity != 0U && sorted == NULL) return 0U;
    size_t count = tui_state_node_list(state, sorted, capacity);
    for (size_t i = 0u; i < count; ++i) {
        if (memcmp(sorted[i].destination, state->node_selection,
                   LXMF_DESTINATION_LENGTH) == 0) {
            free(sorted);
            return i;
        }
    }
    free(sorted);
    return 0u;
}

void tui_state_node_move(tui_state_t *state, int delta) {
    if (state == NULL) return;
    size_t capacity = tui_state_node_count(state);
    rns_node_record *sorted = capacity == 0U ? NULL
        : malloc(capacity * sizeof *sorted);
    if (capacity != 0U && sorted == NULL) {
        tui_state_set_status(state, "Node list is temporarily unavailable");
        return;
    }
    size_t count = tui_state_node_list(state, sorted, capacity);
    if (count == 0u) {
        state->has_node_selection = false;
        free(sorted);
        return;
    }
    size_t position = tui_state_node_position(state);
    if (!state->has_node_selection) position = 0u;
    else if (delta < 0) {
        size_t back = (size_t)(-delta);
        position = position > back ? position - back : 0u;
    } else {
        position += (size_t)delta;
        if (position >= count) position = count - 1u;
    }
    memcpy(state->node_selection, sorted[position].destination,
           LXMF_DESTINATION_LENGTH);
    state->has_node_selection = true;
    free(sorted);
}

void tui_state_request_path(tui_state_t *state) {
    rns_node_record node;
    if (state == NULL) return;
    if (state->runtime == NULL) tui_state_set_status(state, "No network runtime");
    else if (!tui_state_selected_node(state, &node))
        tui_state_set_status(state, "No node selected");
    else if (rns_runtime_request_path(state->runtime, node.destination) == RNS_OK)
        tui_state_set_status(state, "Path refresh requested");
    else tui_state_set_status(state, "Path refresh could not be sent");
}

size_t tui_state_interface_count(const tui_state_t *state) {
    return state != NULL ? state->interfaces.count : 0u;
}

bool tui_state_interface_info(const tui_state_t *state, size_t index,
                              rns_runtime_interface_info_t *info) {
    if (state == NULL || info == NULL || index >= state->interfaces.count)
        return false;
    *info = state->interfaces.items[index];
    return true;
}

void tui_state_interface_refresh(tui_state_t *state) {
    rns_runtime_interface_info_t items[TUI_INTERFACE_CAPACITY];
    size_t count = 0u;
    if (state == NULL) return;
    if (state->runtime != NULL) {
        size_t available = rns_runtime_interface_count(state->runtime);
        if (available > TUI_INTERFACE_CAPACITY) available = TUI_INTERFACE_CAPACITY;
        for (size_t i = 0u; i < available; ++i) {
            if (rns_runtime_interface_info(state->runtime, i, &items[count]) == RNS_OK)
                ++count;
        }
    }
    tui_interfaces_update(&state->interfaces, items, count);
}

void tui_state_interface_move(tui_state_t *state, int delta) {
    if (state == NULL) return;
    tui_state_interface_refresh(state);
    tui_interfaces_move(&state->interfaces, delta);
}

/* ------------------------------------------------------------------ browser */

size_t tui_state_link_count(const tui_state_t *state) {
    return state == NULL ? 0u : rns_micron_link_count(&state->page);
}

const rns_micron_span *tui_state_link(const tui_state_t *state, size_t index) {
    return state == NULL ? NULL : rns_micron_link(&state->page, index);
}

static bool browser_interactive_kind(rns_micron_span_kind kind) {
    return kind == RNS_MICRON_SPAN_LINK || kind == RNS_MICRON_SPAN_FIELD ||
           kind == RNS_MICRON_SPAN_CHECKBOX ||
           kind == RNS_MICRON_SPAN_RADIO;
}

size_t tui_state_browser_control_count(const tui_state_t *state) {
    size_t count = 0u;
    if (state == NULL) return 0u;
    for (size_t i = 0u; i < state->page.span_count; ++i)
        if (browser_interactive_kind(state->page.spans[i].kind)) ++count;
    return count;
}

const rns_micron_span *tui_state_browser_selected_span(
    const tui_state_t *state, size_t *span_index) {
    size_t ordinal = 0u;
    if (state == NULL) return NULL;
    for (size_t i = 0u; i < state->page.span_count; ++i) {
        if (!browser_interactive_kind(state->page.spans[i].kind)) continue;
        if (ordinal++ == state->link_selected) {
            if (span_index != NULL) *span_index = i;
            return &state->page.spans[i];
        }
    }
    return NULL;
}

void tui_state_browser_move(tui_state_t *state, int delta) {
    if (state == NULL || state->field != TUI_FIELD_NONE) return;
    size_t count = tui_state_browser_control_count(state);
    if (count == 0u) { state->link_selected = 0u; return; }
    if (state->link_selected >= count) state->link_selected = count - 1u;
    if (delta < 0)
        state->link_selected = state->link_selected == 0u
                                   ? count - 1u : state->link_selected - 1u;
    else state->link_selected = (state->link_selected + 1u) % count;
}

static size_t form_control_index_for_span(const tui_state_t *state,
                                          size_t span_index) {
    for (size_t i = 0u; i < state->form.count; ++i)
        if (state->form.controls[i].span_index == span_index) return i;
    return SIZE_MAX;
}

static bool replace_browser_identity(tui_state_t *state, bool enabled,
                                      const uint8_t *destination) {
    if (state->runtime == NULL || (enabled && !state->identity.has_private)) {
        tui_state_set_status(state, "Browser identity requires a running network and local identity");
        return false;
    }
    rns_browser_options_t options = {.request_identity = enabled ? &state->identity : NULL};
    rns_browser_t *replacement = NULL;
    rns_status_t status = rns_browser_create(&replacement, state->runtime, &options);
    if (status != RNS_OK) {
        tui_state_set_status(state, "Could not change browser identity: %s", rns_status_string(status));
        return false;
    }
    rns_browser_destroy(state->browser);
    state->browser = replacement;
    state->browser_state = RNS_BROWSER_IDLE;
    state->browser_identified = enabled;
    memset(state->browser_identity_destination, 0, sizeof state->browser_identity_destination);
    if (enabled) memcpy(state->browser_identity_destination, destination, LXMF_DESTINATION_LENGTH);
    return true;
}

bool tui_state_browser_identification(tui_state_t *state, bool enabled) {
    if (state == NULL || state->screen != TUI_SCREEN_BROWSER) return false;
    uint8_t destination[LXMF_DESTINATION_LENGTH] = {0};
    if (enabled) {
        char hash[TUI_ADDRESS_DIGITS + 1u];
        if (strlen(state->url) < TUI_ADDRESS_DIGITS + 1u || state->url[TUI_ADDRESS_DIGITS] != ':') {
            tui_state_set_status(state, "Select a verified remote Nomad page before identifying");
            return false;
        }
        memcpy(hash, state->url, TUI_ADDRESS_DIGITS); hash[TUI_ADDRESS_DIGITS] = '\0';
        if (!tui_hex_parse(hash, destination, sizeof destination) ||
            !tui_state_node_serves_pages(rns_node_registry_get(&state->nodes, destination))) {
            tui_state_set_status(state, "Select a verified remote Nomad page before identifying");
            return false;
        }
    }
    if (!replace_browser_identity(state, enabled, destination)) return false;
    /* Retain the displayed page if reload fails. Identity mode still changed. */
    (void)tui_state_browse(state, state->url, false);
    return true;
}

static bool browse_request(tui_state_t *state, const char *url,
                           bool push_history, const uint8_t *form_msgpack,
                           size_t form_msgpack_length) {
    char requested[RNS_MICRON_TEXT_MAX];
    char destination_text[TUI_ADDRESS_DIGITS + 1u];
    uint8_t destination[LXMF_DESTINATION_LENGTH];
    if (state == NULL) return false;
    if (url == NULL || strlen(url) >= sizeof requested) {
        tui_state_set_status(state, "Invalid or oversized link");
        return false;
    }
    memcpy(requested, url, strlen(url) + 1u);
    if (state->browser == NULL) {
        tui_state_set_status(state,
                             "Configure a network interface before browsing remote pages");
        return false;
    }
    if (strlen(requested) < TUI_ADDRESS_DIGITS + 1u) {
        tui_state_set_status(state, "Unsupported browser URL");
        return false;
    }
    memcpy(destination_text, requested, TUI_ADDRESS_DIGITS);
    destination_text[TUI_ADDRESS_DIGITS] = '\0';
    if (!tui_hex_parse(destination_text, destination, sizeof destination)) {
        tui_state_set_status(state, "Invalid Nomad destination");
        return false;
    }
    const rns_node_record *node = rns_node_registry_get(&state->nodes, destination);
    rns_identity identity;
    if (node == NULL || !rns_identity_from_public(&identity, node->public_key)) {
        tui_state_set_status(state, "No verified identity for this Nomad node");
        return false;
    }
    if (state->browser_identified &&
        memcmp(state->browser_identity_destination, destination, sizeof destination) != 0 &&
        !replace_browser_identity(state, false, NULL)) return false;
    rns_status_t status = rns_browser_open(state->browser, requested, &identity,
                                           form_msgpack,
                                           form_msgpack_length);
    if (status != RNS_OK) {
        tui_state_set_status(state, "Page request failed: %s", rns_status_string(status));
        return false;
    }
    if (push_history && !rns_micron_history_push(&state->history, requested)) {
        rns_browser_cancel(state->browser);
        tui_state_set_status(state, "Browser history is full");
        return false;
    }
    (void)snprintf(state->url, sizeof state->url, "%s", requested);
    state->link_selected = 0u;
    state->page_scroll = 0u;
    /* Force the polling presentation path even for synchronous cache hits. */
    state->browser_state = RNS_BROWSER_IDLE;
    tui_state_set_status(state, rns_browser_loaded_from_cache(state->browser)
        ? "Loaded cached Nomad page" : "Discovering route to Nomad page");
    return true;
}

bool tui_state_browse(tui_state_t *state, const char *url, bool push_history) {
    return browse_request(state, url, push_history, NULL, 0u);
}

bool tui_state_browser_resolve(const tui_state_t *state, const char *target,
                               char *output, size_t capacity) {
    return state != NULL && state->page_url[0] != '\0' &&
        rns_micron_normalize_url(state->page_url, target, output, capacity);
}

void tui_state_browse_selected(tui_state_t *state) {
    char url[RNS_MICRON_TEXT_MAX];
    uint8_t form_msgpack[RNS_BROWSER_FORM_MAX];
    size_t form_length = 0u;
    if (state == NULL) return;
    const rns_micron_span *item = tui_state_browser_selected_span(state, NULL);
    if (item == NULL || item->kind != RNS_MICRON_SPAN_LINK) return;
    const char *target = rns_micron_span_target(&state->page, item);
    if (target[0] == '#') {
        (void)tui_state_browser_jump_anchor(state, target,
                                            item->target_length);
        return;
    }
    /* NomadNet destination-type links support the shorthand and full name.
     * Accept explicit lxmf: address links too; lxm:// paper messages are a
     * different format and must not be interpreted as contact addresses. */
    const char *message_address = NULL;
    if (strncmp(target, "lxmf@", 5u) == 0) message_address = target + 5u;
    else if (strncmp(target, "lxmf.delivery@", 14u) == 0) message_address = target + 14u;
    else if (strncmp(target, "lxmf://", 7u) == 0) message_address = target + 7u;
    else if (strncmp(target, "lxmf:", 5u) == 0) message_address = target + 5u;
    if (message_address != NULL) {
        uint8_t peer[LXMF_DESTINATION_LENGTH];
        if (!tui_hex_parse(message_address, peer, sizeof peer)) {
            tui_state_set_status(state, "Link carries a malformed LXMF address");
            return;
        }
        if (tui_state_open_conversation(state, peer))
            state->screen = TUI_SCREEN_CONVERSATIONS;
        return;
    }
    if (!tui_state_browser_resolve(state, target, url, sizeof url)) {
        tui_state_set_status(state, "Invalid or oversized link");
        return;
    }
    if (item->has_selector &&
        !rns_micron_form_encode(&state->page, &state->form,
                                rns_micron_span_value(&state->page, item),
                                item->value_length,
                                form_msgpack, sizeof form_msgpack,
                                &form_length)) {
        tui_state_set_status(state, "Form data exceeds the safe request limit");
        return;
    }
    (void)browse_request(state, url, true,
                         item->has_selector ? form_msgpack : NULL,
                         item->has_selector ? form_length : 0u);
}

bool tui_state_browser_jump_anchor(tui_state_t *state, const char *target,
                                   size_t target_length) {
    if (state == NULL || target == NULL || target_length == 0u ||
        target[0] != '#' || target_length > RNS_MICRON_ANCHOR_NAME_MAX + 1u)
        return false;
    size_t line = state->page_scroll;
    if (target_length == 1u) {
        bool found = false;
        for (size_t i = state->page_scroll + 1u;
             i < state->page.line_count; ++i) {
            if (state->page.lines[i].heading == 0u) continue;
            line = i;
            found = true;
            break;
        }
        if (!found) {
            tui_state_set_status(state, "No later heading on this page");
            return false;
        }
    } else if (!rns_micron_anchor_line(&state->page, target + 1u,
                                       target_length - 1u, &line)) {
        tui_state_set_status(state, "Unknown page anchor: %.*s",
                             (int)target_length, target);
        return false;
    }
    state->page_scroll = line;
    tui_state_set_status(state, "Moved to %.*s", (int)target_length, target);
    return true;
}

void tui_state_browser_activate(tui_state_t *state) {
    size_t span_index = 0u;
    if (state == NULL || state->field != TUI_FIELD_NONE) return;
    const rns_micron_span *span =
        tui_state_browser_selected_span(state, &span_index);
    if (span == NULL) return;
    if (span->kind == RNS_MICRON_SPAN_LINK) {
        tui_state_browse_selected(state);
        return;
    }
    size_t control = form_control_index_for_span(state, span_index);
    if (control == SIZE_MAX) return;
    if (span->kind == RNS_MICRON_SPAN_FIELD) {
        tui_editor_init(&state->browser_editor, RNS_MICRON_FORM_VALUE_MAX);
        const rns_micron_form_control *value =
            rns_micron_form_control_at(&state->form, control);
        if (value != NULL)
            (void)tui_editor_insert(&state->browser_editor, value->value,
                                    value->value_length);
        state->browser_edit_control = control;
        state->field = TUI_FIELD_BROWSER_FORM;
        tui_state_set_status(state, "Editing page field; Enter applies, Esc cancels");
    } else if (rns_micron_form_toggle(&state->form, &state->page, control)) {
        tui_state_set_status(state, span->kind == RNS_MICRON_SPAN_RADIO
                                        ? "Radio choice selected"
                                        : "Checkbox toggled");
    }
}

bool tui_state_browser_form_apply(tui_state_t *state) {
    if (state == NULL || state->field != TUI_FIELD_BROWSER_FORM) return false;
    if (!rns_micron_form_set(&state->form, &state->page,
                             state->browser_edit_control,
                             tui_editor_text(&state->browser_editor),
                             tui_editor_length(&state->browser_editor))) {
        tui_state_set_status(state, "Page field value is invalid or too long");
        return false;
    }
    tui_editor_clear(&state->browser_editor);
    state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Page field updated locally");
    return true;
}

void tui_state_browser_form_cancel(tui_state_t *state) {
    if (state == NULL || state->field != TUI_FIELD_BROWSER_FORM) return;
    tui_editor_clear(&state->browser_editor);
    state->field = TUI_FIELD_NONE;
    tui_state_set_status(state, "Page field edit cancelled");
}

void tui_state_browse_back(tui_state_t *state) {
    if (state == NULL) return;
    const char *url = rns_micron_history_back(&state->history);
    if (url != NULL) (void)tui_state_browse(state, url, false);
}

void tui_state_browse_node(tui_state_t *state, const rns_node_record *node) {
    char hash[TUI_ADDRESS_DIGITS + 1u];
    char url[RNS_MICRON_TEXT_MAX];
    if (state == NULL || node == NULL) return;
    /*
     * Most announces are LXMF inboxes or transport nodes that serve no pages;
     * requesting one just times out, so say so instead.
     */
    if (!tui_state_node_serves_pages(node)) {
        tui_state_set_status(state,
                             "This announce is not a Nomad Network node and serves no pages");
        state->overlay = TUI_OVERLAY_NONE;
        return;
    }
    tui_hex_format(node->destination, LXMF_DESTINATION_LENGTH, hash);
    (void)snprintf(url, sizeof url, "%s:/page/index.mu", hash);
    (void)tui_state_browse(state, url, true);
    state->screen = TUI_SCREEN_BROWSER;
    state->overlay = TUI_OVERLAY_NONE;
}

bool tui_state_browse_cancel(tui_state_t *state) {
    if (state == NULL || state->browser == NULL) return false;
    rns_browser_state_t current = rns_browser_state(state->browser);
    if (current == RNS_BROWSER_COMPLETE || current == RNS_BROWSER_FAILED) return false;
    rns_browser_cancel(state->browser);
    state->browser_state = RNS_BROWSER_CANCELLED;
    tui_state_set_status(state, "Page request cancelled");
    return true;
}
