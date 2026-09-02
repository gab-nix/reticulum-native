# Link Channel core

The bounded Channel core implements the Reticulum 1.5.2 six-byte message envelope:
network-order message type, sequence number, and payload length. Envelopes are capped
at 500 bytes, so the maximum payload is 494 bytes.

`rns_channel_t` is allocation-free and holds at most 32 transmit and 32 receive
envelopes. Configuration selects a smaller active window, retry count, timeout, an
injected monotonic clock, and callbacks. `rns_channel_send()` queues an envelope only
after the transport callback accepts it. The link/receipt integration calls
`rns_channel_mark_delivered()` when Reticulum reports delivery. `rns_channel_tick()`
performs deterministic retries and terminal timeout handling.

Receive sequence numbers use modulo-65536 half-range ordering. In-window out-of-order
messages are copied into bounded storage and delivered in order once gaps close.
Already-delivered or already-buffered envelopes are ignored and reported through the
event callback. Messages outside the configured receive window are rejected.

This module intentionally does not create Link packets, receipts, sockets, resources,
or threads. It is the protocol/state-machine core those layers can drive. The current
window controller uses conservative additive growth after one window of successful
deliveries and halves on terminal timeout; exact upstream RTT-derived tuning will be
added when receipt timing is integrated with the Link transport.
