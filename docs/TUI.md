# Nomad Chat terminal interface

`nomad-chat tui IDENTITY STORE [DESTINATION]` opens the full-screen terminal
client. `IDENTITY` is a 64-byte Reticulum private identity, `STORE` is the local
LXMF history/outbox, and the optional destination is a 32-character LXMF
delivery address. A destination is useful when the store has no conversations.

The header always reports the local delivery address and the real connectivity
state. Without `--config` it says **OFFLINE**: the TUI queues signed LXMF
messages into the durable local outbox and never claims them as sent. With
`--config`, the caller-polled router prefers authenticated direct delivery and
selects packet- or Resource-backed transfer after identity and path discovery.
The status line reports the queue reason, transfer, proof, delivery, or failure;
starting an asynchronous attempt is never presented as a failure by itself.

## Keys

- `1`/`2`/`3`: show trusted, unknown, or untrusted conversations. New contacts
  begin in Unknown; the grouping is persisted in the peer store.
- `Up`/`Down` or `k`/`j`: select a visible conversation and clear its in-memory
  unread marker.
- `Tab`: select the next conversation.
- `Page Up`/`Page Down`: scroll conversation history.
- `/`: search addresses, local notes, and message text in the current trust tab.
- `Enter`: open the composer; in the composer, queue the message locally.
- `d`: switch the compose delivery mode between direct delivery and an explicit
  propagation-node upload. Direct is the default, and propagated messages never
  silently fall back to direct delivery.
- `v`: explicitly save the first attachment from the newest message that has
  one. Saving is disabled unless `RETICULUM_ATTACHMENT_DIR` names an existing
  absolute directory. The retained packed message is read only for this action;
  names are reduced to traversal-safe leaf names, symlink directories are
  rejected, and an existing file is never overwritten.
- `a`: enter a 32-hex LXMF address, select or create that conversation, and
  open its composer.
- `Left`/`Right`, `Home`/`End`, `Delete`, and `Backspace`: edit input. Editing
  is UTF-8 aware: multi-byte characters are accepted from the terminal one byte
  at a time and are never split by cursor movement or deletion.
- `Ctrl-A`/`Ctrl-E`, `Ctrl-B`/`Ctrl-F`, `Ctrl-U`/`Ctrl-K`, and `Ctrl-W`:
  move to line boundaries, move by one character, erase around the cursor, or
  erase the previous word.
- `i`: show peer address, trust, unread, pin/block state, local note, and a
  clearly labelled QR-display placeholder.
- `p`, `x`, `t`, `u`, `n`: toggle pin, block, trusted, untrusted, and a contact
  note placeholder. Each change is written to the peer store immediately.
  Blocking is enforced in the LXMF router before history, ticket learning,
  callbacks, or packet proofs. Trust does not bypass stamp requirements.
- `y`: show the portable copy fallback. Clipboard integration is unavailable;
  copy message text from `history` or `tui --dump-ui` instead.
- `F`: show the validated Reticulum configuration, including interface
  endpoints and core transport/share policy. This checkpoint is read-only;
  edit the selected configuration file outside the TUI and restart to apply it.
- `R`: open the basic RRC client. Enter the hub's 32-hex destination and
  128-hex public identity, choose a nick and room, then use the action rows to
  connect, configure automatic reconnect, join, part and send. Hub details,
  nick, last room, reconnect preference and the unsent message draft are saved
  in the versioned application settings sidecar and restored after restart.
  Invalid edits remain open and do not replace the active or saved value.
- `?`: show the keyboard help overlay.
- `Esc`: close the active editor or dialog first; cancel an active Browser
  request; return other screens to Conversations. In Conversations, a second
  `Esc` leaves the TUI.
- `q`: leave the TUI when not composing.

The top navigation includes Conversations, Network, Browser, Settings,
Interfaces, Config, Guide, Logs, and RRC screens. Conversations is implemented. Network renders the
bounded active-node registry from verified announces, and Browser renders parsed
Micron content with link selection plus bounded back history. Under `--config`
the browser performs real path discovery, link establishment and page requests,
and reports loading, failure and cancellation states. Settings, Interfaces,
Config and Guide are also functional. RRC has a basic authenticated live client
for text messages; room/member history, Resource envelopes and hub management
remain explicit gaps. Logs provides a bounded in-memory view of user-visible
status transitions, including delivery, browser and runtime-startup results. It excludes
message plaintext, key material and packet captures, and is cleared on restart.

