# AutoInterface

The native AutoInterface is a caller-polled IPv6 link-local discovery and raw
UDP carrier. It follows the pinned Reticulum 1.5.2 discovery construction:

- discovery tokens are `SHA-256(group_id || canonical_peer_ipv6)`;
- the default temporary/link multicast group is derived from bytes 2 through
  13 of `SHA-256(group_id)`;
- discovery, data and timing defaults are 29716, 42671, 1.6-second multicast
  announcements, 5.2-second reverse announcements and 22-second peer expiry;
- carrier datagrams contain the Reticulum packet directly, without HDLC or
  KISS framing;
- packets repeated across interfaces are suppressed for 0.75 seconds in a
  bounded 48-entry window.

The protocol engine in `reticulum/auto.h` owns no sockets or threads. Callers
inject a monotonic clock and datagram callbacks, add bounded IPv6 link-local
carriers, and drive all beacon, reverse-peering and expiry work with
`rns_auto_poll()`. This seam is also how deterministic tests avoid ambient LAN
traffic.

The POSIX runtime adapter enumerates usable IPv6 link-local devices, applies
the `devices` and `ignored_devices` filters, joins the derived multicast group,
and exposes STARTING, UP or DOWN through normal runtime interface diagnostics.
An interface does not become UP until the pinned 1.92-second initial discovery
window has elapsed. A configuration example is:

```ini
[interfaces]
  [[LAN]]
    type = AutoInterface
    enabled = Yes
    group_id = reticulum
    discovery_scope = link
    multicast_address_type = temporary
    discovery_port = 29716
    data_port = 42671
    devices = en0
    ignored_devices = awdl0
```

Current boundaries are explicit: deterministic C tests cover the engine and
pinned vectors, but no stock-Python LAN exchange has been captured. Dynamic
address/interface changes, per-carrier echo diagnostics, IFAC application,
interface modes/announce rate controls, and link MTUs above the library's
current 500-byte packet ceiling remain future work.
