# Security policy

Reticulum C is pre-release software undergoing interoperability and robustness
testing. It has not received an independent security audit. Do not rely on it
for safety-critical or high-risk communications.

## Reporting a vulnerability

Please report suspected vulnerabilities privately through this repository's
GitHub Security Advisories instead of opening a public issue. Include the
affected revision, a minimal reproduction, and the expected impact. Do not
include real private keys, identities, message contents, or third-party network
captures.

## Public repository hygiene

All committed material must be safe to publish. Before pushing, review the
staged file list and staged diff and check that they contain none of the
following:

- Reticulum identities, private keys, ratchet secrets, or tickets;
- LXMF histories, drafts, contacts, node/path/peer stores, or private messages;
- local interface credentials, private endpoints, environment files, or tokens;
- packet captures, debug logs containing payloads, or generated build output.

Fixtures must use synthetic identities and omit plaintext private messages and
secret key material. Interoperability reports may record public hashes, public
keys, protocol states, counters, versions, and source or binary fingerprints.

The ignore rules are a safety net, not proof that a commit is clean. A secret
that was previously committed must be revoked or rotated even if it is later
removed from Git history.
