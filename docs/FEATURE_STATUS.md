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
| Signed announces and path lookup | WIP | Complete persistence and routing compatibility |
| UDP interfaces | IMPLEMENTED | Adverse-network hardening |
| TCP client and server interfaces | IMPLEMENTED | Broader reconnect and topology coverage |
| AutoInterface | IMPLEMENTED | Broader platform compatibility |
| Shared-instance local IPC | IMPLEMENTED | Complete shared-instance behavior |
| KISS serial | IMPLEMENTED | Hardware interoperability coverage; nonblocking backpressure CI coverage exists |
| RNode serial | IMPLEMENTED | Hardware interoperability coverage; chunked handshake and asynchronous old-firmware rejection tests exist |
| IFAC processing | IMPLEMENTED | Broader interface integration |
| Transport forwarding and reverse paths | WIP | Complete multi-hop transport behavior |
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

| Feature | Status | Remaining work |
| --- | --- | --- |
| Message codec, signatures and unknown fields | IMPLEMENTED | Broader compatibility coverage |
| Standard reply, reaction, thread and media fields | IMPLEMENTED | Complete user-facing workflows |
| Opportunistic packet delivery | IMPLEMENTED | Complete ratchet and retry workflows |
| Direct link packet delivery | IMPLEMENTED | Complete reconnect and relay behavior; integration test IDs are distinct from random wire hashes |
| Direct resource delivery | IMPLEMENTED | Persistent transfer resume |
| Delivery proofs and durable queue state | IMPLEMENTED | Complete offline retry scheduling |
| Incoming block, size and stamp policies | IMPLEMENTED | Complete policy controls |
| Ratchet history | WIP | Rotation and enforcement parity |
| Stamps and asynchronous stamp generation | WIP | Complete cancellation and peer-policy behavior |
| Tickets | WIP | Complete issuance and renewal workflows |
| Trusted-contact outgoing tickets | IMPLEMENTED | `test_tui_fields_ui` checks issue/reuse before signing, no ticket for unknown/blocked recipients, and delivered-ticket throttling; full TUI-to-upstream acceptance remains |
| Ticket delivery throttling | IMPLEMENTED | Packet/link/resource proof completion records included locally issued tickets; `test_lxmf_router_receipt` checks no throttling at socket-send, proof-only throttling, restart and one-day expiry; resource-ticket persistence failure coverage remains |
| Python fractional ticket expiry | IMPLEMENTED | `test_lxmf_tickets` accepts float32/64 fractions and rejects NaN/infinity/negative/overflow; integer-second storage rounds expiry down by less than one second, never extending validity |
| Ticket fields composition | IMPLEMENTED | `test_lxmf_fields` covers add/replace/remove, opaque preservation, 64-bit expiry, output object-budget roundtrips, bounds and malformed input; issuance/delivery scheduling remains |
| Propagation upload | WIP | Automatic scheduling and durable resume |
| Propagation download and acknowledgement | WIP | Automatic synchronisation and recovery |
| Paper messages and `lxm://` URIs | IMPLEMENTED | QR rendering and scanning |
| Peer and message persistence | IMPLEMENTED | Store migrations and maintenance controls |

## Nomad client

