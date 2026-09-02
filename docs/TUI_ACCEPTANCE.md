# Nomad Chat TUI Acceptance Contract

The terminal client is accepted as a view over the same identity and persistent
LXMF store used by the non-interactive commands. It must not introduce a second
history format or silently create a replacement identity.

## Entry points

```text
nomad-chat tui IDENTITY STORE [DESTINATION]
nomad-chat tui --dump-ui IDENTITY STORE [DESTINATION]
```

The first form starts the interactive ncurses client. The optional destination
selects its initial conversation; without one, the client opens with no selected
conversation. `q` exits, arrow keys change selection or scroll, and Enter sends
non-empty composer text. Input errors are printed before curses starts.

`--dump-ui` renders the initial screen as deterministic UTF-8 text and exits.
It is the supported automation and accessibility seam: it must not initialize
ncurses, require a PTY, read stdin, emit ANSI escapes, inspect terminal size, or
modify the identity/store. It must work with `TERM=dumb` and `LC_ALL=C`.

The snapshot contains these stable, grep-friendly lines:

```text
Nomad Chat
Identity: <32 lowercase hex digits>
Conversation: <32 lowercase hex digits or none>
Messages: <decimal count>
```

Messages for the selected conversation follow in persistent-store order. Every
row exposes its delivery state and safe textual content. Control bytes in
content are escaped; they must never become terminal control sequences. Styling,
spacing, borders, help text, and future metadata are intentionally not snapshot
API and tests must not match them exactly.

## Automated acceptance

`tests/test_tui_cli.cmake` is PTY-independent and performs a user-level flow:

1. Create an identity with `nomad-chat init`.
2. Seed two queued messages through `nomad-chat repl`.
3. Confirm the existing `history` command reads both messages.
4. Render the selected conversation with `TERM=dumb --dump-ui` and verify its
   identity, destination, count, and both message bodies.
5. Confirm missing arguments return `EX_USAGE` (64) and a missing identity
   returns `EX_DATAERR` (65), without entering curses.

The CMake test should be registered only when the `nomad-chat` target exists:

```cmake
add_test(NAME test_tui_cli
  COMMAND ${CMAKE_COMMAND}
    -DNOMAD_CHAT=$<TARGET_FILE:nomad-chat>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_tui_cli.cmake)
```

Manual release testing still needs a real UTF-8 terminal. Verify resize and
narrow-window behavior, keyboard navigation, long and multiline messages,
empty history, safe control-character rendering, sending and immediate history
refresh, useful delivery-state labels, and clean restoration of terminal state
after normal exit, signals, and runtime errors.

## Release criteria

- Existing identities and stores open without mutation or data loss.
- Empty, two-message, and maximum-length histories render without crashes.
- No message content can inject terminal escapes or corrupt the screen.
- The UI remains usable at 80x24 and degrades to a clear warning below its
  supported minimum size.
- A queued message appears immediately, survives restart, and retains its
  delivery state.
- Sanitizer builds pass both the headless acceptance flow and interactive smoke
  testing.
