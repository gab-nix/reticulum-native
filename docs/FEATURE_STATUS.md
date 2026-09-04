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
| C17 library, injectable platform HAL, buffers and typed status API | IMPLEMENTED | ESP-IDF platform adapter and target validation |
| Replaceable crypto provider API | IMPLEMENTED | OpenSSL-free dispatcher link test passes; ESP-IDF backend and cross-backend vectors remain |
| Transactional storage and nonblocking interface provider APIs | IMPLEMENTED | NVS and native SX1262 providers |
| Explicit platform-separated source manifests | WIP | ESP-IDF component must consume the portable manifest after provider separation |
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
| Segmented large resources | IMPLEMENTED | Complete low-MTU and retry behavior |

## LXMF messaging

| Feature | Status | Remaining work |
| --- | --- | --- |
| Message codec, signatures and unknown fields | IMPLEMENTED | Broader compatibility coverage |
| Standard reply, reaction, thread and media fields | IMPLEMENTED | Complete user-facing workflows |
| Opportunistic packet delivery | IMPLEMENTED | Complete ratchet and retry workflows |
| Direct link packet delivery | IMPLEMENTED | Complete reconnect and relay behavior |
| Direct resource delivery | IMPLEMENTED | Persistent transfer resume |
| Delivery proofs and durable queue state | IMPLEMENTED | Complete offline retry scheduling |
| Incoming block, size and stamp policies | IMPLEMENTED | Complete policy controls |
| Ratchet history | WIP | Rotation and enforcement parity |
| Stamps and asynchronous stamp generation | WIP | Complete cancellation and peer-policy behavior |
| Tickets | WIP | Complete issuance and renewal workflows |
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
| Terminal QR address display | PLANNED | Replace placeholder with actual QR encoding and terminal rendering |
| RRC persistent room history | PLANNED | Bounded persistence, restart recovery and history UI tests |
| RRC Resource envelopes | PLANNED | Oversized channel envelope exchange and upstream acceptance |

## Desktop acceptance coverage

The opt-in CTest `interop` label runs existing Python drivers using the exact
revisions documented in README. Each driver validates its source provenance.
Successful fixture tests alone do not verify an entire screen or subsystem.

| Acceptance behavior | Status | Evidence and remaining work |
| --- | --- | --- |
| Repeatable pinned Python acceptance entry point | IMPLEMENTED | Five opt-in CTest cases; explicit checkout paths, serial execution and bounded timeouts |
| Direct packet and multi-segment LXMF, UDP/TCP | VERIFIED | `interop_lxmf_direct_udp` and `interop_lxmf_direct_tcp` pass bidirectionally with pinned Python; loss/restart scenarios remain outside this evidence |
| Propagation upload/download, UDP/TCP | WIP | `interop_lxmf_propagation_udp` and `interop_lxmf_propagation_tcp`; recovery scenarios remain |
| RRC pinned-codec fixture hub | WIP | `interop_rrc_python`; stock server acceptance remains separate |
| Stock Nomad page/form/file acceptance | PLANNED | Add bidirectional client/host live drivers |
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
| Provider API boundaries for embedded ports | IMPLEMENTED | Full portable-core allocation conversion, ESP-IDF adapters and physical-board validation remain |
| Complete Nomad Network behavioral parity | PLANNED | All remaining WIP items and compatibility gates |
| Complete Reticulum daemon and utility parity | PLANNED | Full implementation |
