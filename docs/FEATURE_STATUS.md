# Canonical feature ledger

This is the source of truth for implementation progress. `IMPLEMENTED` means code and automated tests exist; `VERIFIED` additionally requires bidirectional upstream evidence. `WIP` names missing behavior, `PLANNED` has no complete implementation, and `BLOCKED` records an external dependency.

| Area | Status | Evidence and remaining work |
| --- | --- | --- |
| C17 build, HAL, buffers, status API | IMPLEMENTED | Warning-as-error builds and unit tests. Embedded adapters remain planned. |
| Crypto, identities, destinations, packets | IMPLEMENTED | Known-answer and unit tests; full upstream vector matrix remains WIP. |
| Signed announces and paths | WIP | Build/parse/verification and path tables tested. Live runtime callbacks and upstream bidirectional capture remain. |
| UDP and TCP interfaces | IMPLEMENTED | Loopback integration tests. Live upstream testing remains. |
| AutoInterface, shared IPC, serial KISS, RNode | PLANNED | Framing primitives exist; interface drivers do not. |
| Transport forwarding | WIP | Basic ingress, forwarding and dedupe exist. Reverse/proof/link/tunnel tables and persistent paths remain. |
| Links and channels | WIP | Handshake crypto and bounded channel state tested. Receipts, keepalives, identification and runtime wiring remain. |
| Resources | PLANNED | Segmentation, compression, retries, resume and cancellation remain. |
| LXMF codec, signatures, stamps, tickets | WIP | Core codec and unit tests exist. Full ticket lifecycle and upstream fixtures remain. |
| LXMF delivery and propagation | WIP | Opportunistic UDP exchange works C-to-C. Direct links/resources, retry queues and propagation sync remain. |
| Peer and message persistence | IMPLEMENTED | Crash-recovering message journal and durable peer settings tests. |
| Nomad conversations | WIP | Search, trust tabs, composer, history and offline queue exist. Live receive/send, attachments, replies and reactions remain. |
| Network screen and active nodes | WIP | Registry and static rendering exist. Live verified announce ingestion remains. |
| Native Micron browser | WIP | Bounded parser, links and history exist. Remote fetch, forms, complete markup and upstream page tests remain. |
| Channels/RRC, Interfaces, Config, Guide, Logs, hosted node | PLANNED | Shell navigation seams exist; working screens do not. |
| CLI utilities | WIP | `rnid`, `rnsd`, and initial `nomad-chat` commands exist. Core utility parity and JSON diagnostics remain. |
| Embedded portability | PLANNED | HAL boundary exists; cross-build examples and non-OpenSSL provider remain. |
| Upstream interoperability | BLOCKED | No complete bidirectional acceptance matrix or captured live upstream traffic yet. |

The compatibility revisions and update policy are recorded in `COMPATIBILITY.md`.

