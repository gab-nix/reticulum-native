# Nomad Chat terminal interface

`nomad-chat tui IDENTITY STORE [DESTINATION]` opens the full-screen terminal
client. `IDENTITY` is a 64-byte Reticulum private identity, `STORE` is the local
LXMF history/outbox, and the optional destination is a 32-character LXMF
delivery address. A destination is useful when the store has no conversations.

The header always reports the local delivery address and the real connectivity
state. Without `--config` it says **OFFLINE**: the TUI queues signed LXMF
messages into the durable local outbox and never claims them as sent. With
`--config` it says **ONLINE** and the caller-polled runtime attempts
opportunistic delivery, so the status line distinguishes a queued message from
one that was actually sent.

## Keys

- `1`/`2`/`3`: show trusted, unknown, or untrusted conversations. New contacts
  begin in Unknown; the grouping is persisted in the peer store.
- `Up`/`Down` or `k`/`j`: select a visible conversation and clear its in-memory
  unread marker.
- `Tab`: select the next conversation.
- `Page Up`/`Page Down`: scroll conversation history.
- `/`: search addresses, local notes, and message text in the current trust tab.
- `Enter`: open the composer; in the composer, queue the message locally.
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
  Blocking records the preference only; it does not yet suppress delivery.
- `y`: show the portable copy fallback. Clipboard integration is unavailable;
  copy message text from `history` or `tui --dump-ui` instead.
- `?`: show the keyboard help overlay.
- `Esc`: cancel composition, or leave the TUI when not composing.
- `q`: leave the TUI when not composing.

The top navigation reserves Conversations, Network, Browser, Node, Settings,
Guide, Logs, and RRC screens. Conversations is implemented. Network renders the
bounded active-node registry from verified announces, and Browser renders parsed
Micron content with link selection plus bounded back history. Under `--config`
the browser performs real path discovery, link establishment and page requests,
and reports loading, failure and cancellation states. The other screens report
an explicit “not implemented” status.

In Network, use `j`/`k` or the arrow keys to select a verified announce and
press Enter. The action popup offers `B` to open the node's Nomad address or
`M` to open a conversation with the `lxmf.delivery` address derived from the
same verified identity. An unrelated announce without an associated inbox
cannot be messaged through this shortcut.

Delivery markers are `[.]` queued, `[>]` sending, `[+]` sent, `[x]` delivered,
and `[!]` failed. Incoming messages have no delivery marker. On narrow terminals
the contacts sidebar is hidden; below 38 columns by 10 rows the UI displays a
resize prompt instead of drawing an unusable layout.

For automated checks and accessibility, `nomad-chat tui --dump-ui IDENTITY
STORE [DESTINATION]` prints the same essential state without starting curses.

Use `nomad-chat tui --config CONFIG IDENTITY STORE [DESTINATION]` to start the
caller-polled UDP/TCP runtime. Verified announces populate the Network screen;
an invalid or unavailable network configuration leaves local conversations usable.

When the runtime cannot start, the status line names the reason: an unreadable
file, the parser's line number and message, a configuration that defines no
interfaces, or the first interface that failed with its error. The header shows
`OFFLINE` with no runtime, `NO LINK` when a runtime exists but no interface
started, and `ONLINE` otherwise.

`ONLINE` currently means every configured interface *started*, not that a TCP
session is established. A `TCPClientInterface` pointing at an unreachable host
connects asynchronously and still reports `ONLINE`; distinguishing it needs the
runtime to expose per-interface connection state, which it does not yet do.

## Current persistence boundary

Messages and delivery states use the durable LXMF store. Trust grouping, pins,
blocks, notes, and unread counts are persisted in the companion peer store
(`STORE.peers`), and the known-node registry in `STORE.nodes`. The composer
draft and the search term are deliberately in-memory for the session. Queued
messages are never presented as delivered.
