# Live direct LXMF interoperability

The opt-in harness exchanges real encrypted Reticulum traffic between the C
router and the unmodified pinned Python RNS 1.5.2 / LXMF 1.1.0 stack over UDP
loopback. It does not mock either protocol endpoint and is not a C-to-C test.

```sh
cmake -S . -B build -G Ninja -DRETICULUM_BUILD_TESTS=ON -DRETICULUM_BUILD_APPS=ON -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build --target lxmf_direct_live_driver
python3 tools/test_lxmf_direct_live.py \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --driver build/tests/lxmf_direct_live_driver \
  --output /tmp/lxmf-direct-report.json
```

The script checks exact upstream revisions and rejects tracked source changes.
It verifies imported modules originate inside those checkouts. Python 3.9 or
newer is required; RNS's bundled cryptography fallback is usable without an
external Python cryptography package. Loopback socket permission is required.
The driver is built normally but not registered as an automatic CTest because
the pinned Python source checkouts are an explicit external prerequisite.

Each endpoint creates a temporary synthetic identity. After both announce
identities are known, each sends a 17-byte packet-sized message and a 2,048-byte
message whose incompressible content requires five Resource parts. The harness
requires all of these to pass:

- Both receivers validate source signatures and exact synthetic content.
- Both directions preserve titles and an unknown binary extension field.
- Both senders reach DELIVERED through validated packet/Resource proofs.
- Python selects direct packet then direct Resource representation; the C
  router reports Resource transfer, with more than one actual part.
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

Validation checkpoint: AppleClang warnings-as-errors and the complete 64-test
suite pass. The live exchange also passes with the C driver instrumented with
ASan/UBSan, along with focused `test_lxmf_router_direct` and
`test_runtime_resource` runs. LeakSanitizer is unsupported by this macOS runtime;
GCC was unavailable. This is narrowly scoped bidirectional UDP direct-message
evidence, not full NomadNet parity: TCP, relay hops, loss/reconnect/restart,
stamps/tickets, ratchet enforcement, propagation, multi-segment resources,
physical RNode and end-user TUI workflows are outside this acceptance case.
