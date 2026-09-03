#include "tui.h"
#include "tui_render.h"
#include "tui_state.h"

#include "reticulum/destination.h"
#include "reticulum/identity.h"
#include "reticulum/lxmf.h"
#include "reticulum/lxmf_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t standard_fields(uint8_t *output) {
    static const uint8_t empty[] = {0x80u};
    static const uint8_t quote[] = "quote";
    static const uint8_t reaction[] = "+1";
    static const uint8_t name[] = "../secret";
    static const uint8_t attachment[] = "xyz";
    static const uint8_t format[] = "png";
    static const uint8_t image[] = {0x89u, 0x50u};
    static const uint8_t audio[] = {0x7fu};
    lxmf_standard_fields_t fields = {0};
    fields.present_mask = LXMF_STANDARD_ALL;
    fields.renderer = LXMF_RENDERER_MARKDOWN;
    for (size_t i = 0u; i < 32u; ++i) {
        fields.reply_to[i] = (uint8_t)i;
        fields.reaction_to[i] = (uint8_t)(31u - i);
        fields.thread[i] = 0x44u;
    }
    fields.reply_quote = (lxmf_slice_t){quote, sizeof quote - 1u};
    fields.reaction_content = (lxmf_slice_t){reaction, sizeof reaction - 1u};
    fields.attachment_count = 1u;
    fields.attachments[0].name = (lxmf_slice_t){name, sizeof name - 1u};
    fields.attachments[0].data =
        (lxmf_slice_t){attachment, sizeof attachment - 1u};
    fields.image.format_kind = LXMF_MEDIA_FORMAT_TEXT;
    fields.image.text_format = (lxmf_slice_t){format, sizeof format - 1u};
    fields.image.data = (lxmf_slice_t){image, sizeof image};
    fields.audio.format_kind = LXMF_MEDIA_FORMAT_INTEGER;
    fields.audio.integer_format = LXMF_AUDIO_OPUS_OGG;
    fields.audio.data = (lxmf_slice_t){audio, sizeof audio};
    size_t output_length = 0u;
    assert(lxmf_standard_fields_merge(empty, sizeof empty, &fields,
        LXMF_STANDARD_ALL, 0u, output, 256u, &output_length) == LXMF_OK);
    return output_length;
}

static void store_message(lxmf_store_t *store, rns_identity *sender,
                          const uint8_t destination[16], const uint8_t *fields,
                          size_t fields_length, const char *content,
                          bool retain_packed, double timestamp) {
    lxmf_message_t source = {0};
    memcpy(source.destination, destination, 16u);
    memcpy(source.source, sender->hash, 16u);
    source.timestamp = timestamp;
    source.content = (lxmf_slice_t){(const uint8_t *)content, strlen(content)};
    source.fields_msgpack = (lxmf_slice_t){fields, fields_length};
    uint8_t packed[1024];
    size_t packed_length = 0u;
    assert(lxmf_pack(&source, lxmf_identity_signer, sender, packed,
                     sizeof packed, &packed_length) == LXMF_OK);
    lxmf_message_t decoded;
    assert(lxmf_unpack(packed, packed_length, NULL, NULL, &decoded) == LXMF_OK);
    lxmf_store_message_t stored = {0};
    memcpy(stored.message_id, decoded.message_id, 32u);
    memcpy(stored.destination, source.destination, 16u);
    memcpy(stored.source, source.source, 16u);
    stored.timestamp = timestamp;
    stored.status = LXMF_DELIVERY_DELIVERED;
    stored.signature_state = LXMF_SIGNATURE_VERIFIED;
    stored.content = source.content;
    if (retain_packed)
        stored.packed = (lxmf_slice_t){packed, packed_length};
    bool inserted = false;
    assert(lxmf_store_put(store, &stored, &inserted) == LXMF_OK && inserted);
}

static void remove_state_files(const char *store_path) {
    static const char *const suffixes[] = {
        "", ".settings", ".peers", ".tickets", ".ratchets", ".nodes"};
    char path[1200];
    for (size_t i = 0u; i < sizeof suffixes / sizeof suffixes[0]; ++i) {
        int length = snprintf(path, sizeof path, "%s%s", store_path, suffixes[i]);
        assert(length > 0 && (size_t)length < sizeof path);
        (void)unlink(path);
    }
}

