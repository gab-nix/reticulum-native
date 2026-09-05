# Reticulum Native

Reticulum Native is an independent, portable C17 implementation of Reticulum,
LXMF and selected Nomad Network workflows. It is under active development and
is not yet a complete replacement for the upstream Python applications.

This project was created with extensive assistance from OpenAI Codex under
human direction and review. It is not affiliated with or endorsed by the
Reticulum, LXMF, Nomad Network or OpenAI maintainers.

## Feature status

Nomad pages are cached in memory for the browser session (up to eight pages and
1 MiB total). The default lifetime is 12 hours; a leading `#!c=<seconds>` page
directive overrides it, and `#!c=0` disables caching. Form submissions bypass
the cache. In Browser, `r` clears the session cache and requests a fresh page.
Cached pages are not saved to disk or shared between anonymous and identified
browsing instances.

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

Recipient identity discovery defaults to a five-minute wall-clock deadline.
Restarting does not renew it; expiry keeps the message and requires an explicit
retry. Backward system-clock adjustments can extend this wait. Message journals
written by this version use extended delivery records: back up your store before
upgrading, and do not open an upgraded store with an older binary.

Conversation text wraps to the available width. Page Up/Page Down scroll by
display lines. In Normal mode, `h`/Left focuses the chat list and `l`/Right
focuses history; Tab switches between them. `j`/`k` or Down/Up move in the
focused pane. Home/End jump to the oldest/latest visible history. The top bar
shows CHATS or HISTORY, including on narrow terminals. Escape leaves the editor,
then history focus, before exiting from the chat list. Uppercase `L` opens Logs.
While composing, navigation letters are ordinary text. Enter sends, Ctrl-N inserts a newline, and
Up/Down move through wrapped lines. The editor grows to four rows and scrolls
with the cursor; Escape leaves the editor without discarding the draft.
The top bar shows connection state, known nodes, unread messages and (on wider
terminals) cumulative interface RX/TX totals in KiB.

Press Escape to leave an editor, then `:` to open the command popup. Commands
include `chats`, `network`, `browser`, `settings`, `interfaces`, `config`, `logs`,
`guide`, `node`, `rrc`, `announce`, `sync`, `help` and `q`/`quit`. Enter executes,
Escape closes, and Up recalls the previous command. Commands are an explicit
application allowlist, not a shell. Normal-mode Ctrl-U/Ctrl-D page history;
inside editors the existing editing shortcuts remain unchanged. The composer
has a visible frame, delivery mode and byte counter. This is a Vim-inspired
interface, not a complete Vim emulation.

In Browser, press `i` to request identification for a restricted page, then
Enter to confirm or Escape to cancel. Your public identity is disclosed only
to that node for this session. Following a link to another node switches back
to anonymous browsing. Press `i` again to confirm returning to an anonymous
link; this cannot undo identity information already disclosed to the node.

## Heltec WiFi LoRa 32 V3.1 firmware

An early ESP-IDF project scaffold lives in
`firmware/heltec_wifi_lora_32_v3_1`. It currently provides the immutable board
descriptor, a safe OLED power/reset sequence, and the pinned Semtech SX126x
command driver. It also contains an application-independent SSD1306 status UI
core and bounded serial-shell parser/command registry. The UI core sequences
Vext and reset through the immutable V3.1 BSP, configures the 17/18 I2C pins
through a caller-supplied adapter, and fails independently of networking. The
shell requires explicit confirmation before standard identity import/export,
identity erase, or configuration reset commands and never echoes command
arguments in its own diagnostics. Confirmation is bound to a SHA-256 digest of
the complete invocation, and guarded commands are unavailable unless the
adapter supplies secure randomness and SHA-256. OLED message previews have a
separate enable setting; disabling previews immediately clears retained text.

The firmware now starts the SSD1306 through an ESP-IDF I2C adapter and opens
the SX1262 through its airtime-limited packet scheduler after crypto/storage boot checks. The
large-text diagnostic display shows radio state, RX count and unique verified
identity count (`IDS`). Identities are not physical device counts: one identity
can advertise multiple services, and one device can host multiple identities.
Display failures do not stop radio receive. The USB console
reports health counters without packet contents. RNode frames are reassembled
and announces are signature-verified before entering a bounded 32-destination
diagnostic table. Duplicate and older announces do not refresh entries; entries
expire after an hour. IFAC-protected packets are rejected until interface credentials are supported.
The profile uses 868.100 MHz, 250 kHz bandwidth, SF11, CR4/5 and an 18-symbol
preamble. Raw over-air reception has been observed with a matching external peer;
incoming announces have also passed signature verification on the board, with
stable heap and more than 2 KiB stack headroom. Outbound interoperability remains unverified.

Packet-mode LXMF now uses a persistent NVS identity and ratchet. Tap PRG to
open the menu, then tap to cycle Status, Last message, Announce and Clear message.
Hold for about one second and release to select. Announce queues a signed
`Heltec` delivery announce with a 60-second cooldown; ordinary taps never transmit.
Last message reopens the last verified preview retained in RAM; Clear message
erases that preview. Keep PRG released during reset: it is also the GPIO0 boot
strap. There is no automatic startup announcement. All TX, including message
proofs, uses CAD and a 1% rolling airtime budget; queue acceptance is not RF completion.
The airtime history is volatile and must not be bypassed by repeated rebooting.

