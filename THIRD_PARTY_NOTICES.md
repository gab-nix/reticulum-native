# Third-party and protocol provenance notices

Reticulum C is an independent C implementation intended to interoperate with
Reticulum, LXMF, and Nomad Network. It does not vendor those Python projects or
link to them at runtime. Their names identify compatibility targets and do not
imply affiliation or endorsement.

## Compatibility references

The exact upstream revisions used for protocol study and fixture generation are
recorded in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md). Most behavior was
implemented from documentation and observed wire behavior. The Reticulum
Resource wire format and certain transport behavior were additionally studied
in the pinned reference source because no complete independent byte-level
specification was available. No upstream source text was detected in the C
implementation. The affected implementation boundaries were later replaced
through the separated process recorded in
[`docs/INDEPENDENT_RECONSTRUCTION.md`](docs/INDEPENDENT_RECONSTRUCTION.md).
That record is deliberately narrower than a blanket legal clean-room claim.

Generated fixtures under `tests/fixtures/` contain synthetic protocol vectors
or privacy-filtered interoperability results. Their adjacent provenance records
identify the generator and upstream revision. The project's top-level license
does not replace any rights that may apply to third-party material.

At the recorded compatibility baseline:

- Reticulum and LXMF are distributed under the upstream Reticulum License.
- Nomad Network is distributed under GNU GPL version 3.

Review the exact licenses at the pinned revisions before redistributing
upstream software or source-derived material. This notice is a provenance
record, not a legal conclusion about copyrightability or derivation.

## Build dependencies

Reticulum C links to, but does not vendor:

- OpenSSL 3, distributed under Apache License 2.0;
- bzip2, distributed under its permissive upstream license when compression is
  enabled;
- ncurses, distributed under its permissive upstream license for the terminal
  application;
- operating-system threading, socket, and terminal facilities.

Binary distributors are responsible for including the notices and source or
offer materials required by the versions of these dependencies they ship.
