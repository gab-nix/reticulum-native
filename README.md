# reticulum-c

A portable C17 implementation of the Reticulum networking stack and LXMF client.
The project is under active development. Pinned Python-generated protocol
fixtures exist, but complete live interoperability has not yet passed, so this
is not yet a drop-in replacement for Nomad Network.

See [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) for the exact
implemented feature set and remaining interoperability work. The current CLI is
an early integration tool, not yet a replacement for Nomad Network.

## Requirements

- A C17 compiler (Clang or GCC)
- CMake 3.21 or newer
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

UDP/TCP announces, packet receipts, bidirectional authenticated links,
proof-backed packet-sized direct LXMF, single-segment Resource transport,
Micron page requests, Settings announcements, and the early RRC envelope codec
have automated coverage. Large LXMF router integration, stamps/tickets policy,
propagation sync, hosted nodes, full RRC sessions, AutoInterface/shared IPC,
KISS/RNode drivers, multi-hop parity, and the physical RNode gate remain work in
progress. The feature ledger is authoritative if this summary ever lags.
