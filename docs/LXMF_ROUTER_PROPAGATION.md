# Router propagation upload

The LXMF router can optionally send messages through one caller-selected,
verified `lxmf.propagation` node. This is a library feature; no curses or
application configuration is performed here.

Set `propagation_node_identity`, its matching
`propagation_node_destination`, and the node's verified advertised
`propagation_stamp_cost` in `lxmf_router_config_t`. A runtime and wall clock
are required. The router copies only the node's public identity and rejects a
hash that does not derive from it. The default retry policy is five attempts
starting at ten seconds; bounded overrides are available. A message selects
the route by persisting `LXMF_DELIVERY_METHOD_PROPAGATED` as its desired method.

The non-blocking flow is:

1. Resolve the recipient's verified delivery identity and add its advertised
   delivery stamp/ticket when required.
2. Preserve the destination prefix and encrypt the remaining signed LXMF
   representation for the recipient, using its verified ratchet when present.
3. Hash that destination-plus-ciphertext representation to obtain the
   transient ID. Build the mandatory propagation proof-of-work stamp using
   pinned LXMF's 1,000-round expansion.
4. Submit one upstream-shaped upload to the existing caller-polled propagation
   session, which discovers a path, establishes an authenticated link,
   identifies the sender and transfers a proof-tracked Resource.

While prerequisites, stamping and link setup are in progress the message stays
QUEUED with a specific reason. Resource transfer becomes SENDING and exposes
integer progress. A valid Resource completion proof from the propagation node
becomes SENT, never DELIVERED: final-recipient delivery is not proven. Failures
are requeued with bounded exponential delay. Exhausting the configured attempt
limit is persisted as `retry exhausted` and is not automatically polled again.
Cancellation is durable and destroys the active stamp/session immediately.
Interrupted SENDING and process-local retry deadlines safely return to the
queue during router initialisation.

The message store retains desired/actual delivery method, attempt count,
progress, queue/failure reason and state. It does not yet retain the encrypted
transient representation. Therefore a restart after an upload whose proof was
lost can re-encrypt and submit a different transient ID; receiver replay
protection remains necessary. Only one propagation upload runs at once.

`test_lxmf_router_propagation` runs a C-to-C UDP propagation service and checks
recipient encryption/signature, pinned 1,000-round PN stamp validation,
authenticated identified Resource upload, SENT-not-DELIVERED completion,
progress metadata, cancellation, missing-node queueing, bounded timeout/retry
exhaustion and restart recovery. `test_lxmf_stamp_job` includes a deterministic
Python-derived expanded-stamp vector. This is IMPLEMENTED evidence, not a live
Python propagation-node exchange.

Remaining gaps: small uploads always use a Resource instead of Python's packet
selection; invalid-stamp signalling after Resource acceptance is not handled;
the runtime Resource implementation is limited to one 74-part segment; there
is no durable encrypted transient blob, upload resume, propagation-node
reselection, inbound sync integration, propagation hosting, TUI binding or
live pinned-upstream upload proof. These prevent a general propagation parity
claim.
