# Continuation handoff: NomadNet parity

Updated: 2026-09-04

This document is the starting point for the next development task. It records
the repository state, unfinished isolated work, remaining feature work and the
release gates that must be passed before claiming NomadNet or Reticulum parity.
It complements, but does not replace, the canonical
[`FEATURE_STATUS.md`](FEATURE_STATUS.md) ledger.

## Read this first

Before changing code, read these files in order:

1. [`../AGENTS.md`](../AGENTS.md) for repository rules.
2. [`COMPATIBILITY.md`](COMPATIBILITY.md) for the exact upstream revisions.
3. [`FEATURE_STATUS.md`](FEATURE_STATUS.md) for feature-level evidence.
4. This handoff for priorities and unfinished work.

Do not infer parity from a passing local test suite. `IMPLEMENTED` means local
code and automated tests exist; only captured bidirectional behavior against
the pinned upstream stack can earn `VERIFIED`.

## Compatibility target

The authoritative pinned baseline is:

| Component | Release | Git revision |
| --- | --- | --- |
| Reticulum | 1.5.2 | `ea98db4f53dcf0defc0e71a16e60d28b1229c4e6` |
| LXMF | 1.1.0 | `795fdaa2b0777c13033787d933d1afc94a2377cb` |
| Nomad Network | 1.2.0 | `475c0ee2a0388cf8470e7f1e90d5decb67b579ea` |

The intended product is behavioral and wire compatibility, not a pixel-identical
copy of the Python/Urwid UI.

## Current checkpoint

The main branch entering this documentation checkpoint was `b68b007`.
The warning-as-error build contains 90 registered tests. At that checkpoint all
90 passed, and the focused node/startup sanitizer tests passed. Always rerun the
suite on the current checkout instead of treating this note as current evidence.

The most recent completed changes are:

- `b68b007 fix(tui): announce after interfaces connect`
- `342cac1 feat(tui): show peer relay and site roles`
- `21aefc7 fix(nomad): scale active node registry`

The client now waits for a usable TCP interface before its startup announce,
retries transient startup failures, and reannounces after a full reconnect. The
Network screen marks peer/inbox, active relay and site roles as `P`, `R` and `S`;
lowercase `r` means a valid but inactive/static-only propagation endpoint. The
node registry grows to a bounded maximum of 4,096 records instead of silently
stopping at 256.

On 2026-09-04 the user confirmed that the native client and Culumba on a phone,
attached to the same public TCP instance, discovered each other. This is useful
live smoke evidence for TCP attachment and announcement timing. It is not a
captured bidirectional messaging test and must not be recorded as full upstream
verification.

The normal local test build is:

```sh
cmake -S . -B build-werror -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_BUILD_APPS=ON \
  -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build-werror
ctest --test-dir build-werror --output-on-failure
```

The current runnable client is:

```sh
./build-werror/apps/nomad-chat tui \
  --config config/public-tcp.conf \
  my.identity history.lxms
```

Identity, history, settings, node/path stores, captures and generated
configuration are private local data and must never be committed.

## Preserve this local state

The main checkout has a user-owned modification to
`config/public-tcp.conf`: the RMAP World endpoint was removed while
`node.reticulumnet.nl:4242` remains. Do not stage, overwrite, restore or commit
that file unless the user explicitly asks.

The `.claude/` worktrees are separately owned and must remain quarantined until
reviewed. Do not delete them, reset them or merge them wholesale.

## Unfinished isolated work

These worktrees contain uncommitted work. Their branches are based on older
checkpoints and therefore must not be merged directly. Review each diff, port it
onto a new `codex/` branch from current main, run its focused and full gates,
update the feature ledger, make one focused commit, and only then merge it.

### 1. Multi-segment direct LXMF delivery

- Worktree: `/private/tmp/reticulum-lxmf-multisegment`
- Branch: `codex/lxmf-multisegment-direct`
- Base: `7fd9e76`
- Uncommitted scope: 6 files, approximately 222 insertions and 12 deletions.
- Touched areas: runtime public API, runtime Resource dispatch, LXMF send path,
  direct-router integration tests and the feature ledger.

This is the highest-priority recovery because the core Resource engine can
already assemble multiple segments locally, while the production LXMF router
still needs complete direct-message selection, progress, proof and retry
behavior for those transfers.

