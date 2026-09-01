# Compatibility baseline

This project is a clean-room C implementation. Protocol behaviour is verified
against released upstream software, but upstream implementation source is not
copied into this repository.

The initial interoperability baseline, resolved on 2026-09-01, is:

| Component | Release | Git commit |
| --- | --- | --- |
| Reticulum | 1.5.2 | `ea98db4f53dcf0defc0e71a16e60d28b1229c4e6` |
| LXMF | 1.1.0 | `795fdaa2b0777c13033787d933d1afc94a2377cb` |
| Nomad Network | 1.2.0 | `475c0ee2a0388cf8470e7f1e90d5decb67b579ea` |

Golden vectors generated from these revisions must record the generator,
revision, inputs and expected bytes. Updating a baseline requires regenerating
the vectors and passing all differential interoperability tests.

## Compatibility policy

- The wire behaviour of the pinned releases is authoritative.
- Protocol extensions are added behind explicit capability negotiation.
- Unknown packet contexts and LXMF fields are retained or rejected safely; they
  are never silently reinterpreted.
- Claims of interoperability require a bidirectional test against the pinned
  Python implementation, not only tests between two C instances.
