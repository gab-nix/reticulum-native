# Propagation client service

`lxmf_propagation_session.h` exposes a native, caller-polled propagation
transport session. It is separate from the message router and has no TUI or
storage dependency. It is not yet wired to the application's Sync action.

Create a session with a borrowed runtime, local private identity, verified
propagation-node public identity and its `lxmf.propagation` destination hash.
The destination is checked against the supplied identity. Select a different
node by destroying the old session and creating another.

Call `lxmf_pn_session_sync(session, monotonic_seconds)` or
`lxmf_pn_session_upload(session, upload, monotonic_seconds)`, then continue
polling the runtime and calling `lxmf_pn_session_poll`. The session requests a
missing path, establishes a signed encrypted link and identifies the client.
Each operation has a configurable deadline. Cancel or destroy before destroying
the runtime. Callbacks are synchronous; do not re-enter or destroy the session
from a callback.

Sync lists up to 128 transient IDs, downloads in batches of eight (keeping
requests within the default encrypted packet MTU), and acknowledges only
messages for which the application callback returns true. The callback must
decrypt, authenticate and durably retain the message, or confirm a durable
duplicate, before returning true. A false return leaves the remote copy intact.
Downloaded hashes must match the requested IDs and duplicates are rejected
before callbacks run. `retain_on_node` disables acknowledgements. Acknowledged
counts advance only after the server confirms the acknowledgement request.
The application must persist its own transient-ID duplicate cache.

Uploads copy the encoded batch and send it as a proof-tracked Resource. The
application must already have encrypted each message for its recipient and
appended a valid propagation stamp. COMPLETE means the propagation node
received the transfer, **not** that a final recipient received the message.

The codec bounds are 128 items and 8 MiB. Actual transfer capacity remains
limited by the runtime's current single-segment Resource implementation.
Multi-segment transfer, resume, automatic retry, node reselection, router
integration, durable session state and propagation-node hosting remain future
work. Failed/complete sessions release links and transfers on the next session
poll; cancel and destroy release them immediately. Previously accepted messages
are not rolled back on network failure.

Evidence: `test_lxmf_propagation_session` performs C-to-C UDP exchanges with an
identified `/get` handler and resource receiver: multi-batch download,
resource-backed responses, durable-only acknowledgements, storage rejection,
duplicate IDs, mismatched message hashes, access denial, upload proof,
cancellation and deterministic deadline failure. This is not live Python
interoperability evidence.

Checkpoint validation: AppleClang warnings-as-errors build, 61/61 strict tests,
and focused ASan/UBSan 3/3 pass. macOS `leaks --atExit` reports zero leaks for
the session integration test. LeakSanitizer itself is unavailable on this
platform; GCC was not installed for an additional compiler run.