### 2. Conversation reply and reaction composition

- Worktree: `/private/tmp/reticulum-reply-reaction`
- Branch: `codex/conversation-reply-reaction`
- Base: `0655f49`
- Uncommitted scope: 5 files, approximately 247 insertions and 18 deletions.
- Touched areas: TUI state, dispatcher and renderer.

Review only after the delivery path is stable. It must preserve arbitrary LXMF
fields, drafts and existing conversation state, and needs focused headless input
and rendering tests before integration.

### 3. Micron forms, anchors and caching

- Worktree: `/private/tmp/reticulum-browser-forms`
- Branch: `codex/browser-forms-cache`
- Base: `843d1c3`
- Uncommitted scope: 12 tracked files, approximately 662 insertions and 42
  deletions, plus untracked
  `tests/fixtures/micron_forms.provenance.json`.
- Touched areas: Micron/browser APIs and parser, TUI state/rendering, unit tests
  and the feature ledger.

Do not accept the provenance file solely because it has that name. Confirm its
generator, pinned source revision, inputs and privacy before committing it.

### 4. Claude Resource experiment

- Worktree: `.claude/worktrees/agent-a1b92701ef3f668f6`
- Branch: `worktree-agent-a1b92701ef3f668f6`
- Uncommitted scope: 5 files, approximately 1,106 insertions and 215 deletions.
- Touched areas: Resource/runtime public APIs, Resource implementation and
  tests.

This overlaps the current Resource and multi-segment work heavily. Treat it as
an experiment to audit for useful cases, not as a merge candidate. The other
Claude worktree, `agent-a1838e0c5e83f7047`, was clean at this checkpoint.

## Recommended continuation order

### Priority 0: make messaging dependable for real users

1. Recover and review the multi-segment direct-LXMF work on a fresh branch.
2. Run live native-to-Culumba send and receive tests, including replies and
   final receipt state, over the same TCP instance that now supports discovery.
3. Ensure every queued message exposes one actionable prerequisite: peer
   identity, path, stamp, link, Resource slot or propagation node.
4. Exercise disconnect/reconnect, process restart, duplicates, cancellation,
   expired paths and retry exhaustion without losing history or queued work.
5. Capture privacy-safe evidence: public IDs, state transitions, contexts,
   counters, versions and binary/source fingerprints; never message plaintext,
   private keys or derived secrets.
6. Verify live ratchet and ticket exchange, stamp-required peers and one-hop
   transport delivery against the pinned Python stack.
7. Complete automatic propagation scheduling, durable transient data,
   interrupted upload/download recovery and automatic/manual sync behavior.

Success means short, large and offline messages work in both directions with
truthful `QUEUED -> SENDING -> SENT -> DELIVERED` transitions. `DELIVERED`
requires the appropriate valid packet, link, Resource or recipient proof; a
socket write or propagation-node upload is not recipient delivery.

### Priority 1: finish the conversation experience

1. Recover reply/reaction composition after the router gate passes.
2. Add selection among multiple attachments and explicit safe save actions.
3. Add URI/QR import/export UI, notifications, unread behavior and delivery
   method controls with headless tests.
4. Verify trust/block/note/pin, drafts, search, retries, cancellation and
   signature display as complete workflows with a stock client.

### Priority 2: finish browser and hosted-node behavior

1. Recover the isolated Micron form/anchor/cache work.
2. Verify small and multi-segment pages, relative and cross-node links, anchors,
   editable forms, submissions, cache directives, reload bypass and file
   downloads against a stock NomadNet node.
3. Preserve the last successful page when a later load fails; never render
   placeholder content as if it were a successful remote response.
4. Finish bounded remote file metadata/streaming and hosted-node application
   controls.
5. Add a deliberately opt-in, bounded executable-page worker with deadlines,
   allowlists and no shell execution, then test it against stock NomadNet.

### Priority 3: complete network and Reticulum behavior used by NomadNet

1. Add concurrent peers to the native TCP server; it currently supports one
   accepted connection at a time, although it can accept another after that
   peer disconnects.
2. Finish transport persistence, tunnels, path rebalancing, shared-instance hop
   behavior, backchannel routing and multi-hop reverse-path proof handling.