int main(void) {
    char directory[] = "/tmp/reticulum-fields-ui-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char identity_path[1024];
    char store_path[1024];
    char saved_path[1024];
    char failure_path[1024];
    assert(snprintf(identity_path, sizeof identity_path, "%s/identity", directory) > 0);
    assert(snprintf(store_path, sizeof store_path, "%s/history", directory) > 0);
    assert(snprintf(saved_path, sizeof saved_path, "%s/secret", directory) > 0);
    assert(snprintf(failure_path, sizeof failure_path, "%s/not-a-directory", directory) > 0);

    rns_identity local;
    rns_identity remote;
    uint8_t private_key[64];
    assert(rns_identity_generate(&local));
    assert(rns_identity_generate(&remote));
    assert(rns_identity_export_private(&local, private_key));
    FILE *identity_file = fopen(identity_path, "wb");
    assert(identity_file != NULL);
    assert(fwrite(private_key, 1u, sizeof private_key, identity_file) ==
           sizeof private_key);
    assert(fclose(identity_file) == 0);

    uint8_t local_destination[16];
    static const char *const aspects[] = {"delivery"};
    assert(rns_destination_hash(&local, "lxmf", aspects, 1u,
                                local_destination));
    lxmf_store_t store = {0};
    assert(lxmf_store_open(&store, store_path) == LXMF_OK);
    uint8_t fields[256];
    size_t fields_length = standard_fields(fields);
    lxmf_standard_fields_t parsed_fields;
    assert(lxmf_standard_fields_parse(fields, fields_length, &parsed_fields) ==
           LXMF_OK);
    store_message(&store, &remote, local_destination, fields, fields_length,
                  "with metadata", true, 1.0);
    static const uint8_t empty_fields[] = {0x80u};
    store_message(&store, &remote, local_destination, empty_fields,
                  sizeof empty_fields, "missing packed", false, 2.0);
    static const uint8_t malformed_fields[] = {0x81u, 0x0fu, 0x09u};
    store_message(&store, &remote, local_destination, malformed_fields,
                  sizeof malformed_fields, "malformed fields", true, 3.0);
    lxmf_store_close(&store);

    assert(unsetenv("RETICULUM_ATTACHMENT_DIR") == 0);
    tui_state_t state;
    assert(tui_state_open(&state, identity_path, store_path, NULL, NULL) == 0);
    tui_state_refresh(&state);
    assert(state.message_count == 3u && state.thread_count == 3u);
    assert(state.messages[0].value.packed.data == NULL);
    const tui_message_metadata_t *metadata = &state.messages[0].metadata;
    assert(metadata->state == TUI_METADATA_AVAILABLE);
    assert((metadata->present_mask & LXMF_STANDARD_ALL) == LXMF_STANDARD_ALL);
    assert(strcmp(metadata->reply_quote, "quote") == 0);
    assert(strcmp(metadata->reaction, "+1") == 0);
    assert(metadata->attachment_count == 1u);
    assert(strcmp(metadata->attachments[0].display_name, "../secret") == 0);
    assert(strcmp(metadata->attachments[0].safe_name, "secret") == 0);
    assert(metadata->attachments[0].size == 3u);
    assert(metadata->image_size == 2u &&
           strcmp(metadata->image_text_format, "png") == 0);
    assert(metadata->audio_size == 1u &&
           metadata->audio_integer_format == LXMF_AUDIO_OPUS_OGG);
    assert(state.messages[1].metadata.state == TUI_METADATA_MISSING_PACKED);
    assert(state.messages[2].metadata.state == TUI_METADATA_MALFORMED);

    FILE *dump = tmpfile();
    assert(dump != NULL && tui_render_dump(&state, dump) == 0);
    assert(fseek(dump, 0L, SEEK_SET) == 0);
    char output[4096];
    size_t output_length = fread(output, 1u, sizeof output - 1u, dump);
    output[output_length] = '\0';
    assert(strstr(output, "renderer:markdown") != NULL);
    assert(strstr(output, "reaction:+1") != NULL);
    assert(strstr(output, "attachment 1: ../secret (3 bytes; save as secret)") != NULL);
    assert(strstr(output, "image:2B/png") != NULL);
    assert(strstr(output, "audio:1B/format-16") != NULL);
    assert(strstr(output, "packed message unavailable") != NULL);
    assert(strstr(output, "metadata: malformed") != NULL);
    assert(fclose(dump) == 0);

    struct { char output[9]; unsigned char guard; } narrow = {{0}, 0xa5u};
    tui_render_message_metadata(metadata, narrow.output, sizeof narrow.output);
    assert(narrow.output[sizeof narrow.output - 1u] == '\0');
    assert(narrow.guard == 0xa5u);

    assert(tui_dispatch_key(&state, 'v'));
    assert(strstr(state.status, "RETICULUM_ATTACHMENT_DIR") != NULL);
    assert(tui_state_set_attachment_directory(&state, directory));
    assert(tui_dispatch_key(&state, 'v'));
    FILE *saved = fopen(saved_path, "rb");
    assert(saved != NULL);
    char saved_data[4] = {0};
    assert(fread(saved_data, 1u, 3u, saved) == 3u);
    assert(fclose(saved) == 0 && memcmp(saved_data, "xyz", 3u) == 0);
    assert(tui_dispatch_key(&state, 'v'));
    assert(strstr(state.status, "no file was overwritten") != NULL);

    FILE *failure = fopen(failure_path, "wb");
    assert(failure != NULL && fclose(failure) == 0);
    assert(tui_state_set_attachment_directory(&state, failure_path));
    assert(tui_dispatch_key(&state, 'v'));
    assert(strstr(state.status, "directory is unavailable") != NULL);
    assert(!tui_state_set_attachment_directory(&state, "relative/path"));

    tui_state_close(&state);
    assert(unlink(saved_path) == 0);
    assert(unlink(failure_path) == 0);
    assert(unlink(identity_path) == 0);
    remove_state_files(store_path);
    assert(rmdir(directory) == 0);
    return 0;
}
