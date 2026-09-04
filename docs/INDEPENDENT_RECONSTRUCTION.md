# Independent reconstruction record

This document records the engineering separation used to replace the Resource
and transport/reverse-path implementations after a provenance audit found that
private development history included direct upstream source study. It is a
technical provenance record, not a legal opinion.

## Separation process

Two specification agents were restricted to public Reticulum C headers,
synthetic fixtures, privacy-safe interoperability reports, tests, callers, and
observable compiled behavior. They were expressly prohibited from reading the
existing affected implementation, Git history or diffs, `.claude` material, or
upstream Reticulum, LXMF, and Nomad Network source.

Their specifications were stored outside the repository before implementation:

- Resource specification: 482 lines, SHA-256
  `d8b3abf8576fd0cadac70ef415ef81b5d5e5fc11f7154195f82ff9f72d1ce40c`.
- Transport specification: 560 lines, SHA-256
  `92301e51375e2c03168a97b313ee56a8e405b18e277ae875589dc6980b3dc56b`.

Fresh implementation agents received only the applicable specification, public
tests and callers, unrelated library APIs, and compiler diagnostics. They were
prohibited from reading the replaced files, upstream source, repository history
or diffs, and implementations in other worktrees. The affected files were
deleted before being recreated.

Separate reviewers used the same restricted evidence boundary. Review found and
fixed exact-length validation, decompression bounds, advertisement invariant,
transactional segment advancement, duplicate announce selection, output-buffer
atomicity, transported-link request sizing, and interface-send rollback issues.

## Replaced boundary

The independently reconstructed implementation covers:

- `include/reticulum/resource.h` and `src/link/resource.c`;
- `include/reticulum/transport.h` and `src/transport/transport.c`;
- `include/reticulum/node.h` and `src/transport/node.c`;
- the narrow runtime completion/rollback integration required to commit
  forwarding state only after a successful interface send.

Other modules were not represented as independently reconstructed by this
process merely because they call these APIs.

## Verification

The combined replacement passed the complete 90-test warning-as-error suite.
Focused Resource and transport tests passed under AddressSanitizer and
UndefinedBehaviorSanitizer. The Resource replacement also passed the existing
privacy-safe bidirectional acceptance harness against pinned Reticulum 1.5.2 and
LXMF 1.1.0 over loopback UDP and continuous framed TCP, including packet-sized,
five-part, and greater-than-one-MiB two-segment transfers with final proofs.

The first UDP run correctly failed the gate and exposed an ambiguity in the
black-box specification: Python uses an authenticated encrypted-stream prefix
independent of the advertised random hash. The specification and implementation
were corrected from observed wire behavior, a regression test was added, and
both live transports then passed. No plaintext, private keys, identities,
histories, or raw captures were retained.

These results establish the narrow behavior described above. They do not prove
complete Reticulum or Nomad Network parity, and this record does not replace
qualified legal review.
