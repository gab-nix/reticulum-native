# Compatibility baseline

This project is a clean-room C implementation. Protocol behaviour is verified
against released upstream software, but upstream implementation source is not
copied into this repository.

The initial interoperability baseline, resolved on 2026-09-01, is:

| Component | Release | Git commit |
| --- | --- | --- |
| Reticulum | 1.5.2 | `e983c1e30b5929fbf588f65f83da822650003c45` |
| LXMF | 1.1.0 | `38dd74bed0fdac6aba291e64df2ae0e39688f493` |
| Nomad Network | 1.2.0 | `29e9ed619769067aa3fad7b0a5d9c05b6c9dc4c2` |

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

