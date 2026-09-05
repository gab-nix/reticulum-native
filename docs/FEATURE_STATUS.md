# Feature status

This is the complete public feature ledger. It tracks implementation state only
and does not certify complete Reticulum, LXMF or Nomad Network parity.

- `IMPLEMENTED`: available in the current C implementation.
- `VERIFIED`: a specifically scoped behavior passes bidirectional pinned-upstream tests.
- `WIP`: partially implemented; important behavior remains.
- `PLANNED`: not implemented.

## Foundation and networking

| Feature | Status | Remaining work |
| --- | --- | --- |
| C17 library, injectable platform HAL, buffers and typed status API | IMPLEMENTED | ESP-IDF provider is implemented; target build and physical validation remain |
| Replaceable crypto provider API | IMPLEMENTED | ESP-IDF mbedTLS/libsodium provider contract passes host RFC, boot self-test and cross-provider token vectors; an ESP-IDF 5.5.4/esp32s3 CI build gate is configured but has not run on this branch, and device validation remains |
| Transactional storage and nonblocking interface provider APIs | IMPLEMENTED | Bounded serialized dual-slot NVS has host fake-backend transaction and corruption tests, including hypothetical buffered-commit recovery; pinned ESP-IDF 5.5 direct-set/commit-no-op target validation and SX1262 validation remain |
| Explicit platform-separated source manifests | WIP | Platform/storage component uses an explicit embedded subset; full portable protocol-core consumption remains |
| Cryptography, identities, destinations and packets | IMPLEMENTED | Broader platform validation |
| Identity code without OpenSSL or POSIX dependencies | IMPLEMENTED | Identity erasure uses the platform secure-zero API; `test_crypto_no_default` links the identity implementation with neither host provider and checks pre-provider failure/erasure. ESP-IDF explicitly compiles identity; persistent firmware identity and device exchange remain WIP |
| Signed announces and path lookup | WIP | Complete persistence and routing compatibility |
| UDP interfaces | IMPLEMENTED | Adverse-network hardening |
| TCP client and server interfaces | IMPLEMENTED | Broader reconnect and topology coverage |
| AutoInterface | IMPLEMENTED | Broader platform compatibility |
| Shared-instance local IPC | IMPLEMENTED | Complete shared-instance behavior |
| KISS serial | IMPLEMENTED | Hardware interoperability coverage; nonblocking backpressure CI coverage exists |
| RNode serial | IMPLEMENTED | Hardware interoperability coverage; chunked handshake and asynchronous old-firmware rejection tests exist |
| IFAC processing | IMPLEMENTED | Broader interface integration |
| Transport forwarding and reverse paths | WIP | Complete multi-hop transport behavior |
| Transport test endpoint reservation | IMPLEMENTED | `test_runtime_link_transport` holds all four UDP reservations until runtime handoff to prevent self-reuse of ephemeral ports. This fixes the observed uniqueness assertion; external-process bind races during handoff remain outside this guarantee |
| Persistent node and path registries | IMPLEMENTED | Migration and long-running deployment coverage |

## Links, requests and resources

| Feature | Status | Remaining work |
| --- | --- | --- |
| Outbound and inbound encrypted links | WIP | Complete lifecycle and multi-hop behavior |
| Link identification and packet proofs | IMPLEMENTED | Broader failure recovery |
| Channels | WIP | Complete channel behavior |
| Requests and request handlers | IMPLEMENTED | Complete access-policy compatibility |
| Resource sending and receiving | WIP | Persistent resume and adverse-network recovery |
| Segmented large resources | IMPLEMENTED | Bounded four-part sliding windows and RTT/airtime-aware retry exhaustion have local unit and runtime coverage; persistent resume and pinned-upstream adverse-network verification remain |
| Resource part-hash collision recovery | IMPLEMENTED | `test_resource_sender` injects a collision and validates remapped full transfer; repeated collisions fail after eight attempts with cleanup. Rare CI timeout cause remains unproven; direct test exposes bounded progress diagnostics |

## LXMF messaging

Reliability work is owned by the desktop task; firmware and physical-radio gates
remain with the Heltec task. An implemented mechanism does not complete its
remaining acceptance requirements below.

