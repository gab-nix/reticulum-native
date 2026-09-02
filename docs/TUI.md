# Nomad Chat terminal interface

`nomad-chat tui IDENTITY STORE [DESTINATION]` opens the full-screen terminal
client. `IDENTITY` is a 64-byte Reticulum private identity, `STORE` is the local
LXMF history/outbox, and the optional destination is a 32-character LXMF
delivery address. A destination is useful when the store has no conversations.

The header always reports the local delivery address and the real connectivity
state. The current implementation says **OFFLINE** because the TUI queues signed
LXMF messages into the durable local outbox but does not yet run a Reticulum
transport. Queued messages are not claimed as sent.

## Keys

- `1`/`2`/`3`: show trusted, unknown, or untrusted conversations. New contacts
  begin in Unknown because the current store has no persistent contact model.
- `Up`/`Down` or `k`/`j`: select a visible conversation and clear its in-memory
  unread marker.
- `Tab`: select the next conversation.
- `Page Up`/`Page Down`: scroll conversation history.
- `/`: search addresses, local notes, and message text in the current trust tab.
- `Enter`: open the composer; in the composer, queue the message locally.
- `Left`/`Right`, `Home`/`End`, `Delete`, and `Backspace`: edit input.
- `Ctrl-A`/`Ctrl-E`, `Ctrl-B`/`Ctrl-F`, `Ctrl-U`/`Ctrl-K`, and `Ctrl-W`:
  move to line boundaries, move by one character, erase around the cursor, or
  erase the previous word.
- `i`: show peer address, trust, unread, pin/block state, local note, and a
  clearly labelled QR-display placeholder.
- `p`, `x`, `t`, `u`, `n`: toggle pin, block, trusted, untrusted, and a contact
  note placeholder. These controls are session-only until contact metadata has
  a persistent storage API.
- `y`: show the portable copy fallback. Clipboard integration is unavailable;
  copy message text from `history` or `tui --dump-ui` instead.
- `?`: show the keyboard help overlay.
- `Esc`: cancel composition, or leave the TUI when not composing.
- `q`: leave the TUI when not composing.

The top navigation reserves Conversations, Network, Browser, Node, Settings,
Guide, Logs, and RRC screens. Conversations is implemented. Network now renders
the bounded active-node registry, and Browser renders parsed Micron content with
link selection plus bounded back history. Live announce ingestion and remote
page requests are still explicitly labelled disconnected. The other screens
report an explicit “not implemented” status.

Delivery markers are `[.]` queued, `[>]` sending, `[+]` sent, `[x]` delivered,
and `[!]` failed. Incoming messages have no delivery marker. On narrow terminals
the contacts sidebar is hidden; below 38 columns by 10 rows the UI displays a
resize prompt instead of drawing an unusable layout.

For automated checks and accessibility, `nomad-chat tui --dump-ui IDENTITY
STORE [DESTINATION]` prints the same essential state without starting curses.

Use `nomad-chat tui --config CONFIG IDENTITY STORE [DESTINATION]` to start the
caller-polled UDP/TCP runtime. Verified announces populate the Network screen;
an invalid or unavailable network configuration leaves local conversations usable.

## Current persistence boundary

Messages and delivery states use the durable LXMF store. Drafts, read markers,
trust grouping, pins, blocks, and notes are deliberately in-memory because the
current public store contains messages only. The UI labels every such mutation
as session-only. Network actions remain unavailable and queued messages are
never presented as delivered.
