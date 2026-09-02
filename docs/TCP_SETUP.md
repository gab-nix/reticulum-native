# TCP setup guide

This guide connects `reticulum-c` directly to another Reticulum TCP endpoint.
TCP uses Reticulum packet framing; it is not HTTP and does not provide TLS by
itself. Only expose a listener on networks you trust, or protect it with a VPN,
firewall or encrypted tunnel.

## Build and create an identity

```sh
cmake -S . -B build -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_BUILD_APPS=ON \
  -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/apps/nomad-chat init my.identity
./build/apps/nomad-chat address my.identity
```

Keep the identity file private and backed up.

## TCP server

Use a server when this machine has an address and port reachable by its peers.
Save this as `server.conf`:

```ini
[reticulum]
  enable_transport = No
  share_instance = No
  panic_on_interface_error = Yes
  instance_control_port = 37428
  instance_data_port = 37428

[interfaces]
  [[TCP Listener]]
    type = TCPServerInterface
    enabled = Yes
    listen_ip = 0.0.0.0
    listen_port = 4242
```

`0.0.0.0` accepts IPv4 connections on every local interface. Use `127.0.0.1`
for same-machine testing or a specific LAN address to restrict the listener.
Permit TCP port 4242 through the firewall when remote clients must connect.

```sh
./build/apps/rnsd --check server.conf
./build/apps/rnsd server.conf
```

## TCP client

On the connecting machine save this as `client.conf`, replacing the example
address with the server's reachable address:

```ini
[reticulum]
  enable_transport = No
  share_instance = No
  panic_on_interface_error = Yes
  instance_control_port = 37428
  instance_data_port = 37428

[interfaces]
  [[TCP Uplink]]
    type = TCPClientInterface
    enabled = Yes
    target_host = 192.168.1.20
    target_port = 4242
```

```sh
nc -vz 192.168.1.20 4242
./build/apps/rnsd --check client.conf
./build/apps/rnsd client.conf
```

Configure one end as the server and the other as the client. If startup reports
an interface error, verify the address, firewall and port, then restart it.

## Run the Nomad TUI

Shared-instance IPC is not implemented, so the TUI cannot reuse a separately
running C `rnsd`. Stop `rnsd` when it would conflict with the listener, then let
the TUI open the interface itself:

```sh
./build/apps/nomad-chat tui --config client.conf my.identity history.lxms
```

Use `server.conf` instead on the server side. Press `N` for Network. Verified
announces appear with destination, hop count, interface ID and reachability.
They persist beside the history as `history.lxms.nodes`.

```sh
./build/apps/nomad-chat nodes history.lxms.nodes
./build/apps/nomad-chat nodes --json history.lxms.nodes
```

## Python Reticulum peers

Configure the Python endpoint with the matching server/client role, host and
port. The C implementation accepts and verifies compatible announce packets,
but live bidirectional interoperability is not yet release-certified.

## Current limitations

- One accepted connection is managed per configured TCP server interface.
- TCP client reconnection and tunnel persistence are not implemented.
- Shared-instance IPC and daemon reuse are not implemented.
- The C client does not yet originate periodic Nomad node announces.
- Remote Nomad page requests, resources and complete multi-hop transport remain
  work in progress.
- A Network entry proves local announce validation, not full page or messaging
  interoperability. See [FEATURE_STATUS.md](FEATURE_STATUS.md).

## Troubleshooting

- `configuration valid` checks syntax, not connectivity.
- `connection refused` means no listener is reachable at the selected endpoint.
- `interface ... down` means startup failed; keep
  `panic_on_interface_error = Yes` while diagnosing.
- An empty Network screen means no valid peer announce has arrived yet.
- `cannot open node registry` means no registry has been persisted at that path.

