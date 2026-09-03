# Release status

This is a development checkpoint, not a NomadNet-compatible v1 release.
[FEATURE_STATUS.md](FEATURE_STATUS.md) is the canonical detailed ledger;
[COMPATIBILITY.md](COMPATIBILITY.md) records the pinned RNS 1.5.2, LXMF 1.1.0
and NomadNet 1.2.0 revisions. A local test pass is not upstream verification.

## Available now

- C17 library and POSIX UDP/TCP runtime with verified announces, packet receipts,
  bidirectional authenticated links, request handlers and single-segment
  Resource sending/receiving.
- Native LXMF direct packet/Resource and opportunistic delivery; persistent
  ratchets and tickets; caller-polled outbound stamps; inbound block, stamp and
  size policies; and durable messages retaining full incoming representations.
- An ncurses client with conversations, Network discovery, remote Micron page
  requests and Settings announcements, plus the existing diagnostic CLI tools.
- Opt-in [static page hosting](HOSTED_NODE.md) and a bounded
  [propagation client session](PROPAGATION_CLIENT.md) at library level. Complete
  hosting and propagation application workflows are not yet available.

## Evidence and release gates

The repository has strict Clang builds, unit/headless tests, loopback UDP/TCP
integration tests, focused ASan/UBSan coverage, and provenance-recorded pinned
Python protocol fixtures. The [live opportunistic report](LXMF_LIVE_TESTING.md)
verifies short identity-key packets in both directions over loopback UDP.
The [live direct report](LXMF_DIRECT_INTEROP.md) verifies bidirectional 17-byte
packet and 2,048-byte incompressible Resource messages, including proofs and
retained metadata, against the pinned Python stack over loopback UDP. These
reports are narrow evidence, not full messaging or TUI parity. See each report
and the ledger for source revisions and exclusions. Build and test commands
are in the [README](../README.md#build-and-test).

Remaining release gates include multi-segment Resources/resume; complete retry,
propagation and offline delivery; full Micron forms, executable pages and remote
files; hosted-node and propagation-node operation in the apps; RRC sessions and
remaining TUI workflows; broader multi-hop routing and bidirectional upstream
one-hop evidence; AutoInterface/shared IPC/KISS/RNode; broader upstream tests;
and physical RNode validation.

Current bounds remain explicit: one 74-part Resource segment, an 8 MiB packed
message limit, a 4 KiB content-preview admission limit and a 16 MiB journal quota.
Increasing a storage limit does not complete the missing transport behavior.
