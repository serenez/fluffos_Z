---
layout: doc
title: driver / uncommitted review 2026-03-24
---
# Uncommitted Review Notes (2026-03-24)

This note records the review findings for the uncommitted workspace changes that
were present on 2026-03-24. It focuses on code-level defects and validation
gaps that should remain visible even if the change set is later squashed into a
single commit.

## Scope

- Package additions: `gateway`, `json_extension`, `myutil`, `seed_random`
- Compiler diagnostics and parser/lexer support
- Runtime/network changes around `interactive`, `comm`, `heartbeat`,
  `call_out`, `eval_limit`, and Windows startup/logging
- New unit tests and testsuite compiler fixtures

## Outstanding Findings

### 1. Gateway session creation leaks an object reference

**Severity:** high

`gateway_create_session_internal()` adds an extra reference to the login object,
but the teardown paths only remove session mappings and never release that extra
reference.

Evidence:

- `src/packages/gateway/gateway_session.cc:371`
- `src/packages/gateway/gateway_session.cc:440`
- `src/packages/gateway/gateway_session.cc:568`
- `src/packages/gateway/gateway_session.cc:907`
- `src/comm.cc:1406`

Impact:

- Gateway users that disconnect can leave player objects with an inflated
  refcount.
- Object destruction, cleanup, and mudlib-side lifecycle hooks can be delayed
  or skipped entirely for those sessions.

Recommended follow-up:

- Pair the extra `add_ref()` with a matching release in the gateway session
  teardown path, instead of relying on `remove_interactive()` to clean up the
  session-owned reference.

### 2. `gateway_session_send()` can free the same JSON document twice

**Severity:** high

Several error branches inside `gateway_session_send()` free `yyjson_mut_doc`
immediately and then fall through to a common cleanup path that frees the same
pointer again.

Evidence:

- `src/packages/gateway/gateway_session.cc:772`
- `src/packages/gateway/gateway_session.cc:812`
- `src/packages/gateway/gateway_session.cc:838`
- `src/packages/gateway/gateway_session.cc:849`

Impact:

- The failure path can corrupt the heap instead of safely reporting an error.
- Low-memory or malformed-request handling becomes unsafe.

Recommended follow-up:

- Centralize ownership so each `yyjson_mut_doc *doc` is released exactly once.
- If early free is kept, set the pointer to `nullptr` before entering common
  cleanup.

### 3. The interactive input buffer limit changed from 1 MiB to 64 KiB globally

**Severity:** medium

`MAX_TEXT` was reduced globally and packet-length validation in `comm.cc` now
rejects larger MUD packets before they reach the existing command pipeline.

Evidence:

- `src/interactive.h:10`
- `src/comm.cc:79`
- `src/comm.cc:751`
- `src/comm.cc:827`

Impact:

- This affects all interactive input that shares the same buffer contract, not
  only gateway traffic.
- Existing mudlibs or clients that relied on larger packet payloads can now be
  disconnected unexpectedly.
- The gateway runtime config still exposes packet-size knobs that no longer
  match the driver-wide input ceiling.

Recommended follow-up:

- Reconfirm whether the 64 KiB limit is intentional for the entire driver.
- If the reduction is gateway-specific, move the limit to gateway framing
  instead of the shared interactive buffer.

### 4. `compile_error_*` runtime fields need an LPC-side contract test

**Severity:** medium

The driver now injects structured compile diagnostics into the error-handler
mapping, but the new coverage only validates the compiler snapshot/log path and
does not verify what the mudlib actually receives through the master error
handler.

Evidence:

- `src/vm/internal/simulate.cc:1791`
- `src/vm/internal/simulate.cc:1808`
- `src/tests/test_lpc.cc:165`
- `src/tests/test_lpc.cc:201`
- `src/tests/test_lpc.cc:221`

Impact:

- A refactor can silently break the mudlib-facing contract while unit tests keep
  passing.
- Mudlibs consuming `compile_error_file`, `compile_error_line`,
  `compile_error_column`, or the source/caret context would not be protected by
  tests.

Recommended follow-up:

- Add an LPC integration test that asserts the exact mapping keys and values
  seen by `master::error_handler`.

## Integration Watch Items

### 1. Compiler fixture files are part of the new regression coverage

The new compiler tests depend on:

- `testsuite/include/line_error_bad_header.h`
- `testsuite/single/tests/compiler/fail/line_error_include.c`
- `testsuite/single/tests/compiler/fail/line_error_direct.c`

These files must stay versioned together with `src/tests/test_lpc.cc`; dropping
them later would silently invalidate the new regression coverage.

### 2. Removing backlog files reduces historical context

The change set deletes `TODO.md` and `QWEN.md`. If their content is still
needed, it should be moved to a maintained location instead of disappearing from
history without a replacement note.
