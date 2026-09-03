# RRC interoperability checkpoint

The opt-in RRC harness verifies the native caller-polled client against
Reticulum 1.5.2 (`ea98db4f53dcf0defc0e71a16e60d28b1229c4e6`) and the
wire schema and CBOR encoder from NomadNet 1.2.0
(`475c0ee2a0388cf8470e7f1e90d5decb67b579ea`). The Python endpoint is a
small fixture hub built on the unmodified pinned RNS destination/link API. It
uses NomadNet's `_make_envelope()` and vendored CBOR module for every response.

NomadNet 1.2.0 contains an RRC client, but it does not contain a hub server.
This checkpoint therefore does **not** claim compatibility with a stock
NomadNet hub or with an external `rrcd` deployment.

## Verified exchange

On loopback UDP, the harness requires all of the following before reporting
success:

- a valid `rrc.hub` destination derived from the fixture's public identity;
- an authenticated Reticulum link and identification of the C client;
- an exact HELLO envelope and WELCOME capability/limit parsing;
- normalized room JOIN and a matching JOINED response;
- exact text-message fields in both directions and message-ID correlation;
- an eight-byte PING nonce and matching PONG;
- a room PART and matching PARTED response; and
- explicit client disconnect.

The exchange exposed a real incompatibility: RRC wire timestamps used the
caller's monotonic poll time. NomadNet envelopes use Unix epoch milliseconds.
The session now obtains wall-clock time only while encoding envelopes and
continues to use monotonic time for local deadlines and retry scheduling. Its
wall-clock provider is injectable for deterministic and embedded runtimes; the
POSIX HAL clock is only the default when no provider is supplied.

The native session also maintains a bounded room/member view from JOINED and
PARTED envelopes, retains desired rooms across an unexpected link closure and
reissues their JOIN only after the next authenticated WELCOME. Roomless NOTICE
text is retained as the current MOTD. These additions currently have pinned
fixture and deterministic C-to-C runtime evidence; they were not added to the
recorded Python interoperability result and are not claimed as verified.

The committed privacy-safe result is
`tests/fixtures/nomadnet_rrc_udp_live.provenance.json`. It contains source and
binary fingerprints, public message IDs, boolean assertions, upstream pins,
and limitations. It contains no message text, private identities, keys, or
packet captures.

## Running the opt-in harness

Build the strict test driver:

```sh
cmake -S . -B build -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_BUILD_APPS=ON \
  -DRETICULUM_WARNINGS_AS_ERRORS=ON
cmake --build build --target rrc_live_driver
```

Then provide clean checkouts, or recorded-clean source trees with adjacent
provenance markers, at the exact revisions above:

```sh
python3 tools/test_rrc_live.py \
  --reticulum /path/to/pinned/Reticulum \
  --nomadnet /path/to/pinned/NomadNet \
  --driver build/tests/rrc_live_driver \
  --output rrc-live-report.json
```

The script rejects revision drift and tracked modifications. It creates all
identities, configuration, and storage in a temporary directory.

## Remaining gates

External `rrcd` interoperability, Resource envelopes, reconnect/rejoin against
Python, transport relays, TCP, persistent message history, room-list and WHO
commands, member nick resolution, rate enforcement, and hosted hub operation
remain unverified.
