# Live direct LXMF interoperability

The opt-in harness exchanges real encrypted Reticulum traffic between the C
router and the unmodified pinned Python RNS 1.5.2 / LXMF 1.1.0 stack over UDP
or framed TCP loopback. It does not mock either protocol endpoint and is not a
C-to-C test. UDP remains the default; pass `--transport tcp` to start a pinned
Python `TCPServerInterface` and connect the native TCP client.

```sh
cmake -S . -B build -G Ninja -DRETICULUM_BUILD_TESTS=ON -DRETICULUM_BUILD_APPS=ON -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build --target lxmf_direct_live_driver
python3 tools/test_lxmf_direct_live.py \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --driver build/tests/lxmf_direct_live_driver \
  --output /tmp/lxmf-direct-report.json

python3 tools/test_lxmf_direct_live.py \
  --transport tcp \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --driver build/tests/lxmf_direct_live_driver \
  --output /tmp/lxmf-direct-tcp-report.json
```

The script checks exact upstream revisions and rejects tracked source changes.
It verifies imported modules originate inside those checkouts. Python 3.9 or
newer is required; RNS's bundled cryptography fallback is usable without an
external Python cryptography package. Loopback socket permission is required.
The driver is built normally but not registered as an automatic CTest because
the pinned Python source checkouts are an explicit external prerequisite.

Each endpoint creates a temporary synthetic identity. After both announce
identities are known, each sends three messages: a 17-byte packet-sized
message; a 2,048-byte incompressible message requiring five Resource parts;
and a 23-byte-content message whose deterministic binary extension exceeds
1 MiB and requires two Resource segments and more than 2,200 parts. The harness
requires all of these to pass:

- Both receivers validate source signatures, exact synthetic content and exact
  deterministic extension bytes.
- Both directions preserve titles and unknown binary extension fields.
- All three sends in both directions reach DELIVERED through validated packet
  or Resource proofs.
- Python selects packet, Resource and Resource representations; both endpoints
  report that the final transfer spans more than one Resource segment.
- The C receiver retains the complete representation after verification.

The first real exchange revealed that the C router discarded the packed
representation for verified incoming messages. The separately committed
retention fix is required: do not weaken the metadata assertion to make an
older build pass.

`tests/fixtures/lxmf_direct_udp_live.provenance.json` records a successful run,
upstream revisions, binary/source fingerprints, public message IDs, state
transitions, validation results and part counts. It contains no plaintext,
keys, identity files, histories or raw packet capture. New runs generate new
identities and IDs. Temporary identity/configuration/history data is removed
on normal completion, failure and child termination.

`tests/fixtures/lxmf_direct_tcp_live.provenance.json` records the equivalent
framed-TCP run from committed native source `9e38600`. An earlier TCP attempt
identified a real MTU negotiation defect: the C responder confirmed Python's
16 KiB stream MTU despite using 500-byte packet/framing buffers, so Python's
large Resource parts were correctly sent but could not be admitted. The link
layer now signs a downgrade to its actual local bound. The new multi-segment
acceptance case exposed a second defect: the native TCP egress queue held only
eight frames while pinned Python can grow a Resource request window to 75
parts. Queue capacity is now derived from that pinned window maximum and the
bounded number of active runtime links. The final exchange receives all six
packet/Resource completion proofs.

Validation checkpoint: AppleClang warnings-as-errors and the complete offline
suite pass. Focused link/direct-router/runtime-Resource state-machine tests pass
with ASan/UBSan, and the normal committed driver passes both live UDP and TCP
exchanges. LeakSanitizer is unsupported by this macOS runtime; GCC was
unavailable. This is narrowly scoped bidirectional UDP and continuous
framed-TCP direct-message evidence, not full NomadNet parity: relay hops,
loss/reconnect, stamps/tickets, ratchet enforcement, propagation, physical
RNode and end-user TUI workflows are outside this acceptance case.
