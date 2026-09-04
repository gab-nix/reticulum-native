# Feature status

This is the complete public feature ledger. It tracks implementation state only
and does not certify complete Reticulum, LXMF or Nomad Network parity.

- `IMPLEMENTED`: available in the current C implementation.
- `WIP`: partially implemented; important behavior remains.
- `PLANNED`: not implemented.

## Foundation and networking

| Feature | Status | Remaining work |
| --- | --- | --- |
| C17 library, HAL, buffers and typed status API | IMPLEMENTED | Additional embedded platform adapters |
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
| Directory and map workflows | PLANNED | Full implementation |
| Hosted propagation-node controls | PLANNED | Full implementation |

## Tools and portability

| Feature | Status | Remaining work |
| --- | --- | --- |
| Identity utility | IMPLEMENTED | Additional output formats |
| Reticulum daemon | WIP | Complete transport-node behavior |
| Node, messaging and diagnostics subcommands | WIP | Complete operational-tool coverage |
| Headless TUI state tests | IMPLEMENTED | More layouts and terminal sizes |
| Parser fuzz targets | WIP | Coverage for every untrusted parser |
| Embedded HAL boundary | IMPLEMENTED | Board-specific adapters and validation |
| Complete Nomad Network behavioral parity | PLANNED | All remaining WIP items and compatibility gates |
| Complete Reticulum daemon and utility parity | PLANNED | Full implementation |
