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

- `Up`/`Down` or `k`/`j`: select a conversation.
- `Tab`: select the next conversation.
- `Page Up`/`Page Down`: scroll conversation history.
- `Enter`: open the composer; in the composer, queue the message locally.
- `Left`/`Right` and `Backspace`: edit the composer.
- `Esc`: cancel composition, or leave the TUI when not composing.
- `q`: leave the TUI when not composing.

Delivery markers are `[.]` queued, `[>]` sending, `[+]` sent, `[x]` delivered,
and `[!]` failed. Incoming messages have no delivery marker. On narrow terminals
the contacts sidebar is hidden; below 38 columns by 10 rows the UI displays a
resize prompt instead of drawing an unusable layout.

For automated checks and accessibility, `nomad-chat tui --dump-ui IDENTITY
STORE [DESTINATION]` prints the same essential state without starting curses.
