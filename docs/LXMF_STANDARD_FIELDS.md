# LXMF standard fields

The native standard-field API inspects and constructs the optional fields used
by pinned LXMF 1.1.0 and NomadNet 1.2.0 without taking ownership of message
bytes. Include `reticulum/lxmf_fields.h` to use it.

Supported fields are renderer, reply target and quote, reaction, thread ID,
file attachments, image and audio. Hash references require the full 32-byte
LXMF message ID. Quotes and reaction content are bounded UTF-8 binary values.
Attachments are bounded `[text filename, binary data]` pairs. Image and audio
accept either binary data or `[format, binary data]`, where the format is a
bounded string or one of the pinned integer audio-mode constants.

`lxmf_standard_fields_parse()` returns borrowed slices and rejects malformed or
duplicate known fields. It structurally validates but does not interpret
unknown MessagePack entries. `lxmf_standard_fields_merge()` replaces or removes
only fields selected by the caller; every untouched key/value pair is copied
byte-for-byte, including unknown extensions. Passing an empty existing map
constructs a new fields map.

The API caps map objects, nesting, attachment counts, names, quotes, reactions
and media data. The complete output remains subject to the 8 MiB LXMF message
bound. `lxmf_attachment_safe_name()` derives a traversal-safe candidate name,
but never creates a file. Applications must require a separate explicit user
action before saving attachment bytes.

`tests/fixtures/lxmf_standard_fields.provenance.json` records the exact pinned
LXMF and RNS revisions, upstream MessagePack encoder and fixture digest. Fixture
equality demonstrates deterministic representation compatibility; it is not a
live messaging or UI interoperability result.
