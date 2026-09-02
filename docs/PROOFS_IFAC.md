# Packet proofs and IFAC

This implementation follows the wire behaviour pinned to Reticulum 1.5.2.

An explicit packet proof is the 32-byte full packet hash followed by the
destination identity's 64-byte Ed25519 signature over that hash. An implicit
proof contains only the signature. Validation requires an exact 96- or 64-byte
representation and verifies it against the expected packet hash.

IFAC derives a 64-byte private identity key using SHA-256 over each configured
network-name/passphrase component, SHA-256 over their concatenation, then
HKDF-SHA256 with Reticulum's fixed IFAC salt. The tag is the requested trailing
bytes of the identity signature over the unmodified frame. Protecting inserts
the tag after the two-byte header, derives a frame mask using the tag and IFAC
key, masks everything except the tag, and keeps the IFAC flag set. Receiving
performs the inverse operation and compares a freshly calculated tag in
constant time.

The unit vectors were generated with the pinned Python implementation and use
fixed inputs so derivation, mask placement, proof layout, and signatures remain
deterministically testable.
