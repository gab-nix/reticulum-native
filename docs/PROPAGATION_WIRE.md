# LXMF 1.1.0 propagation codec boundary

The allocation-free API in `reticulum/lxmf_propagation.h` handles serialization
only. It does not start a propagation session, verify a peer, encrypt LXMF,
validate propagation stamps, authorize acknowledgements or host a node.

The pinned upstream shapes are:

| Operation | MessagePack object |
| --- | --- |
| Propagation announce | `[false, timebase, enabled, transfer_limit_kb, sync_limit_kb, [stamp_cost, flexibility, peering_cost], metadata]` |
| Packet/resource upload | `[timestamp, [destination + encrypted_message + propagation_stamp, ...]]` |
| Packet upload rejection | `[0xf5]` |
| `/get` list request | `[nil, nil]` |
| `/get` download request | `[wanted_ids, already_have_ids, transfer_limit_kb]` |
| `/get` acknowledgement | `[nil, already_have_ids]` |
| List response | `[transient_id, ...]` |
| Download response | `[destination + encrypted_message, ...]` |
| `/get` authentication failures | Integer `0xf0` (identity absent) or `0xf1` (access denied) |

The download response excludes the propagation stamp. A transient ID is the
32-byte SHA-256 hash of the destination prefix plus encrypted message, also
excluding the propagation stamp. Limits are decimal kilobytes, not KiB.

Acknowledgements can delete the server's only copy. Session code must not send
them merely because a response decoded: validate and durably store the message
first. Upload/resource proofs certify the transport operation; these codecs
do not issue proofs or elevate opaque bytes to a verified message.

Decoders return borrowed immutable slices, with no heap allocations and no
output mutation on failure. Caller-supplied buffers are mandatory for encoders;
the output length is set only on success. The implementation limits batches to
128 items, total wire input/output to 8 MiB, announces to 4096 bytes and unknown
metadata nesting to 16 levels/4096 objects. Unknown metadata map bytes and
outer announce extension objects are retained. Negative, non-finite, malformed
or oversized values fail explicitly. The pinned Python validator accepts some
coercible non-canonical types that this strict codec deliberately rejects.
The encoder uses canonical unsigned integers and float64; parsing float32 or
non-minimal integer encodings preserves values, not the original numeric bytes.

## Reproducing the offline evidence

Check out the exact commits recorded in the fixture provenance, then run:

```sh
python3 tools/generate_lxmf_propagation_fixtures.py \
  --reticulum /path/to/pinned/Reticulum \
  --lxmf /path/to/pinned/LXMF \
  --output-dir tests/fixtures
ctest --test-dir build -R test_lxmf_python_propagation --output-on-failure
```

The generator executes upstream announce and list/get handler methods against
synthetic in-memory peers and temporary synthetic message files. For upload
and rejection packing it extracts and executes the actual pinned assignment
expressions, avoiding a local restatement as the fixture oracle. It pins both
Git revisions and verifies import origins. Fixture provenance records source
methods/expressions and SHA-256 hashes. No real identity, message, private key
or capture is committed. Tests consume Python bytes and reproduce canonical
representations, including unknown metadata, UTF-8 name data, floating limits,
denied access, empty limited downloads and malformed/truncated input.

This is offline serialization evidence, not C↔Python network verification.
Node selection, upload/list/download sessions, deadlines, interrupted-transfer
recovery, cryptography, propagation stamps and peering remain separate gates.

The isolated libFuzzer harness (Clang) can run without sockets or a runtime:

```sh
clang -std=c17 -Iinclude -fsanitize=fuzzer,address,undefined \
  src/lxmf/propagation.c tests/fuzz/fuzz_lxmf_propagation.c \
  -o build/fuzz-lxmf-propagation
build/fuzz-lxmf-propagation -runs=10000 -max_len=4096
```

Implementation checkpoint: AppleClang 17 warning-as-error build and all 57
tests passed (socket tests required loopback permission). The focused fixture,
truncation and mutation tests passed under ASan/UBSan; Clang static analysis
reported no findings. The harness itself compiles, but this host lacks
`libclang_rt.fuzzer_osx.a`, so libFuzzer execution was not performed. GCC is not
installed on this host; its build gate remains outstanding.