In Network, use `j`/`k` or the arrow keys to select a verified announce. The
list scrolls around the selection, and the selection is held by destination
address rather than by row, so it stays on the same node as new announces
re-sort the list. Rows that serve Micron pages are marked `PAGE`.
Press `/` to filter by display name or any destination-address fragment; the
filtered list retains reachable/newest/address ordering and anchored selection.

Enter opens a details popup showing the address, announced name, node type,
route, and associated LXMF inbox, followed by the actions that apply to that
node: `b` to browse its pages, `m` to open a conversation with its
`lxmf.delivery` address, `p` to save a verified enabled propagation node with a
valid advertised cost, `r` to refresh its path, and `Esc` to close. Actions
that cannot apply are listed with the reason, because most announces are LXMF
inboxes or transport nodes that serve no pages at all.

For the currently selected propagation node, `s` starts one explicit
list/download/ack sync. While it is active the same action cancels it. Settings
also has a `Sync Now` row that changes to `Cancel Sync` and shows the current
path/link/list/download/ack phase plus received/available counts. Completion
shows accepted, durable-duplicate, rejected and acknowledged totals. Rejected
items remain on the node; cancellation and transport failures do not discard
conversations or previously stored messages.

Delivery markers are `[.]` queued, `[>]` sending, `[+]` sent, `[x]` delivered,
and `[!]` failed. Incoming messages have no delivery marker. On narrow terminals
the contacts sidebar is hidden; below 38 columns by 10 rows the UI displays a
resize prompt instead of drawing an unusable layout.

A saved propagation-node address stays inert until a fresh cryptographically
verified `lxmf.propagation` announce says the node is enabled and advertises a
cost from 1 to 254. The Settings screen distinguishes ready, stale, disabled,
invalid-cost and awaiting-announce states. `[+]` for propagated delivery means
the node accepted the upload; it does not claim the final recipient received it.
Manual download sync starts only while that saved node has a fresh, reachable,
enabled and cost-bearing verified announce. It is caller-polled and does not
block keyboard input. Automatic sync scheduling is disabled and not yet
implemented.

For automated checks and accessibility, `nomad-chat tui --dump-ui IDENTITY
STORE [DESTINATION]` prints the same essential state without starting curses.

## Page loading

Nomad Network sends any page larger than the link MDU as a Reticulum
**Resource**, which is how nearly every real page arrives. Both packet-backed
and resource-backed pages now load. While a request is in flight the browser
shows the progress line alone; a retained page from a previous URL is only
redisplayed after a failure and is labelled as such.

Resources spanning more than one segment, or more than the 74 parts a single
advertisement hashmap carries, remain unsupported. The Micron renderer handles
sections, headings, formatting, colours, alignment, dividers and selectable
links. Tables and partials are flattened; form editing/submission, executable
pages, file downloads, in-page anchor positioning and cache directives remain.

Use `nomad-chat tui --config CONFIG IDENTITY STORE [DESTINATION]` to start the
caller-polled UDP/TCP runtime. Verified announces populate the Network screen;
an invalid or unavailable network configuration leaves local conversations usable.

When the runtime cannot start, the status line names the reason: an unreadable
file, the parser's line number and message, a configuration that defines no
interfaces, or the first interface that failed with its error. The header shows
`OFFLINE` with no runtime, `NO LINK` when a runtime exists but no interface
started, and `ONLINE` otherwise.

The runtime exposes per-interface state and errors. `ONLINE` means at least one
configured interface is up; `NO LINK` means none is usable. It does not by
itself prove that a remote destination is reachable, so route, link and request
failures remain visible without discarding local history or the last page.

## Current persistence boundary

Messages and delivery states use the durable LXMF store. Trust grouping, pins,
blocks, notes, and unread counts are persisted in the companion peer store
(`STORE.peers`), and the known-node registry in `STORE.nodes`. Full packed
representations, delivery metadata and queue state are journaled. The current
composer draft and search term remain in-memory. Queued messages are never
presented as delivered.
