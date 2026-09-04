# Reticulum C

A portable C17 implementation of the Reticulum networking stack and LXMF client.
The project is under active development. Pinned protocol fixtures and specific
live Python interoperability checks exist, but the complete acceptance matrix
has not passed. This is not yet a drop-in replacement for Nomad Network.

> **Development disclosure:** Reticulum C was designed and implemented with
> extensive assistance from [OpenAI Codex](https://openai.com/codex/), under
> human direction and review. It is an independent compatibility project and
> is not affiliated with or endorsed by the Reticulum, LXMF, Nomad Network, or
> OpenAI maintainers. Its exact implementation provenance—including the
> source-reviewed Resource protocol—is documented in
> [COMPATIBILITY.md](docs/COMPATIBILITY.md) and
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

See the [release summary](docs/IMPLEMENTATION_STATUS.md) and canonical
[feature ledger](docs/FEATURE_STATUS.md) for implemented behavior, evidence and
remaining work. Compatibility targets the revisions recorded in the
[compatibility baseline](docs/COMPATIBILITY.md), not an unpinned latest release.
For continuing development, including isolated unfinished work and its safe
recovery order, read the [continuation handoff](docs/CONTINUATION_HANDOFF.md).

## Requirements

- A C17 compiler (Clang or GCC)
- CMake 3.21 or newer
- Ninja for the build commands below
- OpenSSL 3.x development headers and libraries
- POSIX threads and ncursesw (macOS and Linux)
- libbz2 for compressed Reticulum Resources

## Build and test

```sh
cmake -S . -B build -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_BUILD_APPS=ON \
  -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-sanitize -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_SANITIZERS=address,undefined
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

Set `RETICULUM_WARNINGS_AS_ERRORS=ON` for strict local and CI builds.

## Try the terminal chat UI

```sh
./build/apps/nomad-chat init my.identity
./build/apps/nomad-chat tui my.identity history.lxms 0123456789abcdef0123456789abcdef
```

The optional final value is a peer LXMF delivery address. Press `Enter` to
compose, `Esc` to cancel, arrow keys or `j`/`k` to select conversations, and
`q` to exit. Started this way the TUI reports `OFFLINE` and only persists
signed messages to the local outbox.

To run it against a real network, pass an interface configuration:

```sh
./build/apps/nomad-chat tui --config config/public-tcp.conf my.identity history.lxms
```

That form starts a caller-polled UDP/TCP runtime, ingests cryptographically
verified announces into the Network screen, and attempts proof-backed direct
LXMF delivery over reusable links. Messages remain visibly queued while a peer
identity, route, link, Resource slot, stamp, or propagation node is missing.

Inside the TUI:

- Press `N`, select a node with `j`/`k`, and press `Enter` for node details.
- Press `b` in node details to browse an announced Nomad page, when available.
- Press `m` to open the node's announced LXMF inbox, when available.
- Press `S` for Settings. You can edit the display name and announce schedule,
  then choose **Announce Now** to announce the local `lxmf.delivery` address.
- Press `C` to return to conversations, and `a` to compose to a 32-hex address.

The identity, history, peer state, node registry, and settings are local private
data. Keep them out of Git. The application stores companion state beside the
message store (for example `history.lxms.peers`, `.nodes`, and `.settings`).

See [docs/TCP_SETUP.md](docs/TCP_SETUP.md) for direct TCP configuration,
[docs/TUI.md](docs/TUI.md) for the complete key map, and
[docs/FEATURE_STATUS.md](docs/FEATURE_STATUS.md) for precise evidence and gaps.

## Current release boundary

The native library and early client include:

- UDP/TCP networking, verified announces, packet receipts, authenticated inbound
  and outbound links, request handlers, and bounded segmented Resource transfer.
- Proof-backed direct LXMF over reusable links and Resources, opportunistic
  packets, ratchet history, ticket reuse, bounded asynchronous stamp generation,
  and durable delivery state. Accepted incoming messages retain their full
  representation, including title, unknown fields and stamps.
- Core inbound source-block and message-size policy, with stamp enforcement
  independent of contact trust. Application preferences remain separate from
  wire validation and cryptography.
- Network discovery, remote Micron page requests, conversation/history access,
  and Settings-controlled `lxmf.delivery` announcements.
- An opt-in [static page hosting API](docs/HOSTED_NODE.md) and a caller-polled
  [propagation client session API](docs/PROPAGATION_CLIENT.md). Manual
  propagation upload/sync is wired into the router and TUI, but automatic
  scheduling, durable resume and complete hosted-node controls remain.

Each Resource advertisement remains limited to 74 carried hashes, while the
core sender/receiver can advance across bounded segments. Not every production
delivery path has integrated or verified multi-segment behavior. The 8 MiB
codec/storage bound does not imply that all messages of that size can be
delivered: the history content-preview admission limit is 4 KiB and the journal
quota is 16 MiB.

Major remaining work includes persistent Resource resume and adverse-network
recovery; complete retry and offline propagation workflows; live verification
of Micron forms and anchors, executable pages and remote file downloads;
hosted-node/propagation application controls; complete RRC and remaining TUI
workflows; multi-hop transport parity; upstream verification of AutoInterface,
shared IPC, KISS and RNode; and physical RNode validation.

## Interoperability evidence

[Live opportunistic testing](docs/LXMF_LIVE_TESTING.md) records bidirectional
short identity-key LXMF packets against pinned Python RNS/LXMF over loopback UDP.
[Live direct testing](docs/LXMF_DIRECT_INTEROP.md) records 17-byte packet and
2,048-byte incompressible Resource messages in both directions, including final
proofs and retained title/unknown fields. These narrow gates do not establish
transport-hop, reconnect, ticket/ratchet or complete TUI interoperability.
Python-generated fixtures and C-to-C tests cover additional layers but are not
substitutes for the complete upstream matrix. The feature ledger and linked
reports record the exact scope of each result; no full NomadNet or Reticulum
parity is claimed.

## Security and private data

Treat every checkout as though it may become public. Never commit Reticulum
identities or private keys, LXMF histories and companion stores, local settings,
private interface configuration, packet captures, or message logs. The supplied
Git ignore rules cover the common generated names, but they are not a substitute
for reviewing every staged change before committing. See
[SECURITY.md](SECURITY.md) for vulnerability reporting and the repository's
public-safety policy.

The tracked `config/public-tcp.conf` is a non-secret example containing public
TCP endpoints. Put machine-specific changes in a separately named ignored local
configuration such as `config/client.local.conf`.

## License

No open-source license has been selected yet. Until a license file is added,
copyright remains with the contributors and publication of the source does not
grant permission to copy, modify, or redistribute it.