| Reliability acceptance item | Status | Evidence / remaining gate |
| --- | --- | --- |
| Persisted recipient discovery deadline and explicit retry | IMPLEMENTED | `test_lxmf_identity_discovery` covers deadline expiry, offline restart, identity-arrival bypass, no attempt inflation and manual retry. `test_lxmf_store_delivery` covers old records, delivery updates and compaction. Five-minute default uses wall time; backward clock adjustment handling remains WIP. Local policy tests are not upstream verification |
| Clock changes during discovery | WIP | Persisted wall deadline survives restart, but rollback can extend the wait; add a bounded monotonic session guard and deterministic clock-change tests |
| Public identity lifetime independent of routes | WIP | Current recall depends on unexpired path snapshots; add a separately bounded durable verified-key store and eviction/restart tests without implying service availability |
| Fair deferred propagation sync | WIP | Process-local pending sync scheduling and cancellation are implemented below; durable intent/restart and adverse-network scheduling against pinned peers remain |
| Failed durable writes during queue transitions | WIP | Retain existing records and drafts; inject failures at each metadata/status transition and verify no false delivery or automatic revival of terminal messages |

| Feature | Status | Remaining work |
| --- | --- | --- |
| Message codec, signatures and unknown fields | IMPLEMENTED | Broader compatibility coverage |
| Standard reply, reaction, thread and media fields | IMPLEMENTED | Complete user-facing workflows |
| Opportunistic packet delivery | IMPLEMENTED | Complete ratchet and retry workflows |
| Direct link packet delivery | IMPLEMENTED | Complete reconnect and relay behavior; integration test IDs are distinct from random wire hashes |
| Direct resource delivery | IMPLEMENTED | Persistent transfer resume |
| Delivery proofs and durable queue state | IMPLEMENTED | Complete offline retry scheduling |
| Missing-peer discovery retries and queue fairness | IMPLEMENTED | `test_lxmf_identity_discovery` checks 15-second retry throttling, immediate identity-arrival recovery and broadcast fanout over two UDP interfaces; `test_lxmf_router_recovery` checks round-robin polling, failed-message recovery and restart clock rebasing without transmission-attempt inflation. `test_tui_state` checks background queue events do not overwrite Network actions. Deadline and clock-change gates are tracked separately above |
| Local announced-service path responses | IMPLEMENTED | `test_runtime_path_response` verifies fresh signatures, app-data/ratchet retention, ingress-only replies, duplicate/cooldown limits and last-registration cleanup. Explicitly announced registered services only; cached-route transport responses remain WIP |
| Identity resolution after Network entry expiry and restart | IMPLEMENTED | `test_tui_state` retains a cryptographically learned runtime identity after registry eviction and runtime export/destroy/recreate/import, validates its LXMF destination hash, and rejects unknown destinations; stock-peer restart messaging remains outside this evidence |
| Recipient identity recall across service announces | IMPLEMENTED | Runtime bounded public-key recall derives and checks the requested service hash across unexpired paths. `test_tui_state` covers delivery/site/propagation announces, registry expiry, restart, unknown and expired identities. `test_lxmf_router_propagation` uploads a recipient-encrypted message learned only via its site announce, without an inbox route; unrelated relay keys cannot substitute for the recipient |
| Propagated queue status ownership | IMPLEMENTED | `test_tui_state` ignores uncached background queue events and distinguishes missing recipient identity from relay availability; newly queued messages enter the UI cache before synchronous send callbacks so progress is not replaced with stale queued state |
| Selected propagation node restart recovery | IMPLEMENTED | `test_tui_settings_ui` learns a signed synthetic propagation announce, saves/closes/reopens the app and starts sync without another announce. Cached metadata requires a matching unexpired, responsive path, public key and announce timebase; its startup lifetime follows the rebased path, not old monotonic registry timestamps. Reachability remains unconfirmed. `test_tui_state` rejects changed keys/timebases, disabled nodes, invalid costs and expired paths. Stock-peer restart exchanges remain unverified |
| Propagation busy and queue failure diagnostics | IMPLEMENTED | `test_tui_state` distinguishes queued sync from an already-running sync; `test_tui_settings_ui` checks readable queue errors retain the composer and do not insert a message. Missing selected-node information triggers an explicit path refresh on sync. The reported user's separate queue failure remains unresolved |
| Public peer identities in path snapshots | IMPLEMENTED | Version 2 stores only already-attached public keys; `test_path_store` covers mixed identity/route-only records, bounds, corruption, malformed identity flags, transactional rejection, offline expiry and version-1 route-only migration. Checksummed snapshots are trusted local storage, not independently authenticated announces |
| Full-capacity TUI path persistence | IMPLEMENTED | `test_tui_paths` saves and reloads 4096 synthetic paths with public identities and maximum blob history, exceeding the former 64 KiB cap; saves allocate queried size within a 2 MiB ceiling and retain atomic replacement/symlink rejection |
| Incoming block, size and stamp policies | IMPLEMENTED | Complete policy controls |
| Ratchet history | WIP | Rotation and enforcement parity |
| Stamps and asynchronous stamp generation | WIP | Complete cancellation and peer-policy behavior |
| Tickets | WIP | Complete issuance and renewal workflows |
| Trusted-contact outgoing tickets | IMPLEMENTED | `test_tui_fields_ui` checks issue/reuse before signing, no ticket for unknown/blocked recipients, and delivered-ticket throttling; full TUI-to-upstream acceptance remains |
| Ticket delivery throttling | IMPLEMENTED | Packet/link/resource proof completion records included locally issued tickets; `test_lxmf_router_receipt` checks no throttling at socket-send, proof-only throttling, restart and one-day expiry; resource-ticket persistence failure coverage remains |
| Python fractional ticket expiry | IMPLEMENTED | `test_lxmf_tickets` accepts float32/64 fractions and rejects NaN/infinity/negative/overflow; integer-second storage rounds expiry down by less than one second, never extending validity |
| Ticket fields composition | IMPLEMENTED | `test_lxmf_fields` covers add/replace/remove, opaque preservation, 64-bit expiry, output object-budget roundtrips, bounds and malformed input; issuance/delivery scheduling remains |
| Propagation upload | WIP | Automatic scheduling and durable resume |
| Propagation completion persistence recovery | IMPLEMENTED | `test_lxmf_router_propagation` exhausts synthetic journal capacity before metadata and status completion writes; no SENT event occurs until recovery, only one upload occurs, attempts remain unchanged, save retries are limited to once per second and unchanged errors are suppressed. `test_tui_state` checks actionable storage feedback. Completed session is process-local; crash/restart recovery may safely reupload and upstream duplicate acceptance remains a separate gate |
| Propagation download and acknowledgement | WIP | Automatic synchronisation and recovery |
| Deferred propagation sync behind uploads | IMPLEMENTED | `test_lxmf_router_propagation_sync` covers one accepted pending intent, repeated starts, cancellation without cancelling upload, node-change exclusion and automatic start after slot release; `test_tui_state` checks queued/cancel feedback. Polling services accepted sync before new uploads. Deferred intent is process-local; restart persistence and adverse-network upstream scheduling verification remain WIP |
| Paper messages and `lxm://` URIs | IMPLEMENTED | QR rendering and scanning |
| Peer and message persistence | IMPLEMENTED | Store migrations and maintenance controls |
| Refuse unknown message journal formats without modification | IMPLEMENTED | `test_lxmf_store` appends synthetic unknown versions, record types and reserved flags; repeated opens fail with every byte preserved and prior messages remain usable after fixture removal. Incomplete known-header tails still recover. This protects this reader, not older released binaries; generalized unknown-field migration remains separate |

