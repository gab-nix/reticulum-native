# Live pinned LXMF opportunistic testing

The opt-in `tools/test_lxmf_live_python.py` harness exchanges real encrypted
Reticulum UDP packets in both directions between the C command-line client and
the pinned Python RNS/LXMF implementations. It is not a mock decoder comparison.

Build the C apps first. Use Python 3.9 or newer and clean, user-supplied upstream
checkouts at these exact revisions:

- Reticulum: `ea98db4f53dcf0defc0e71a16e60d28b1229c4e6` (1.5.2).
- LXMF: `795fdaa2b0777c13033787d933d1afc94a2377cb` (1.1.0).

```sh
python3 tools/test_lxmf_live_python.py \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --nomad-chat /path/to/reticulum-c/build/apps/nomad-chat \
  --rnid /path/to/reticulum-c/build/apps/rnid \
  --native-commit FULL_40_HEX_SOURCE_COMMIT_USED_TO_BUILD_APPS \
  --timeout 30 \
  --output /tmp/lxmf-live-result.json
```

The report output is optional and must not already exist. The tool never clones
repositories, installs dependencies, joins a public network, uses the normal RNS
configuration, or opens user identities or histories. It requires loopback UDP
permission. Upstream imports must resolve inside the supplied pinned checkouts;
tracked modifications or mismatched commits are rejected.

The native source commit is explicitly caller-declared: the tool fingerprints
the actual executables but cannot infer their build provenance from binaries.
Use the commit from the clean source tree you built, not an assumed current HEAD.

For C → Python, `nomad-chat send-udp` sends to a real pinned `LXMRouter` delivery
destination. The router's delivery callback verifies the message signature,
source, destination and exact synthetic content. For Python → C, upstream
`LXMessage.send()` and RNS UDP transport send to `nomad-chat receive-udp`; its
signature-verifying decoder must return the exact expected source and content.
Fresh encrypted retries accommodate the C receiver's missing readiness signal.
The receiver and upstream singleton runtime run in an isolated process group
with a deadline and cleanup on interruption.

All generated identities, ratchets, stores and configuration live in a temporary
directory removed after child processes stop. Raw subprocess output and upstream
logs are discarded. The optional JSON contains only revision/binary fingerprints,
message IDs, byte counts, synthetic-content hashes and check results. It contains
neither message text nor key material. Do not replace synthetic messages with
private conversations.

## Exact scope of the successful gate

[Recorded result](LXMF_LIVE_OPPORTUNISTIC.json), run on 2026-09-03, passes in both
directions with RNS 1.5.2 and LXMF 1.1.0. The built executables are identified by
SHA-256 in the report. This earns `VERIFIED` only for **short identity-key
opportunistic packet interoperability over directly connected loopback UDP**.

The harness explicitly provisions public identities. It does not verify
announce discovery, path selection, the asynchronous C router, queued delivery,
packet proofs/receipts, ratchets, stamps, tickets, direct links, Resources,
propagation, TCP, transport hops, NomadNet UI behavior or full messaging parity.
In particular, it is not evidence that failed queued TUI messages are fixed.

The harness remains outside the default CTest suite because upstream checkouts
are optional. Its network-free safety checks can be run independently:

```sh
python3 tools/test_lxmf_live_python_unit.py
```

These cover clean/mismatched/modified revision checks, binary fingerprints and
process-group termination, including an exit race, and rejecting recovering
sanitizer diagnostics even when a C process exits successfully.
