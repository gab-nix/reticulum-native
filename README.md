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

## Heltec WiFi LoRa 32 V3.1 firmware

An early ESP-IDF project scaffold lives in
`firmware/heltec_wifi_lora_32_v3_1`. It currently provides the immutable board
descriptor and a safe OLED power/reset sequence; it does not yet drive the
SX1262, OLED, Reticulum runtime or LXMF services.

The descriptor follows Heltec's official V3 board pin definition and the
upstream RNode `BOARD_HELTEC32_V3` mapping. V3.1 keeps the V3 radio/display
pinout but has distinct power circuitry. The V3.1 battery-sense circuit is
recorded as present but unvalidated and remains disabled. This firmware does
not assign GPIO37; Heltec documents that battery-control behavior for V3.2,
not V3.1.

- Heltec V3 pins: https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/blob/master/variants/heltec_wifi_lora_32_V3/pins_arduino.h
- Heltec revision log: https://github.com/HelTecAutomation/HeltecDocs/blob/master/doc/node/esp32/source/wifi_lora_32/hardware_update_log.md
- RNode board mapping: https://github.com/markqvist/RNode_Firmware/blob/master/Boards.h#L2633-L2699

The firmware pins ESP-IDF 5.5.4 and the `esp32s3` target. After installing
and activating ESP-IDF 5.5.4, configure and build it separately from the desktop
project:

```sh
cd firmware/heltec_wifi_lora_32_v3_1
idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build
```

Embedded cryptography uses ESP-IDF 5.5.4 and its Apache-2.0-licensed mbedTLS
component for SHA-256, HMAC, HKDF and AES-CBC. Ed25519, X25519 and constant-time
comparison use the ISC-licensed `espressif/libsodium` 1.0.22 component from the
[ESP Component Registry](https://components.espressif.com/component/espressif/libsodium).
Neither dependency is vendored. The component manifests pin both versions.
ESP-IDF generates `dependencies.lock` while resolving registry components; the
target CI job preserves that generated lock as an artifact until it can be
reviewed and committed from an actual ESP-IDF build rather than fabricated on a
host without the ESP-IDF toolchain.

Do not flash or transmit without a suitable 868 MHz antenna. The host test
suite validates the V3.1 descriptor, but an ESP-IDF firmware build and physical
board validation remain required before the board support is operational.

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
They cover direct and propagated LXMF over UDP/TCP, small and resource-backed
pages served by the pinned NomadNet page handler, plus an RRC schema fixture
hub using upstream codecs. The RRC test does not certify a stock RRC server.
Use `ctest --test-dir build -LE interop` for the ordinary suite. Offline builds
leave `RETICULUM_PYTHON_INTEROP` disabled and require no Python installation.

## License

Copyright (C) 2026 Gabriele Fumagalli.

Unless a file states otherwise, this project is licensed under the GNU General
Public License, version 3 or, at your option, any later version
(`GPL-3.0-or-later`). See [LICENSE](LICENSE).
