# Router propagation upload and synchronization

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

## Inbound synchronization

`lxmf_router_propagation_sync_start()` begins one explicit, caller-polled
list/download/ack transaction against the same selected node. The router polls
the existing propagation session from `lxmf_router_poll()` and exposes an
immutable snapshot with `lxmf_router_propagation_sync_status()`. Its state
distinguishes path discovery, link establishment, list, download, ack,
completion, remote/transport failure and cancellation. An inbound sync and an
outbound propagation upload are serialized; selecting a different node while
either operation is active returns `LXMF_ERR_PENDING`.

Downloaded bytes remain untrusted encrypted LXMF. The router verifies the
transient hash and local destination, decrypts with the local identity and its
configured private-ratchet history, then uses the same bounded signature,
stamp, source-block and durable-store path as packet and direct Resource
delivery. Only a durable insertion or an already durable message-ID duplicate
is acknowledged. Rejected signatures, stamps, blocked sources, wrong
destinations and malformed ciphertext stay on the node. `retain_on_node`
suppresses acknowledgements even after successful storage. COMPLETE means the
network transaction ended; `result` and `rejected` remain nonzero when one or
more items were deliberately retained after local validation failed.

`test_lxmf_router_propagation_sync` exercises C-to-C UDP list/download/ack,
packet and Resource responses, exact destination/transient checks, block,
stamp and signature rejection, explicit retain, cancellation, active-node
replacement protection, and durable duplicate acknowledgement after closing
and reopening the message store. This is deterministic local integration
evidence, not verification against a stock LXMF propagation node.

Remaining gaps: small uploads always use a Resource instead of Python's packet
selection; invalid-stamp signalling after Resource acceptance is not handled;
the runtime Resource implementation is limited to one 74-part segment; there
is no durable encrypted transient blob, upload resume, propagation-node
reselection during an active operation, automatic/periodic sync scheduling,
sync resume/session persistence, propagation hosting, TUI sync control or live
pinned-upstream upload/download proof. These prevent a general propagation
parity claim.
