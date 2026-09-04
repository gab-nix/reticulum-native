# Reticulum Native

Reticulum Native is an independent, portable C17 implementation of Reticulum,
LXMF and selected Nomad Network workflows. It is under active development and
is not yet a complete replacement for the upstream Python applications.

This project was created with extensive assistance from OpenAI Codex under
human direction and review. It is not affiliated with or endorsed by the
Reticulum, LXMF, Nomad Network or OpenAI maintainers.

## Feature status

The concise list of implemented, work-in-progress and planned functionality is
maintained in [docs/FEATURE_STATUS.md](docs/FEATURE_STATUS.md).

## Build

Requirements: CMake 3.21 or newer, Ninja, a C17 compiler, OpenSSL 3.x, bzip2,
POSIX threads and ncursesw.

```sh
cmake -S . -B build -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_BUILD_APPS=ON \
  -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the terminal client with a locally created identity, message store and
interface configuration:

```sh
./build/apps/nomad-chat init my.identity
./build/apps/nomad-chat tui --config config/client.local.conf \
  my.identity history.lxms
```

Never commit identities, private keys, message stores, local configuration,
packet captures or logs containing private traffic.

## Python interoperability tests

The optional live test group uses local Python peers and temporary synthetic
identities. Supply clean checkouts at these exact revisions:

- Reticulum: `ea98db4f53dcf0defc0e71a16e60d28b1229c4e6`
- LXMF: `795fdaa2b0777c13033787d933d1afc94a2377cb`
- NomadNet: `475c0ee2a0388cf8470e7f1e90d5decb67b579ea`

Install their Python dependencies in your test environment, then configure:

```sh
cmake -S . -B build -DRETICULUM_PYTHON_INTEROP=ON \
  -DRETICULUM_PYTHON_RNS=/path/to/Reticulum \
  -DRETICULUM_PYTHON_LXMF=/path/to/LXMF \
  -DRETICULUM_PYTHON_NOMADNET=/path/to/NomadNet
cmake --build build
ctest --test-dir build -L interop --output-on-failure
```

These tests require loopback socket access and may take several minutes each.
They cover direct and propagated LXMF over UDP/TCP, plus an RRC schema fixture
hub using upstream codecs. The RRC test does not certify a stock RRC server.
Use `ctest --test-dir build -LE interop` for the ordinary suite. Offline builds
leave `RETICULUM_PYTHON_INTEROP` disabled and require no Python installation.

## License

Copyright (C) 2026 Gabriele Fumagalli.

Unless a file states otherwise, this project is licensed under the GNU General
Public License, version 3 or, at your option, any later version
(`GPL-3.0-or-later`). See [LICENSE](LICENSE).
