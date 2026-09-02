# Canonical feature ledger

This is the source of truth for implementation progress. `IMPLEMENTED` means code and automated tests exist; `VERIFIED` additionally requires bidirectional upstream evidence. `WIP` names missing behavior, `PLANNED` has no complete implementation, and `BLOCKED` records an external dependency.

| Area | Status | Evidence and remaining work |
| --- | --- | --- |
| C17 build, HAL, buffers, status API | IMPLEMENTED | Warning-as-error builds and unit tests. Embedded adapters remain planned. |
| Crypto, identities, destinations, packets | IMPLEMENTED | Known-answer and unit tests; full upstream vector matrix remains WIP. |
| Signed announces and paths | WIP | Build/parse/verification, verified runtime callbacks and path snapshots tested. Persistent discovery and upstream bidirectional capture remain. |
| UDP and TCP interfaces | IMPLEMENTED | Loopback tests, setup guide and a reachability-checked public uplink configuration. Protocol-level upstream testing remains. |
| AutoInterface, shared IPC, serial KISS, RNode | PLANNED | Framing primitives exist; interface drivers do not. |
| Transport forwarding | WIP | Basic ingress, forwarding and dedupe exist. Reverse/proof/link/tunnel tables and persistent paths remain. |
| Links and channels | WIP | Handshake crypto and bounded channel state tested. Receipts, keepalives, identification and runtime wiring remain. |
| Resources | PLANNED | Segmentation, compression, retries, resume and cancellation remain. |
| LXMF codec, signatures, stamps, tickets | WIP | Core codec and unit tests exist. Full ticket lifecycle and upstream fixtures remain. |
| LXMF delivery and propagation | WIP | Outbound router now drains queued messages through encrypted opportunistic packets and persists status transitions; direct links/resources, retries and propagation sync remain. |
| Peer and message persistence | IMPLEMENTED | Crash-recovering message journal and durable peer settings tests. |
| Nomad conversations | WIP | Search, trust tabs, address entry, known-node inbox handoff, composer, history and offline queue exist. Live receive/send, attachments, replies and reactions remain. |
| Network screen and active nodes | WIP | Configured TUI runtime consumes verified announces, associates Nomad nodes with LXMF inboxes, and renders sorted persistent records. Search, path refresh, details and upstream verification remain. |
| Native Micron browser | WIP | Bounded parser, links and history exist. Remote fetch, forms, complete markup and upstream page tests remain. |
| Channels/RRC, Interfaces, Config, Guide, Logs, hosted node | PLANNED | Shell navigation seams exist; working screens do not. |
| CLI utilities | WIP | `rnid`, `rnsd`, and `nomad-chat` history/node diagnostics exist, including JSON node output. Core utility parity remains. |
| Embedded portability | PLANNED | HAL boundary exists; cross-build examples and non-OpenSSL provider remain. |
| Upstream interoperability | BLOCKED | No complete bidirectional acceptance matrix or captured live upstream traffic yet. |

The compatibility revisions and update policy are recorded in `COMPATIBILITY.md`.