## Nomad client

| Feature | Status | Remaining work |
| --- | --- | --- |
| Terminal application shell and screen dispatcher | IMPLEMENTED | Additional accessibility controls |
| Contextual chat-pane keyboard navigation | IMPLEMENTED | `test_tui_dispatch` covers h/l and arrows/Tab focus, visual-row j/k bounds, Home/End, Escape layering, literal editor input and modal isolation. Top bar and pane headers show focus; contact-list viewport follows selection. `test_tui_settings_ui` checks narrow rendering and wrapped history. Full-message reader beyond the existing preview cap remains separate |
| Command popup and framed editor | IMPLEMENTED | Bounded app-only `:` commands, previous-command recall, Normal/Insert/Command labels, wider framed composer with byte count, wrapped popup feedback and single-frame modal rendering. `test_tui_dispatch` checks command isolation, unknown commands, quit, offline announce, draft preservation and Ctrl-U/D; `test_tui_settings_ui` checks 38x10 popup cursor and textbox bounds. Full Vim motions, completion, safe bracketed paste and redesign of remaining dialogs are WIP |
| Conversation list and message composer | WIP | Notifications and complete attachment workflows |
| Wrapped conversation text and composing | IMPLEMENTED | Word-wrapped cached bodies/metadata, preserved LF, visual-row scrolling, a cursor-following multirow composer (Ctrl-N newline, Enter send, Up/Down movement), terminal-cell-aware clipping and connection/node/unread/traffic bar. `test_tui_text`, `test_tui_editor`, `test_tui_settings_ui` cover long lines, wide/combining text, vertical motion, narrow cursor position and scrolling. Existing 4096-byte preview limit and rich browser/RRC/dialog wrapping remain WIP |
| Per-conversation drafts | IMPLEMENTED | Draft management controls |
| Search, trust, pin, note and block state | IMPLEMENTED | Complete contact management UI |
| Reply and reaction composition | IMPLEMENTED | `test_tui_settings_ui` checks reply targeting follows the last visible message after wrapped-line scrolling; complete per-message navigation remains |
| Delivery state, progress, retry and cancellation | WIP | Complete end-user recovery workflows |
| Network discovery and active-node list | WIP | Complete diagnostics and action availability |
| Persistent node registry | IMPLEMENTED | Long-running expiry and migration coverage |
| Native Micron browser | WIP | Complete remote forms, files and caching behavior |
| Opt-in browser link identification | IMPLEMENTED | Pinned UDP/TCP tests check anonymous denial and identified access to `.allowed` pages; Browser `i` asks confirmation, scope is one node/session and cross-node navigation resets anonymity; headless consent/scope tests |
| Browser LXMF conversation handoff | IMPLEMENTED | Headless keyboard tests cover upstream `lxmf@`/`lxmf.delivery@` and explicit `lxmf:`/`lxmf://` address forms, malformed input, preserved trust/block/notes/drafts, and no automatic send |
| Retained browser page after load failure | IMPLEMENTED | `test_browser_retained` covers loading, malformed UTF-8, cancellation, timeout and subsequent success; page URL remains tied to its successful document |
| TUI retained-page link origin | IMPLEMENTED | `test_tui_state` resolves relative and root links against the displayed page rather than a failed destination; error screen and headless output expose retained URL |
| Browser same-node link reuse | IMPLEMENTED | Python UDP/TCP page tests require two pages over one authenticated link; destination or public identity change opens a fresh link |
| Native browser session caching | IMPLEMENTED | Eight entries and 1 MiB aggregate raw-response budget; destination/path/public-key scoped, anonymous and identified instances isolated. Default 12-hour lifetime and bounded `#!c=` parsing; zero/malformed directives disable caching. Forms bypass cache, Reload clears it, failures retain the displayed page. `test_browser_retained` covers hits, expiry, zero/overflow TTL, reload, forms, clock reversal and eviction. Pinned `interop_browser_udp/tcp` check packet/resource-page cache revisits without extra host requests. Disk persistence and full upstream cache-directive acceptance remain WIP |
| Bounded browser route discovery | IMPLEMENTED | `test_browser_discovery` covers timeout, retry, cancellation and clock failure; upstream adaptive first-hop allowance remains |
| Static hosted Nomad pages | IMPLEMENTED | Application controls and authentication UI |
| Executable hosted pages and forms | WIP | Complete execution policy and compatibility |
| Settings and self-announcement | WIP | Complete runtime apply and scheduling behavior |
| Interfaces screen | IMPLEMENTED | More detailed runtime controls |
| Configuration screen | IMPLEMENTED | Complete safe live reconfiguration |
| Disabled TCP/UDP endpoint configuration round trips | IMPLEMENTED | `test_config` covers absent and partial TCP client/server and UDP endpoints through parse/emit/reparse, preserving disabled state and specified ports; explicit zero ports remain rejected and bounded emission failure is tested. Incomplete serial/radio settings are outside this row |
| Upstream interface enable-key compatibility | IMPLEMENTED | `test_config` covers `enabled` and legacy `interface_enabled`, either-true precedence in both orders, per-interface isolation, invalid boolean rejection and emitted-config round trips; semantics checked against pinned Reticulum interface creation |
| Guide screen | IMPLEMENTED | Expanded contextual help |
| Event log screen | IMPLEMENTED | Filtering and persistent diagnostics |
| Channels and RRC | WIP | Complete rooms, moderation and reconnect workflows |
| Directory screen | PLANNED | Match upstream 1.2.0 placeholder; contact-directory behavior is covered by Network/contact workflows |
| Map screen | PLANNED | Match upstream 1.2.0 placeholder; a full mapping system is outside this parity baseline |
| Hosted propagation-node controls | PLANNED | Full implementation |
| Hosted Node TUI static-service controls | IMPLEMENTED | `test_tui_settings_ui` covers explicit enable/stop, root/page/access persistence, startup recovery, announce scheduling/offline retry, rollback on invalid publication/save failure and 38x10 curses rendering. Long-value editor viewport has UTF-8 boundary tests. File hosting, OS executable pages, propagation hosting and integrated stock TUI acceptance remain separate |
| Terminal QR address display | IMPLEMENTED | Bounded public-address QR, four-module quiet zone and half-block rendering; `test_tui_qr` checks bounds/rows/layout, headless dispatcher checks modal keys, `tools/test_tui_qr_decode.py` independently decodes synthetic output with pinned zxing-cpp; physical phone scan and paper-message QR remain |
| RRC persistent room history | PLANNED | Bounded persistence, restart recovery and history UI tests |
| RRC Resource envelopes | PLANNED | Oversized channel envelope exchange and upstream acceptance |

