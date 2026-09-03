# Live LXMF propagation interoperability

The opt-in propagation harness connects the native caller-polled router to an
unmodified pinned Python RNS 1.5.2 / LXMF 1.1.0 propagation node over loopback
UDP or a continuous framed-TCP connection. It exercises both client directions
in one run:

- C identifies to the Python propagation destination, uploads a stamped,
  encrypted message, receives the node's completion proof, and Python verifies
  its source signature, title, unknown field and exact synthetic content.
- Python places a stamped encrypted message in its real propagation store; C
  performs `/get` list, download and acknowledgement, verifies/decrypts and
  durably inserts it, and the Python node removes it only after acknowledgement.

Build and run it explicitly:

```sh
cmake -S . -B build -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_BUILD_APPS=OFF \
  -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build --target lxmf_propagation_live_driver
python3 tools/test_lxmf_propagation_live.py \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --driver build/tests/lxmf_propagation_live_driver \
  --output /tmp/lxmf-propagation-report.json

python3 tools/test_lxmf_propagation_live.py \
  --transport tcp \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --driver build/tests/lxmf_propagation_live_driver \
  --output /tmp/lxmf-propagation-tcp-report.json
```

The script requires exact upstream commits and rejects tracked modifications.
For a source-only Reticulum tree whose Git object database was deliberately
removed after verification, it also accepts an adjacent `.provenance` marker
containing the exact commit and `tracked_status=clean`. Imported Python modules
must still resolve inside the supplied trees. All identities, stores and
configuration are generated beneath a temporary directory and removed.

The recorded acceptance uses a 257-byte C message and an incompressible
2,048-byte Python message, propagation cost 13, verified public announces and
one authenticated link. The report contains public destination/message IDs,
state/counter results and source/binary fingerprints only. It contains no
plaintext, private keys, identity files, histories or packet captures.

`tests/fixtures/lxmf_propagation_udp_live.provenance.json` and
`tests/fixtures/lxmf_propagation_tcp_live.provenance.json` record successful
bidirectional runs. These verify one loopback UDP session and one continuous
framed-TCP client connection. Automatic scheduling, TCP reconnect,
restart/resume, retries under loss, relay hops, server hosting, peering,
multi-segment Resources, TUI-driven sync and physical RNode transport remain
outside this gate.
