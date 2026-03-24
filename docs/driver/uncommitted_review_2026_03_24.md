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

## Validation Updates (2026-03-25)

The original review has now been revalidated against code changes and runtime
tests:

- The earlier gateway session refcount leak callout did not reproduce after
  adding a lifecycle regression test. The current teardown path drops the
  interactive-owned reference as expected.
- The `gateway_session_send()` double-free issue has been fixed in code.
- The `compile_error_*` mudlib contract is now covered by LPC-side integration
  tests.
- The shared input-buffer limit has been restored to 1 MiB, while gateway
  packet-size enforcement continues to use its own `max_packet_size` config and
  regression coverage.

## Fixed Findings

### 1. `gateway_session_send()` could free the same JSON document twice

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

Resolution:

- The error paths now leave ownership to the common cleanup branch so each
  `yyjson_mut_doc *doc` is released exactly once.
- Cleanup now also nulls the pointer after free to keep future refactors safe.

### 2. `compile_error_*` runtime fields needed an LPC-side contract test

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

Resolution:

- `testsuite/single/master.c` now records the last error mapping for test
  inspection.
- `src/tests/test_lpc.cc` now asserts the exact `compile_error_*` keys and
  values delivered to `master::error_handler` for both direct syntax errors and
  include-header failures.

### 3. The shared interactive input buffer had been reduced from 1 MiB to 64 KiB globally

**Severity:** medium

`MAX_TEXT` had been reduced globally and packet-length validation in `comm.cc`
would reject larger MUD packets before they reached the existing command
pipeline.

Evidence:

- `src/interactive.h`
- `src/comm.cc`
- `src/tests/test_lpc.cc`

Impact:

- This affected all interactive input that shares the same buffer contract, not
  only gateway traffic.
- Existing mudlibs or clients that relied on larger packet payloads could be
  disconnected unexpectedly.

Resolution:

- `MAX_TEXT` has been restored to `1 * 1024 * 1024`.
- `interactive_t` no longer embeds a fixed 1 MiB array. The shared input buffer
  now uses dynamic allocation and grows on demand up to the same hard limit.
- A regression test now asserts both the 1 MiB hard limit and that
  `interactive_t` no longer embeds the full buffer size.
- Additional tests verify that the runtime buffer starts small, grows on
  demand, and that gateway sessions use the same small shared input-buffer
  strategy instead of preallocating 1 MiB per session.
- A second regression test verifies that gateway packet-size control remains
  independent and can still exceed `MAX_TEXT` through `gateway_config("max_packet_size", ...)`.

### 4. Gateway session teardown refcount balance was revalidated

**Severity:** reviewed

The original review suspected that gateway session teardown leaked one object
reference. A dedicated lifecycle test now exercises session creation and
destruction against the real teardown path.

Evidence:

- `src/tests/test_lpc.cc`
- `testsuite/clone/gateway_login_example.c`

Result:

- The current teardown path drops the interactive-owned reference as expected.
- No production code change was required for this item, but the regression test
  now protects the refcount contract from future breakage.

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