## Desktop acceptance coverage

The opt-in CTest `interop` label runs existing Python drivers using the exact
revisions documented in README. Each driver validates its source provenance.
Successful fixture tests alone do not verify an entire screen or subsystem.

| Acceptance behavior | Status | Evidence and remaining work |
| --- | --- | --- |
| Repeatable pinned Python acceptance entry point | IMPLEMENTED | Twelve opt-in CTest cases, including the explicitly delayed-worker regression; explicit checkout paths, serial execution and bounded timeouts |
| Direct packet and multi-segment LXMF, UDP/TCP | VERIFIED | `interop_lxmf_direct_udp` and `interop_lxmf_direct_tcp` pass bidirectionally with pinned Python; loss/restart scenarios remain outside this evidence |
| Bidirectional local-service path discovery, UDP/TCP | VERIFIED | `interop_discovery_udp/tcp` prove stock Python requests recover the C identity/path after losing its startup announce, and C requests discover a never-announced Python destination; public keys match in both directions. Multi-hop cached-route responses and adversarial discovery remain outside this evidence |
| Bidirectional ticket exchange and stamped resource replies, UDP/TCP | VERIFIED | Direct drivers exchange signed ticket fields, then validate ticket-backed stamps for packet and multi-segment replies against pinned Python; renewal, expiry-boundary and restart scenarios remain outside this evidence |
| Ticket acceptance harness delivery-worker ordering | IMPLEMENTED | Direct Python replies wait for the validated-delivery callback and stored ticket, not the earlier packet proof. `interop_lxmf_ticket_worker_race_udp` delays the synthetic worker by 500 ms and retains strict ticket-stamp assertions; driver diagnostics expose lengths/booleans only, never ticket bytes or message contents |
| Propagation upload/download, UDP/TCP | WIP | `interop_lxmf_propagation_udp` and `interop_lxmf_propagation_tcp`; recovery scenarios remain |
| RRC pinned-codec fixture hub | WIP | `interop_rrc_python`; stock server acceptance remains separate |
| C browser to pinned Nomad page handler, UDP/TCP | IMPLEMENTED | `interop_browser_udp` and `interop_browser_tcp` validate small and resource-backed rendered pages; integrated stock TUI acceptance remains |
| C browser executable-page form submissions, UDP/TCP | IMPLEMENTED | `interop_browser_udp/tcp` send two distinct MessagePack forms to pinned unmodified `Node.serve_page`; field/variable values and ignored keys checked; OS executable sandboxing remains separate |
| Python client to C hosted page service, UDP/TCP | IMPLEMENTED | `interop_hosted_udp/tcp` check exact small/resource responses, two in-process executable-provider form results and anonymous denial/identified allowlist access; paired reverse browser tests cover protocol direction, not stock Nomad TUI rendering or OS executable sandboxing |
| Bidirectional hosted page/request bytes, UDP/TCP | VERIFIED | Paired `interop_browser_udp/tcp` and `interop_hosted_udp/tcp` pass with pinned upstream for small/resource pages, form values, anonymous denial and identified access; evidence excludes complete TUI/hosting controls and OS execution policy |
| Stock Nomad file acceptance | PLANNED | Add bidirectional client/host live drivers |
| Complete keyboard workflow acceptance | WIP | Existing headless tests cover subsets; every screen/action needs mapped cases |
| Remote hosted file transfer | PLANNED | Hosted files currently support local reads only; add upstream file metadata/resource exchange |
| Large browser form requests | PLANNED | Browser currently caps encoded forms at 403 bytes; add resource-backed request acceptance |