3. Complete adaptive Resource windows/retry timing, persistent resume,
   metadata-bearing segmented files and very-low-MTU behavior.
4. Add live pinned-Python tests for AutoInterface and shared-instance IPC.
5. Add live TNC validation for KISS and live Python/physical-hardware validation
   for RNode. Simulated pseudo-terminal tests are not physical verification.
6. Finish IFAC/interface-mode/announce controls and dynamic AutoInterface
   device/address changes.

### Priority 4: complete remaining NomadNet workflows

1. RRC: persistent history, room discovery/WHO, member nicknames, mentions,
   Resource envelopes, commands, operator events, rate enforcement, external
   hubs, hosted hubs and complete reconnect behavior.
2. Network: richer path/interface diagnostics, propagation peer management,
   stale/expiry presentation and stock-client workflow evidence.
3. Settings/Config/Interfaces/Logs/Guide/Node/Directory: complete editable
   configuration and safe runtime apply/restart, hosted-node and propagation
   controls, and headless coverage for every overlay and narrow terminal.
4. CLI: implement the remaining operational utility behavior and stable JSON
   schemas; current tools are not complete replacements for the RNS utilities.
5. Embedded: provide cross-build examples and a non-OpenSSL crypto provider.

## Known implemented-but-not-fully-verified areas

- UDP and reconnecting TCP clients work locally; continuous UDP/TCP direct and
  propagation exchanges have narrow pinned-Python evidence.
- AutoInterface, shared-instance IPC, KISS and RNode serial have deterministic
  local/simulated tests but not the complete upstream or hardware matrix.
- Authenticated links, requests, Resources and transport forwarding exist, but
  important multi-hop, retry, persistence and live upstream cases remain.
- Opportunistic and direct LXMF packet/Resource paths have narrow bidirectional
  pinned-Python evidence. This is not complete messaging parity.
- Static hosted pages and a propagation client API exist, but full hosted-node
  controls, propagation-node operation and automatic client scheduling do not.
- The native browser loads basic remote Micron pages, but forms, anchors,
  caching, richer constructs and stock-node evidence remain incomplete.
- The RRC client completes one narrow pinned-schema UDP exchange; the full room
  and hub experience remains unfinished.

## Required gate for every recovered feature

For each major feature or bug fix:

1. Start from current main on a separate `codex/` branch/worktree.
2. Preserve unrelated changes and private local state.
3. Keep library protocol/crypto/state machines separate from curses and app
   persistence.
4. Bound every untrusted input, allocation, collection and copy.
5. Add deterministic success, malformed-input, timeout, cancellation and
   recovery tests appropriate to the change.
6. Update [`FEATURE_STATUS.md`](FEATURE_STATUS.md) in the same commit without
   overstating evidence.
7. Run the warning-as-error build and all tests, then focused ASan/UBSan tests.
8. Run `git diff --check` and review the staged diff for private data.
9. Commit one focused change and merge only after all applicable gates pass.

## Full parity release gate

Do not call the project 100% compatible until all of the following are recorded
against the pinned revisions:

- C-to-Python and Python-to-C announces, paths, proofs, links, requests,
  channels and Resources, including one-hop transport and adverse networking.
- Opportunistic, direct packet, direct multi-segment Resource, stamp, ticket,
  ratchet and propagation messaging in both directions with restart/reconnect.
- Discovery, messaging, pages, links, forms, executable pages, files, hosting,
  propagation and RRC through stock NomadNet-compatible clients.
- UDP, framed TCP, AutoInterface, shared IPC, simulated KISS/RNode and physical
  RNode evidence.
- Warning-clean GCC and Clang builds, ASan, UBSan, leak checks, static analysis,
  fuzz/malformed corpora and headless TUI tests for every screen and overlay.

The scope beyond NomadNet-required behavior—complete `rnsd` transport-daemon and
all core RNS utility parity—remains a subsequent milestone and must be tracked
separately rather than silently included in a NomadNet parity claim.

## First instruction for the next task

Begin by validating current main and inspecting the four unfinished work areas
above without editing them. Then recover `codex/lxmf-multisegment-direct` onto a
fresh branch, review it against the pinned Python behavior, and make direct LXMF
delivery pass strict, sanitizer and live bidirectional tests. Do not start new
UI work until that delivery checkpoint is committed and merged.
