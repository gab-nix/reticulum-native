# reticulum-c

A portable C17 implementation of the Reticulum networking stack and LXMF client.
The project is under active development; protocol interoperability is validated
against the reference Python implementations.

See [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) for the exact
implemented feature set and remaining interoperability work. The current CLI is
an early integration tool, not yet a replacement for Nomad Network.

## Requirements

- A C17 compiler (Clang or GCC)
- CMake 3.21 or newer
- OpenSSL 3.x development headers and libraries
- POSIX threads (macOS and Linux)

## Build and test

```sh
cmake -S . -B build -G Ninja -DRETICULUM_BUILD_TESTS=ON
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

That form reports `ONLINE`, ingests verified announces into the Network screen,
and attempts opportunistic LXMF delivery. See [docs/TUI.md](docs/TUI.md) for the
full key map and the persistence boundary.