## Tools and portability

| Feature | Status | Remaining work |
| --- | --- | --- |
| Identity utility | IMPLEMENTED | Additional output formats |
| Reticulum daemon | WIP | Complete transport-node behavior |
| Node, messaging and diagnostics subcommands | WIP | Complete operational-tool coverage |
| Headless TUI state tests | IMPLEMENTED | More layouts and terminal sizes |
| Parser fuzz targets | WIP | Coverage for every untrusted parser |
| Provider API boundaries for embedded ports | IMPLEMENTED | ESP-IDF platform, NVS and crypto adapters exist; target CI compilation and a boot-time KAT are configured, but the crypto host test still uses an OpenSSL symmetric shim and no target/device result is recorded yet |
| Runtime attachment of caller-polled interface providers | IMPLEMENTED | `test_runtime_provider` covers start rollback, cross-runtime double attachment, copied names/kinds, stable configured/provider slots, capacity, stop/destroy ownership, malformed ingress isolation, callback budgets, fair polling, offline recovery, MTU/outbound/broadcast enforcement, signed-announce path learning and exact-interface routing. SX1262 tests retain queued RX/completions during zero-budget scheduling maintenance. This attaches providers alongside existing POSIX runtime storage; full embedded runtime extraction and physical radio exchanges remain WIP |
| Radio completion priority during runtime polling | IMPLEMENTED | `test_sx1262_interface` covers queued TX completion surviving zero-budget maintenance and a delayed poll past its software deadline, ahead of RX/stale events; continuous RX without completion still times out with bounded work. PHY providers must prioritize the active CAD/TX completion; the Heltec adapter already does so. Physical IRQ timing remains unverified |
| Heltec WiFi LoRa 32 V3.1 ESP-IDF scaffold and BSP descriptor | WIP | ESP-IDF build, hardware drivers and physical-board validation |
| Heltec crypto boot stack protection | IMPLEMENTED | ESP-IDF 5.5.4 build resolves an 8192-byte main stack with canary/watchpoint protection and unchanged AES/SHA acceleration. Configuration tests reject undersized/missing stacks and disabled protections. Ten physical reset boots, a 60-second stability observation and a cold power-cycle pass crypto self-test, heap integrity and storage initialization, with at least 3336 bytes measured stack headroom. Startup evidence only, not radio or LXMF interoperability |
| Heltec OLED and receive-only hardware diagnostics | IMPLEMENTED | ESP-IDF I2C adapter and application loop connect the OLED core and SX1262 owner task. Tests cover large diagnostic glyph bounds, display/radio failure isolation, bounded receive draining and no linked send API. The receive-only application overrides the library preset to 868.100 MHz, SF11, 250 kHz and CR4/5, with regression assertions for the exact radio profile. ESP-IDF 5.5.4 builds, physical OLED/RX initialization and large-text visual acceptance pass with stable heap and zero transmitted frames. Raw-frame exchange and protocol dispatch remain separate gates |
| SX1262 commanded-mode startup validation | IMPLEMENTED | Physical V3.1 reports command status 1 in confirmed XOSC standby. Accept only command codes 1/2/6 with the requested XOSC or RX mode, retaining device-error and sync-register readback checks. Unit tests exhaustively cover all 8x8 mode/status pairs and failed readback. On-board initialization reaches confirmed RX; over-air reception remains unverified |
| Pinned Semtech SX126x command driver dependency | IMPLEMENTED | Exact v2.5.0 sources, license, provenance and checksums are recorded; host command/HAL-boundary test passes, but the Heltec backend and hardware remain unverified |
| Heltec V3.1 SX1262 low-level radio interface | IMPLEMENTED | Host simulations cover SPI command/read alignment, BUSY timing, modem profiles, calibration, health checks, IRQ/RX/TX/CAD state, bounded queues, recovery and statistics. Every accepted TX reserves a bounded FIFO terminal result; local tests cover success, start/IRQ failure, timeout, stop, result backpressure, recovery backoff and no implicit retry. CAD reports bounded clear/busy/failure results, and per-frame timeout uses a locally tested Semtech-matched airtime calculation plus a configured margin. ESP-IDF target CI, packet scheduler integration and physical V3.1 RF validation remain required before `VERIFIED` |
| Heltec V3.1 scheduler-to-SX1262 PHY binding | IMPLEMENTED | A dedicated ESP-IDF component now compiles the portable interface, RNode split framing and atomic packet scheduler, while a board adapter translates the full explicit-header modem profile and correlates scheduler tokens with bounded low-level CAD/TX results. Host fake-radio tests cover one- and two-frame sends, lower-ID rollover, stale results, CAD clear/busy/failure, TX success/failure, timeout cancellation with in-place reset/reconfigure/RX recovery, repeated start/stop, RX delivery and teardown failure. ESP-IDF CI and physical RNode exchange remain required before `VERIFIED` |
| Bounded RNode split-packet framing codec | IMPLEMENTED | Local boundary, malformed-fragment, duplicate, reorder, timeout, collision, sequence-reuse and callback-failure tests pass; stock RNode interoperability is not yet verified |
| Atomic SX1262 Reticulum packet scheduler | IMPLEMENTED | Portable fake-PHY tests cover exact SF5/SF6/SF8/SF12 airtime vectors, one/two-frame atomic sends, 61-minute conservative 1% duty accounting, CAD/DIFS/random contention, correlated PHY events, terminal frame failures, guarded split sequences, bounded queues, malformed RX/reassembly, callback enqueue reentrancy and the generic interface adapter. ESP-IDF PHY binding and physical RNode interoperability remain required before `VERIFIED` |
| Heltec V3.1 OLED status/preview and bounded shell cores | IMPLEMENTED | Host simulations cover Vext/reset/I2C sequencing, display failure isolation, independent preview disable-and-clear, explicit zero-timeout behavior, malformed UTF-8, line/token/registry bounds and SHA-256-bound guarded invocations with mandatory secure randomness; ESP-IDF I2C/UART0 adapters, `app_main` integration and physical hardware remain WIP |
| Heltec receive-only verified announce discovery | IMPLEMENTED | `test_heltec_discovery` exercises signed packet and split-frame reception, signature rejection, duplicate suppression, older timestamps, expiry and incomplete/reordered fragments. Application calls existing library framing/packet/announce APIs; 32 bounded destination records and privacy-safe counters are separate from routing. OLED shows unique verified identity count (IDS), distinct from service/destination and physical-device counts; same-identity multi-service and expiry tests pass. Strict and ASan/UBSan suites pass 115 tests; ESP-IDF 5.5.4 builds. Physical incoming announces pass signature verification with stable heap and over 2 KiB measured stack headroom; this is one-way evidence only, not full interoperability. IFAC, persistent peer identities, full routing and messaging dispatch remain unsupported in this firmware path |
| Heltec V3.1 SX1262, OLED and UART shell firmware integration | WIP | Receive-only verified discovery and OLED diagnostics are connected; attach the full Reticulum runtime and UART shell application handlers, then validate on physical hardware |
| Bounded packet-mode LXMF endpoint | IMPLEMENTED | `test_lxmf_packet_node` covers persisted identity/ratchet reuse, failed writes, corrupt version rejection, increasing announce timebase across restart, signed ratchet announces, unknown-sender rejection, ratchet-encrypted short reception, duplicate suppression/re-proofs, tamper rejection and explicit proof verification. No sockets, curses or ESP dependencies in endpoint. Unknown senders must announce first; replay cache and accepted messages are volatile, no direct links/resources, no ratchet rotation/stamp enforcement or durable inbox |
| Heltec scheduled TX and PRG announce integration | WIP | Firmware uses existing 1% airtime/CAD scheduler for announcements and proofs, GPIO0 PRG debounced release with 60-second cooldown, no automatic startup announce, versioned NVS identity and temporary OLED preview. `test_announce_button` covers boot-held/release/bounce/hold/cooldown; strict and sanitizer suites pass 117 tests and ESP-IDF 5.5.4 builds. Physical PRG release queues an announce and the radio reports successful packet TX; identity reopen and stable RX startup pass. Peer announce visibility, short inbound LXMF and returned proof remain acceptance gates. Clock starts at build time and persists monotonic announce floor; airtime history and message replay state do not survive reboot |
| Packet LXMF ingress diagnostics | IMPLEMENTED | `test_lxmf_packet_node` distinguishes malformed/IFAC inputs, packet types, learned announces, other destinations, local data, unsupported link requests/layouts and local non-data packets. Only aggregate counters are logged. Physical capture shows one accepted short message and transmitted proofs; peer acknowledgement is not yet confirmed. Message handling reduced measured stack headroom below the 2 KiB target, requiring additional margin before release |
| Heltec packet-mode stack margin | IMPLEMENTED | Main task default and resolved-config guard now require 12288 bytes, retaining canary/watchpoint checks. Configuration tests reject the former 8192-byte profile. Strict/sanitizer suites pass 117 tests and ESP-IDF 5.5.4 builds; post-message physical headroom must be remeasured against the 2 KiB gate |
| Large paginated OLED message previews | IMPLEMENTED | Message screen uses 10x14-pixel glyphs on a four-row/ten-column grid, with UTF-8-safe unsupported-glyph substitution and automatic 40-character page cycling every four seconds. Firmware preview duration is 30 seconds. Grid-boundary/page-transition tests and strict/sanitizer suites pass; physical readability acceptance remains pending |
| Packet-mode deferred sender verification | IMPLEMENTED | Up to four encrypted packets await a sender identity for five minutes; receive processing expires ineligible entries. Any destination-verified signed announce can supply the same identity's delivery key. Tests cover non-delivery announce revalidation, no early callback/proof, bounded capacity and expiry. Strict/sanitizer suites pass 118 tests; device revalidation remains a separate gate |
| Heltec single-button menu and last-message recall | IMPLEMENTED | PRG taps open/cycle Status, Last message, Announce and Clear message in a visible highlighted list; a 700 ms stable hold selects before release, without repeating. Navigation cannot announce, cooldown remains 60 seconds, boot-held input is ignored. Last verified preview is retained in bounded application RAM and can be reopened or cleared. Compact 5x7 message glyphs fit 21 columns by 8 rows; lowercase renders using uppercase glyphs and unsupported characters have visible placeholders. Menu and renderer regression tests cover held selection, release suppression, lowercase equivalence and compact wrapping. Physical readability remains an acceptance gate |
| Heltec live inbox and node browsing | IMPLEMENTED | Application-owned eight-entry volatile inbox retains up to 95 display characters per verified message, newest-first with tap navigation and hold-to-menu. New arrivals update the live view; older selection stays anchored until eviction. Live node list groups verified announces by identity, displays hash suffix and announce age, pages five rows and retains identity selection through updates/expiry. Pure `test_live_view`, button navigation and OLED tests cover empty/overflow/truncation/selection/identity grouping. This is bounded RAM preview history, not durable or full-message storage; node presence reflects verified announces, not a liveness probe. Physical readability and fresh radio exchange remain acceptance gates |
| Complete Nomad Network behavioral parity | PLANNED | All remaining WIP items and compatibility gates |
| Complete Reticulum daemon and utility parity | PLANNED | Full implementation |
| Large-message integration watchdog | IMPLEMENTED | Multi-segment direct-delivery test permits up to 60 seconds on instrumented runners rather than ten. Monotonic progress, partial progress, receipt-confirmed delivery and exact received content assertions remain required; protocol deadlines are unchanged. This is test infrastructure, not new interoperability evidence |
