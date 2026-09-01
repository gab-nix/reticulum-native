# LXMF compatibility

This module implements the base LXMF message representation used by the pinned
Python LXMF reference implementation: two 16-byte destination hashes, a 64-byte
Ed25519 signature and a MessagePack payload. The payload is encoded as
`[timestamp, title, content, fields]`, with an optional fifth direct stamp.

The message ID is SHA-256 over `destination || source || msgpack(payload[0:4])`.
The signature input is that same byte string followed by the 32-byte message ID.
The optional stamp is deliberately excluded from both. Title and content are
binary UTF-8 byte strings; fields are preserved as one bounded MessagePack map.

`lxmf_pack()` delegates Ed25519 signing to a callback so the codec does not own
identity keys. `lxmf_unpack()` can similarly delegate signature verification.
Callers must verify signatures before presenting a message as authenticated.

Ticket-backed stamps are supported. Proof-of-work stamp generation/validation,
propagation-node envelopes and stamps, compression negotiation, attachments,
router queues, Reticulum Link/Resource delivery and message persistence belong
to higher layers and are not implemented in this codec.

The decoder is intentionally strict: one top-level array, float64 timestamps,
binary/string title and content, a map for fields, at most one 16-byte stamp,
no trailing bytes, and MessagePack nesting limited to 32 levels.
