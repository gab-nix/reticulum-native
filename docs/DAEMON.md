# Reticulum C daemon

`rnsd` owns a caller-polled `rns_runtime_t`. The runtime combines the configuration,
transport/node ingress, and a bounded registry of interfaces. It does not create a
background thread: applications may call `rns_runtime_poll()` from their own event
loop, while the daemon sleeps briefly between idle polls.

## Running

```sh
rnsd --check ~/.reticulum/config
rnsd ~/.reticulum/config
```

`SIGINT` and `SIGTERM` only set a signal-safe stop flag. Cleanup happens in normal
control flow. `--once` starts every interface, performs one bounded poll, prints
interface startup state, and exits; it is useful for service health checks.

The runtime currently opens IPv4 UDP, TCP client, and TCP server interfaces. UDP
datagrams and HDLC-framed TCP packets enter the same node pipeline. When transport
is enabled, forwardable packets are emitted on the other active interfaces. A TCP
server presently accepts one simultaneous peer per configured interface.

AutoInterface, KISS serial, and RNode serial are represented in configuration but
are explicitly reported as `unsupported`; they are never silently treated as
active. With `panic_on_interface_error = yes`, any failed or unsupported enabled
interface prevents daemon startup. In tolerant mode, startup succeeds and callers
can inspect each interface's state, last error, and packet/byte counters.

This daemon is an early native runtime, not yet a drop-in replacement for Python
RNS. Shared-instance TCP data IPC and TCP reconnect scheduling are available,
but the daemon does not yet implement the shared control/RPC channel,
multiple accepted TCP peers, persistent transport identity/path state, or delayed
announce rebroadcast scheduling.