| Feature | Status | Remaining work |
| --- | --- | --- |
| Terminal application shell and screen dispatcher | IMPLEMENTED | Additional accessibility controls |
| Conversation list and message composer | WIP | Notifications and complete attachment workflows |
| Per-conversation drafts | IMPLEMENTED | Draft management controls |
| Search, trust, pin, note and block state | IMPLEMENTED | Complete contact management UI |
| Reply and reaction composition | IMPLEMENTED | Complete per-message navigation |
| Delivery state, progress, retry and cancellation | WIP | Complete end-user recovery workflows |
| Network discovery and active-node list | WIP | Complete diagnostics and action availability |
| Persistent node registry | IMPLEMENTED | Long-running expiry and migration coverage |
| Native Micron browser | WIP | Complete remote forms, files and caching behavior |
| Opt-in browser link identification | IMPLEMENTED | Pinned UDP/TCP tests check anonymous denial and identified access to `.allowed` pages; Browser `i` asks confirmation, scope is one node/session and cross-node navigation resets anonymity; headless consent/scope tests |
| Browser LXMF conversation handoff | IMPLEMENTED | Headless keyboard tests cover upstream `lxmf@`/`lxmf.delivery@` and explicit `lxmf:`/`lxmf://` address forms, malformed input, preserved trust/block/notes/drafts, and no automatic send |
| Retained browser page after load failure | IMPLEMENTED | `test_browser_retained` covers loading, malformed UTF-8, cancellation, timeout and subsequent success; page URL remains tied to its successful document |
| TUI retained-page link origin | IMPLEMENTED | `test_tui_state` resolves relative and root links against the displayed page rather than a failed destination; error screen and headless output expose retained URL |
| Browser same-node link reuse | IMPLEMENTED | Python UDP/TCP page tests require two pages over one authenticated link; destination or public identity change opens a fresh link |
| Bounded browser route discovery | IMPLEMENTED | `test_browser_discovery` covers timeout, retry, cancellation and clock failure; upstream adaptive first-hop allowance remains |
| Static hosted Nomad pages | IMPLEMENTED | Application controls and authentication UI |
| Executable hosted pages and forms | WIP | Complete execution policy and compatibility |
| Settings and self-announcement | WIP | Complete runtime apply and scheduling behavior |
| Interfaces screen | IMPLEMENTED | More detailed runtime controls |
| Configuration screen | IMPLEMENTED | Complete safe live reconfiguration |
| Guide screen | IMPLEMENTED | Expanded contextual help |
| Event log screen | IMPLEMENTED | Filtering and persistent diagnostics |
| Channels and RRC | WIP | Complete rooms, moderation and reconnect workflows |
| Directory screen | PLANNED | Match upstream 1.2.0 placeholder; contact-directory behavior is covered by Network/contact workflows |
| Map screen | PLANNED | Match upstream 1.2.0 placeholder; a full mapping system is outside this parity baseline |
| Hosted propagation-node controls | PLANNED | Full implementation |
| Hosted Node TUI screen | PLANNED | Replace unavailable action with hosting controls and headless acceptance tests |
| Terminal QR address display | IMPLEMENTED | Bounded public-address QR, four-module quiet zone and half-block rendering; `test_tui_qr` checks bounds/rows/layout, headless dispatcher checks modal keys, `tools/test_tui_qr_decode.py` independently decodes synthetic output with pinned zxing-cpp; physical phone scan and paper-message QR remain |
| RRC persistent room history | PLANNED | Bounded persistence, restart recovery and history UI tests |
| RRC Resource envelopes | PLANNED | Oversized channel envelope exchange and upstream acceptance |

## Desktop acceptance coverage

The opt-in CTest `interop` label runs existing Python drivers using the exact
revisions documented in README. Each driver validates its source provenance.
Successful fixture tests alone do not verify an entire screen or subsystem.

| Acceptance behavior | Status | Evidence and remaining work |
| --- | --- | --- |
| Repeatable pinned Python acceptance entry point | IMPLEMENTED | Nine opt-in CTest cases; explicit checkout paths, serial execution and bounded timeouts |
| Direct packet and multi-segment LXMF, UDP/TCP | VERIFIED | `interop_lxmf_direct_udp` and `interop_lxmf_direct_tcp` pass bidirectionally with pinned Python; loss/restart scenarios remain outside this evidence |
| Bidirectional ticket exchange and stamped resource replies, UDP/TCP | VERIFIED | Direct drivers exchange signed ticket fields, then validate ticket-backed stamps for packet and multi-segment replies against pinned Python; renewal, expiry-boundary and restart scenarios remain outside this evidence |
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
| Heltec WiFi LoRa 32 V3.1 ESP-IDF scaffold and BSP descriptor | WIP | ESP-IDF build, hardware drivers and physical-board validation |
| Pinned Semtech SX126x command driver dependency | IMPLEMENTED | Exact v2.5.0 sources, license, provenance and checksums are recorded; host command/HAL-boundary test passes, but the Heltec backend and hardware remain unverified |
| Heltec V3.1 SX1262 low-level radio interface | IMPLEMENTED | Host simulations cover SPI command/read alignment, BUSY timing, modem profiles, calibration, health checks, IRQ/RX/TX state, bounded queues, recovery and statistics; ESP-IDF target CI and physical V3.1 RF validation remain required before `VERIFIED` |
| Bounded RNode split-packet framing codec | IMPLEMENTED | Local boundary, malformed-fragment, duplicate, reorder, timeout, collision, sequence-reuse and callback-failure tests pass; stock RNode interoperability is not yet verified |
| Heltec V3.1 SX1262, OLED and UART shell firmware | WIP | Integrate the simulated low-level radio with Reticulum framing/runtime, OLED and shell; validate the ESP-IDF build and physical hardware |
| Complete Nomad Network behavioral parity | PLANNED | All remaining WIP items and compatibility gates |
| Complete Reticulum daemon and utility parity | PLANNED | Full implementation |
