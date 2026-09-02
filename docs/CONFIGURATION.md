# Configuration subset

`rns_config_parse()` reads the core Reticulum ConfigObj-style layout without
dynamic allocation. Input lines, strings, and interface count are bounded by
the constants in `reticulum/config.h`. The supported layout is:

```ini
[reticulum]
  enable_transport = No
  share_instance = Yes
  panic_on_interface_error = No
  instance_control_port = 37428
  instance_data_port = 37428

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

Unknown sections, keys, and interface types are rejected with a status, source
line, and human-readable diagnostic. Enabled interfaces are also checked for
their required connection fields. Disabled interfaces may intentionally retain
incomplete metadata. `rns_config_emit()` writes the accepted model back in one
canonical, deterministic form suitable for inspection or persistence.
