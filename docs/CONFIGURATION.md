# Configuration subset

`rns_config_parse()` reads the core Reticulum ConfigObj-style layout without
dynamic allocation. Input lines, strings, and interface count are bounded by
the constants in `reticulum/config.h`. The supported layout is:

```ini
[reticulum]
  enable_transport = No
  share_instance = Yes
  shared_instance_type = tcp
  instance_name = default
  panic_on_interface_error = No
  shared_instance_port = 37428
  instance_control_port = 37429

[interfaces]
  [[Example]]
    type = TCPClientInterface
    enabled = Yes
    target_host = 127.0.0.1
    target_port = 4242
```

Supported interface types are `TCPClientInterface`, `TCPServerInterface`,
`UDPInterface`, `AutoInterface`, `KISSInterface`, and `RNodeInterface`.
AutoInterface is configuration metadata only until its transport implementation
is available. TCP and UDP address/port fields and KISS/RNode serial metadata are
stored as typed values. RNode radio metadata includes frequency, bandwidth,
transmit power, spreading factor, and coding rate.

Parsed configurations that explicitly enable `share_instance` elect a local
TCP owner on `127.0.0.1:shared_instance_port`. The first runtime becomes the
shared server and opens configured system interfaces; later runtimes connect
as local clients and leave their duplicate system interfaces disabled. Local
clients reconnect without blocking the caller when the owner restarts.
`instance_data_port` remains accepted as a legacy alias for
`shared_instance_port`. The `unix` type and `instance_name` are retained by the
configuration model, but AF_UNIX transport and the separate authenticated
control/RPC socket are not implemented yet and produce an explicit unsupported
status when a local interface is created.

For end-to-end TCP examples, TUI commands, firewall notes and current runtime
limitations, see [TCP_SETUP.md](TCP_SETUP.md).

Unknown sections, keys, and interface types are rejected with a status, source
line, and human-readable diagnostic. Enabled interfaces are also checked for
their required connection fields. Disabled interfaces may intentionally retain
incomplete metadata. `rns_config_emit()` writes the accepted model back in one
canonical, deterministic form suitable for inspection or persistence.