Send a short **opportunistic** LXMF message to the discovered Heltec. The
development firmware saves verified messages before queuing an explicit proof;
Chats retains sender-grouped history. Unknown-sender integration is in progress:
the separate Unverified view labels claimed identities, hides invalid-signature
content and requires confirmation for deletion. Saved encrypted packets can be
rechecked after verified announces without acknowledging unknown senders.
The updated firmware compiles with ESP-IDF 5.5.4, but still requires physical
acceptance; host tests and compilation do not establish this behavior on the board.

History is **unencrypted on flash** in this development profile. Deletion is
logical journal deletion, not guaranteed forensic erasure. Clear View does not
delete saved chats. Packet contents are never logged. Compact OLED previews
substitute unsupported Unicode glyphs and label shortened text with an ellipsis.
Quick replies, transport forwarding, direct links/resources, large messages,
durable replay protection, ratchet rotation and stamp enforcement remain
unfinished. The full UART shell and full Reticulum runtime are not yet connected.
Announce time starts at firmware build time, with a persisted increasing floor
across restart; this is not a synchronized real-time clock.

The descriptor follows Heltec's official V3 board pin definition and the
upstream RNode `BOARD_HELTEC32_V3` mapping. V3.1 keeps the V3 radio/display
pinout but has distinct power circuitry. The V3.1 battery-sense circuit is
recorded as present but unvalidated and remains disabled. This firmware does
not assign GPIO37; Heltec documents that battery-control behavior for V3.2,
not V3.1.

- Heltec V3 pins: https://github.com/Heltec-Aaron-Lee/WiFi_Kit_series/blob/master/variants/heltec_wifi_lora_32_V3/pins_arduino.h
- Heltec revision log: https://github.com/HelTecAutomation/HeltecDocs/blob/master/doc/node/esp32/source/wifi_lora_32/hardware_update_log.md
- RNode board mapping: https://github.com/markqvist/RNode_Firmware/blob/master/Boards.h#L2633-L2699

The upstream Semtech SX126x driver is vendored at version 2.5.0, commit
`a10c5dfdf89788c6ac805e9fe98889de44175aa2`, under its Clear BSD License.
Exact included files, source URLs and SHA-256 checksums are recorded in
`third_party/semtech/sx126x_driver/UPSTREAM.md`. Optional upstream BPSK and
LR-FHSS modules are not included in the initial LoRa-only firmware profile.

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
suite validates the V3.1 descriptor plus simulated OLED and shell state, but an
ESP-IDF firmware build and physical board validation remain required before the
board support is operational.

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
The discovery tests exercise both directions against stock pinned Reticulum,
including recovery after a missed startup announcement. A registered C service
answers path requests only after its application explicitly announces it;
unregistering the service removes its retained announcement identity and data.
They cover direct and propagated LXMF over UDP/TCP, small and resource-backed
pages served by the pinned NomadNet page handler, Python requests to C-hosted
pages/forms and restricted pages, plus an RRC schema fixture
hub using upstream codecs. The RRC test does not certify a stock RRC server.
Use `ctest --test-dir build -LE interop` for the ordinary suite. Offline builds
leave `RETICULUM_PYTHON_INTEROP` disabled and require no Python installation.

## Optional static page hosting

Open **Node** with `O`. Set an absolute page root and a semicolon-separated list
of relative pages, such as `index.mu;about.mu`, then select **Start hosting**.
Only listed static files are published; traversal, symlinks and executable files
are rejected. The screen shows the hosted address while running. `j`/`k` select
an item and Enter edits or activates it; stop hosting before changing its root,
page list or access policy. Identified-only access requires clients to identify
their links, and existing per-page `.allowed` policies still apply.

Hosting is off by default. Enabling it persists startup behavior beside the
message store; **Stop hosting** disables it for subsequent starts. The service
announces after startup and at the configured Settings announcement interval;
**Announce now** is also available. A failed save cancels activation. Executable
pages, file serving and propagation-node hosting are not enabled by these controls.

## License

In Conversations, `i` opens peer information and `q` shows its address QR; `U`
shows your own address (also available in Settings). QR display requires a
UTF-8 terminal at least 40 columns by 23 rows; smaller terminals retain the
plain address. These QR codes contain only public addresses, not identities
or keys. Paper-message QR export and camera scanning are separate workflows.

The application vendors the MIT-licensed [Project Nayuki C QR encoder](https://github.com/nayuki/QR-Code-generator)
1.8.0 at `720f62bddb7226106071d4728c292cb1df519ceb`, with unmodified sources,
license headers and SHA-256 provenance in `third_party/qrcodegen`. It is not
linked into `libreticulum` or the embedded firmware.

Copyright (C) 2026 Gabriele Fumagalli.

Unless a file states otherwise, this project is licensed under the GNU General
Public License, version 3 or, at your option, any later version
(`GPL-3.0-or-later`). See [LICENSE](LICENSE).
