# LXMF peer store

`lxmf_peer_store` persists the local contact and conversation settings needed by
the Nomad-style chat UI. Each peer is keyed by its 16-byte LXMF delivery address
and stores its display name, trust/block/pin choices, private note, propagation
preference and optional node, last-seen and announce times, unread count, and
unfinished draft.

Text is UTF-8 and length-bounded. Callers set both the byte length and trailing
NUL; embedded NULs, malformed/overlong encodings, invalid enums, and oversized
records are rejected. Values returned by `get` are copies and remain valid after
the store is closed.

Changes are made in memory with `put` and `remove`, then committed with `save`.
The snapshot format starts with `LXPEERS`, a format version, bounded record and
payload counts, and a CRC-32 over the complete payload. Multi-byte integers use
network byte order. A save writes and fsyncs `path.tmp`, atomically renames it,
then fsyncs the parent directory. At open, a valid main file wins and stale temp
data is discarded. If the main file is absent or corrupt, a complete valid temp
snapshot is promoted, providing recovery from interruption around the rename.

The format version is currently `1`. Unknown versions and corrupt or duplicate
records fail closed; they are never partially loaded.
