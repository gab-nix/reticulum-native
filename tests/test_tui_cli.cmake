if(NOT DEFINED NOMAD_CHAT)
  message(FATAL_ERROR "NOMAD_CHAT executable path is required")
endif()

set(tmp "$ENV{TMPDIR}/nomad-chat-tui-cmake-test")
if(tmp STREQUAL "/nomad-chat-tui-cmake-test")
  set(tmp "/tmp/nomad-chat-tui-cmake-test")
endif()
file(REMOVE_RECURSE "${tmp}")
file(MAKE_DIRECTORY "${tmp}")

set(identity "${tmp}/identity")
set(store "${tmp}/history.lxms")
set(input "${tmp}/repl-input")

execute_process(
  COMMAND "${NOMAD_CHAT}" init "${identity}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE address
  ERROR_VARIABLE error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0 OR NOT address MATCHES "^[0-9a-f][0-9a-f]+$")
  message(FATAL_ERROR "TUI fixture identity creation failed: ${result} ${error}")
endif()

# Seed the real persistent store through a public CLI, avoiding knowledge of its
# binary format. This also exercises that the TUI and existing history command
# consume exactly the same backing data.
file(WRITE "${input}" "first seeded message\nsecond seeded message\n/quit\n")
execute_process(
  COMMAND "${NOMAD_CHAT}" repl "${identity}" "${address}" "${store}"
  INPUT_FILE "${input}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE repl_output
  ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT repl_output MATCHES "queued [0-9a-f]+")
  message(FATAL_ERROR "TUI fixture history creation failed: ${result} ${error} ${repl_output}")
endif()

execute_process(
  COMMAND "${NOMAD_CHAT}" history "${store}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE history
  ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR
   NOT history MATCHES "first seeded message" OR
   NOT history MATCHES "second seeded message")
  message(FATAL_ERROR "seeded history cannot be read: ${result} ${error} ${history}")
endif()

# --dump-ui is the stable, PTY-independent rendering seam. TERM=dumb ensures
# the snapshot cannot accidentally depend on terminfo, cursor movement, colour,
# locale-specific glyphs, or an attached interactive terminal.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "TERM=dumb" "LC_ALL=C"
          "${NOMAD_CHAT}" tui --dump-ui "${identity}" "${store}" "${address}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE snapshot
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "headless TUI snapshot failed: ${result} ${error}")
endif()
foreach(expected
    "Nomad Chat"
    "Identity: ${address}"
    "Conversation: ${address}"
    "Messages: 2"
    "first seeded message"
    "second seeded message")
  if(NOT snapshot MATCHES "${expected}")
    message(FATAL_ERROR "headless TUI snapshot lacks '${expected}':\n${snapshot}")
  endif()
endforeach()

# Malformed invocations remain script-friendly and must never enter curses.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "TERM=dumb"
          "${NOMAD_CHAT}" tui --dump-ui
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 64 OR NOT error MATCHES "usage:")
  message(FATAL_ERROR "missing TUI arguments should return EX_USAGE (64): ${result} ${output} ${error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "TERM=dumb"
          "${NOMAD_CHAT}" tui --dump-ui "${tmp}/missing-identity" "${store}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 65 OR NOT error MATCHES "cannot open identity")
  message(FATAL_ERROR "missing TUI identity should return EX_DATAERR (65): ${result} ${output} ${error}")
endif()

file(REMOVE_RECURSE "${tmp}")
