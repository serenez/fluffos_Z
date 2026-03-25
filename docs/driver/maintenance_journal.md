---
layout: doc
title: driver / maintenance journal
---
# Driver Maintenance Journal

This file is the single place to track driver-side maintenance updates.

## Update Rule

From this point forward, every non-trivial code change should update this file.
Each entry should include:

- date
- commit id
- scope
- what changed
- validation
- related docs or audit files when applicable

This keeps the recent maintenance history readable without forcing a full
`git log` review every time.

## Recent Timeline

### 2026-03-25 `67331e79`

**Title**

`fix(driver): 修复审计台账中的输入边界与保存路径问题`

**Scope**

- `save_object`
- parser recursion
- `add_action` error path
- `mudlib_stats` restore path
- audit ledger updates

**What changed**

- Replaced fixed-size `save_object` header handling with dynamic string
  construction to avoid overflow on long object names.
- Added short-path length guards before checking `".c"` suffixes.
- Reworked parser recursive phrase joining to avoid the fixed 1024-byte stack
  buffer.
- Removed the intermediate fixed `buf` and `error(buf)` pattern in
  `add_action`, eliminating both buffer overflow risk and format-string
  re-parsing.
- Replaced `mudlib_stats` restore parsing based on `fscanf("%s")` with dynamic
  stream parsing.
- Updated the full audit ledger and marked the audited issues as fixed.

**Validation**

- Rebuilt `build_codex_review_fix` `lpc_tests` with MSYS2 MINGW64.
- Ran `ctest --output-on-failure -j 1`.
- Result: `40/40` passed.

**Related docs**

- `docs/audit/full_code_audit_2026_03_25.md`

### 2026-03-25 `dbe3f9ae`

**Title**

`fix(driver): 修复异步 IO、DNS 与 DB 并发安全问题`

**Scope**

- async file IO
- async `getdir`
- DNS request lifecycle
- DB async/sync locking
- socket address parsing
- mudlib stats long-string handling

**What changed**

- Fixed async `getdir` heap overwrite by switching from raw `dirent` storage to
  filename string storage.
- Added failure handling for `open`, `read`, `gzopen`, and `gzdopen` paths in
  async read/write flows.
- Added lock coverage so `db_close` and related DB paths no longer race with
  async DB execution.
- Hardened socket `"host port"` parsing against overlong input.
- Fixed DNS null-base and submit-failure handling so callbacks clean up
  correctly.
- Added regression coverage for long author/domain strings, DNS failure
  callback scheduling, and overlong socket host input.

**Validation**

- Rebuilt `lpc_tests` in `build_codex_review_fix`.
- Ran `ctest --output-on-failure -j 1`.
- Result: `39/39` passed.
- Built an extra SQLite-enabled configuration to confirm DB code still
  compiled.

### 2026-03-25 `3b531567`

**Title**

`fix(driver): 修复 lpcc tracing 落盘与语法生成漂移`

**Scope**

- `lpcc` tracing lifecycle
- grammar autogen reproducibility

**What changed**

- Ensured `lpcc --tracing` collects traces on both success and early-failure
  paths.
- Adjusted Bison generation flags to avoid embedding host-specific absolute
  paths in generated grammar outputs.
- Refreshed generated grammar files to make diffs stable across machines.

**Validation**

- Rebuilt `lpcc`.
- Verified default run does not create a trace.
- Verified explicit tracing writes a file on both success and failure.
- Ran `ctest`, result at that point: `36/36` passed.

### 2026-03-25 `a2bccf2f`

**Title**

`fix(lpcc): 调整 tracing 开关为显式启用`

**Scope**

- `lpcc` CLI behavior
- tracing defaults

**What changed**

- Removed unconditional `trace_lpcc.json` generation at startup.
- Added `--tracing <trace.json>` to opt into tracing explicitly.
- Tightened CLI parsing and usage reporting around the tracing flag.

**Validation**

- Rebuilt `lpcc`.
- Verified default execution no longer creates `trace_lpcc.json`.
- Verified explicit tracing output works with a caller-provided path.

### 2026-03-25 `5495a02c`

**Title**

`fix(driver): 修复 gateway 会话生命周期与输入竞态`

**Scope**

- gateway session lifecycle
- `exec()` object remapping
- pending logon scheduling
- `process_input()` keepalive
- telnet/tls init failure paths

**What changed**

- Fixed gateway session teardown and `exec()` remap edge cases so session
  lookup remains valid after object transfer.
- Removed UAF/iterator invalidation risks in gateway master read and broadcast
  paths.
- Prevented pre-logon input from running too early and tightened cleanup when
  `connect()` fails.
- Added object keepalive around `process_input()` to avoid refcount fatal
  issues after `exec()`.
- Added missing null/resource checks in telnet and TLS setup paths.

**Validation**

- Rebuilt `lpc_tests`.
- Ran `ctest`.
- Result at that point: `36/36` passed.

### 2026-03-25 `95eab12e`

**Title**

`fix(driver): 收口预登录竞态并补齐交互失败路径防护`

**Scope**

- pending user cleanup
- delayed logon event lifecycle
- TLS and websocket init failure rollback
- ASCII post-destruction safety

**What changed**

- Stopped ASCII processing from touching a freed `interactive_t` after
  `process_input()` destroys the owning object.
- Reworked delayed logon scheduling so pending users can be cancelled cleanly.
- Added `cleanup_pending_user()` to centralize pre-login resource cleanup.
- Prevented TLS `SSL_new()` failure from silently degrading into a non-TLS
  session.
- Unified websocket failure cleanup with the pending-user cleanup path.

**Validation**

- Ran `git diff --check`.
- Rebuilt `lpc_tests`.
- Ran the test binary directly.
- Result at that point: `32/32` passed.

### 2026-03-25 `ed4a8a9d`

**Title**

`fix(driver): 修复 ASCII 端口边界解析并补齐分配失败防护`

**Scope**

- ASCII input line splitting
- allocation failure rollback

**What changed**

- Fixed empty-line handling around `\r` lookback.
- Fixed same-read parsing for sequences like `cmd1\r\ncmd2\n`.
- Added cleanup on `evtimer_new()` failure in `new_user()`.
- Added websocket rollback on `evbuffer_new()` failure.

**Validation**

- Rebuilt `lpc_tests`.
- Ran the test binary directly.
- Result at that point: `30/30` passed.

### 2026-03-25 `aaf3a495`

**Title**

`fix(driver): 修复动态交互缓冲回归并补强网关链路`

**Scope**

- interactive dynamic text buffer
- multi-protocol input handling
- gateway packet path
- memory accounting
- review doc refresh

**What changed**

- Replaced the fixed interactive text buffer with a dynamically allocated
  buffer that starts small and grows up to the restored 1 MiB logical limit.
- Reworked input buffering for telnet, ascii, websocket, and MUD paths.
- Fixed MUD packet completion handling for chunked reads.
- Fixed last-buffered-line handling in normal TCP/Telnet cleanup.
- Fixed gateway send-path double-free and session create/destroy cleanup for
  dynamic text buffers.
- Updated memory accounting and the previous review note to reflect the real
  conclusions.

**Validation**

- Rebuilt `lpc_tests`.
- Ran the test binary directly.
- Result at that point: `28/28` passed.

## How to Extend This File

Append new entries at the top of the timeline.
Keep old entries intact.
When a change also updates audit findings, add the related audit file in the
entry.
