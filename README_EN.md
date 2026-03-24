# FluffOS

This branch keeps FluffOS's LPC driver compatibility while adding a new
gateway-facing runtime path, new LPC utility packages, and stronger compiler
diagnostics.

If you want a quick summary of what changed in this branch, start here instead
of reading the older upstream-oriented README.

The primary README for this repository is Chinese:
[README.md](README.md)

## Highlights In This Branch

### 1. Gateway package for Go or external frontends

The new `gateway` package adds a dedicated listener and LPC integration path for
an external gateway process.

It currently provides:

- a standalone gateway listener, separate from telnet/websocket ports
- a 4-byte big-endian length-prefixed JSON protocol
- master connection tracking and per-player gateway sessions
- heartbeat / timeout management
- driver-to-gateway output routing for gateway users
- LPC efuns for listening, status/config inspection, broadcast, targeted send,
  session lookup, and session teardown

Gateway docs:

- protocol and LPC integration:
  [docs/driver/gateway.md](docs/driver/gateway.md)
- LPC example object:
  [testsuite/clone/gateway_login_example.c](testsuite/clone/gateway_login_example.c)

### 2. JSON extension package

The new `json_extension` package adds JSON utilities that can be used directly
from LPC:

- `json_encode(mixed)`
- `json_decode(string)`
- `write_json(string, mixed)`
- `read_json(string)`
- `sort_mapping(mapping, int|void)`
- `sort_mapping_int(mapping, int|void)`

### 3. Utility packages

Two more LPC-facing packages were added:

- `myutil`
  - `uuid()`
  - `strbuf_new()`
  - `strbuf_add()`
  - `strbuf_addf()`
  - `strbuf_dump()`
  - `strbuf_clear()`
- `seed_random`
  - `seed_random()`
  - `seed_random_batch()`
  - `seed_next()`

### 4. Better compiler diagnostics

Compiler and mudlib error handling now expose richer structured compile
diagnostics, including file, line, column, source snippet, and caret position.

This branch also adds new regression coverage for:

- direct syntax errors
- include/header syntax errors
- parameter type diagnostics
- locals growth and scratchpad behavior
- heartbeat lifecycle behavior
- inherit list limits
- UTF-8 truncation and eval deadline paths

### 5. Runtime and platform work

The change set also includes runtime cleanups and platform work, including:

- gateway-aware interactive/session fields
- gateway-aware `exec()` and `remove_interactive()` handling
- offline startup path for tools that do not require a backend loop
- Windows console UTF-8 / UTF-16 logging improvements
- heartbeat, call_out, eval deadline, and inherit-list robustness work

## Quick Build

Primary targets remain Linux, macOS, and Windows via MSYS2 / MINGW64.

If you do not need the DB package, a simple local build is:

```bash
cmake -S . -B build -G Ninja -DPACKAGE_DB=OFF
cmake --build build --target driver
```

Useful package toggles for this branch:

- `-DPACKAGE_GATEWAY=ON`
- `-DPACKAGE_JSON_EXTENSION=ON`
- `-DPACKAGE_MYUTIL=ON`

Gateway runtime config keys added by this branch:

- `gateway port`
- `gateway external`
- `gateway debug`
- `gateway packet size`

## Where To Start In Code

- gateway core:
  [`src/packages/gateway/`](src/packages/gateway)
- gateway LPC efuns:
  [`src/packages/gateway/gateway.spec`](src/packages/gateway/gateway.spec)
- gateway runtime bootstrap:
  [`src/mainlib.cc`](src/mainlib.cc)
- JSON extension:
  [`src/packages/json_extension/`](src/packages/json_extension)
- utility packages:
  [`src/packages/myutil/`](src/packages/myutil),
  [`src/packages/seed_random/`](src/packages/seed_random)

## Related Notes

- code review follow-up notes for the large landing commit:
  [docs/driver/uncommitted_review_2026_03_24.md](docs/driver/uncommitted_review_2026_03_24.md)

## Support

- website / documentation: <https://www.fluffos.info>
- forum: <https://forum.fluffos.info>
- Discord: <https://discord.gg/E5ycwE8NCc>
- QQ group: `451819151`
