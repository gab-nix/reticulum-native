# Compatibility baseline

This project is an independent C implementation. Protocol behaviour is verified
against released upstream software, and no upstream implementation source is
copied into this repository.

Most of the stack was derived from observed wire behaviour and published
documentation. Private development history included direct study of the pinned
reference implementation for Resource and selected transport behavior where no
complete byte-level specification existed. Those implementation boundaries were
subsequently replaced through the independently separated reconstruction process
recorded in [`INDEPENDENT_RECONSTRUCTION.md`](INDEPENDENT_RECONSTRUCTION.md).
The public project describes that process precisely instead of making a blanket
legal claim about clean-room status or copyrightability of protocol behavior.

The initial interoperability baseline, resolved on 2026-09-01, is:

| Component | Release | Git commit |
| --- | --- | --- |
| Reticulum | 1.5.2 | `ea98db4f53dcf0defc0e71a16e60d28b1229c4e6` |
| LXMF | 1.1.0 | `795fdaa2b0777c13033787d933d1afc94a2377cb` |
| Nomad Network | 1.2.0 | `475c0ee2a0388cf8470e7f1e90d5decb67b579ea` |

Golden vectors generated from these revisions must record the generator,
revision, inputs and expected bytes. Updating a baseline requires regenerating
the vectors and passing all differential interoperability tests.

The first executable pinned fixture is the LXMF delivery announce application
data set in `tests/fixtures/lxmf_delivery_announce_vectors.h`. Its provenance is
recorded alongside it, and `tools/generate_lxmf_announce_fixtures.py` refuses to
run against source trees at different commits. The fixture exercises the
authoritative Python `LXMRouter.get_announce_app_data()` method and the C test
checks both encoding to those bytes and decoding from them. This is codec-level
evidence, not a live-network or complete messaging parity claim.

The signed message vectors in `tests/fixtures/lxmf_message_vectors.h` similarly
execute pinned Python `LXMessage.pack()` and its opportunistic packet builder.
They record exact signatures, message IDs, canonical packed bytes, and the
destination-prefix-elided opportunistic application payload. The corresponding
C test validates signing preimages, encoding, signature verification, decoding,
standard and unknown fields, and the 255-byte MessagePack binary boundary. The
fixture intentionally excludes random Reticulum ciphertext and therefore does
not establish live encrypted delivery interoperability.

The Reticulum link vectors in `tests/fixtures/rns_link_vectors.h` execute the
pinned Python packet, link-signalling, link-ID and identity-signing APIs. They
record deterministic public link-request and LRPROOF packets, their signing
preimages, RTT MessagePack plaintext and link-identification signing data. The
C test reconstructs the packets, verifies the signatures and checks locally
decrypted RTT plaintext. Random-IV ciphertext and live link establishment are
intentionally outside this fixture, so this is deterministic representation
evidence rather than bidirectional link interoperability.

The ordinary-proof reverse-path implementation was audited against the pinned
`RNS/Transport.py` and `RNS/Packet.py`: a forwarded packet is keyed by its
16-byte truncated packet hash, records ingress and downstream interfaces, is
culled after 480 seconds, and is consumed when a proof attempts to return.
`tests/fixtures/rns_reverse_path.provenance.json` records those inputs and the
separate ordinary explicit/implicit proof representation. The deterministic C
test is source-derived local evidence, not bidirectional transport interop.

The NomadNet RRC vectors in `tests/fixtures/nomadnet_rrc_vectors.h` execute the
pinned 1.2.0 `_make_envelope()` helper and vendored CBOR encoder/decoder. They
cover every defined envelope type, optional fields, nested capability/limit
maps, UTF-8, CBOR unsigned integer boundaries, resource metadata and unknown
keys. The C codec parses those Python bytes and emits the same canonical known
envelope. Unknown outer keys are bounded and skipped rather than retained;
unknown nested body values remain opaque CBOR. These fixtures do not establish
an encrypted session or live compatibility with an RRC hub.

## Compatibility policy

- The wire behaviour of the pinned releases is authoritative.
- Protocol extensions are added behind explicit capability negotiation.
- Unknown packet contexts and LXMF fields are retained or rejected safely; they
  are never silently reinterpreted.
- Claims of interoperability require a bidirectional test against the pinned
  Python implementation, not only tests between two C instances.
