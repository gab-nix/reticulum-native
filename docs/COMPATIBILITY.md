# Compatibility baseline

This project is an independent C implementation. Protocol behaviour is verified
against released upstream software, and no upstream implementation source is
copied into this repository.

Most of the stack was derived from observed wire behaviour and the published
documentation. The Resource sub-protocol is the exception: no byte-level
specification for it is published, and the upstream manual states that the
Python reference implementation is the only complete authority. Its wire format
(advertisement fields, part-request layout, map hash derivation, assembly order
and proof) was therefore read from the pinned reference implementation and
reimplemented in original C. This project is consequently not clean-room with
respect to the Resource sub-protocol, and the earlier clean-room claim has been
withdrawn rather than left inaccurate.

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

## Compatibility policy

- The wire behaviour of the pinned releases is authoritative.
- Protocol extensions are added behind explicit capability negotiation.
- Unknown packet contexts and LXMF fields are retained or rejected safely; they
  are never silently reinterpreted.
- Claims of interoperability require a bidirectional test against the pinned
  Python implementation, not only tests between two C instances.
