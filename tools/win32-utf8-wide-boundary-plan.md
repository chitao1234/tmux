# Win32 UTF-8 / Wide-Char Boundary Plan

Date: 2026-05-16

Status: Draft

Related docs:

- [`tools/win32-port-findings.md`](win32-port-findings.md)
- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)
- [`tools/win32-console-relay-plan.md`](win32-console-relay-plan.md)
- [`tools/win32-server-side-handle-io-plan.md`](win32-server-side-handle-io-plan.md)

## Goal

Establish a single, explicit string boundary for the Win32 port:

- tmux internals remain UTF-8 `char *` and byte-oriented data;
- Win32 OS boundaries use `wchar_t` only at the edge;
- narrow CRT calls on Windows are not used for environment, filesystem,
  process creation, cwd discovery, or shell validation;
- text conversion policy is consistent at the console boundary.

This is a boundary hardening plan, not a transport redesign and not an auth
redesign.

## Boundary Contract

The intended model is:

1. Process entry:
   - `wmain` / `GetCommandLineW` / wide argv are decoded to UTF-8 once.
2. Environment:
   - Windows environment is imported as wide strings and normalized into
     tmux's UTF-8 environment store.
   - tmux environment updates are exported back to Win32 through one wide
     boundary helper.
3. Filesystem and paths:
   - all real file and directory operations on Windows cross through wide
     helpers;
   - path comparisons and validation remain UTF-8 inside tmux;
   - no mixed wide-then-narrow fallback for the same path.
4. Process creation:
   - child argv, cwd, and environment are converted to UTF-16 once for
     `CreateProcessW`.
5. Console text:
   - tmux keeps UTF-8 internally;
   - real console output becomes UTF-16 only at the final writer boundary.

The boundary is not:

- `getenv()` on Windows;
- `_environ` as canonical process state;
- `fopen()` / `open()` / `unlink()` / `access()` for Windows-facing paths;
- `wchar_t` as an internal Unicode model.

## Current Problems To Close

The current tree already has partial wide-char handling, but it is not
systematic.

- Startup environment import still aliases `_environ` in
  [`win32-error.c`](../win32-error.c).
- `tmux.c` still copies the CRT environment directly into `global_environ`
  after startup.
- `compat/setenv.c` still mutates `_putenv` / `_environ` on Win32.
- `tmux.c` and `win32-error.c` still use `getenv()` for Windows-facing values
  such as `SHELL`, `HOME`, `USERPROFILE`, `TERMINFO`, `TMUX`, `VISUAL`, and
  `EDITOR`.
- `cfg.c` still loads config files with narrow `fopen()` on Windows.
- `file.c` still falls back from `CreateFileW()` to narrow `open()` for some
  non-regular path cases.
- `win32-ipc.c` still removes socket cleanup paths with narrow `unlink()`.
- `win32-event.c` still falls back to permissive UTF-8 conversion after a
  strict `MultiByteToWideChar()` failure for console writes.
- `utf8.c` exposes `utf8_towc()` / `utf8_fromwc()` even though Windows
  `wchar_t` is UTF-16 code-unit sized, not a stable internal scalar type.

## Implementation Stages

### Stage 1: Make the boundary explicit

- Define the Win32 UTF-8 / wide-char boundary in code comments and helper
  names.
- Separate strict conversion helpers from any display-only or compatibility
  fallback helpers.
- Stop treating `wchar_t` as a general-purpose internal tmux text type.

### Stage 2: Fix environment ingress and egress

- Replace the `_environ` startup import with a wide environment import path.
- Normalize imported environment entries into tmux's UTF-8 environment store.
- Replace direct Win32-facing `getenv()` uses with either `global_environ`
  lookups or explicit wide helpers.
- Replace `compat/setenv.c` / `unsetenv()` Win32 behavior with a wide
  boundary helper, then refresh tmux's UTF-8 environment state from that
  helper.
- Make the client identify path send normalized UTF-8 environment strings
  only, not raw CRT snapshots.

### Stage 3: Close filesystem and path leaks

- Replace narrow config-file loading in `cfg.c` with a wide path helper.
- Remove narrow fallbacks in `file.c` where a Windows path has already been
  normalized to UTF-16.
- Convert socket cleanup and managed-root cleanup in `win32-ipc.c` to wide
  path operations.
- Audit `log.c`, `popup.c`, `status.c`, `server.c`, and `client.c` for any
  Win32-facing path operation that still relies on narrow CRT behavior.
- Remove `MAX_PATH`-limited assumptions from the helper layer where possible.

### Stage 4: Harden process and shell boundaries

- Make shell validation Windows-aware instead of byte-oriented.
- Ensure `default-shell` and related path checks are validated in the same
  encoding used for process creation.
- Keep `CreateProcessW()` as the only process creation boundary.
- Ensure child environment building preserves all valid UTF-8 entries and
  fails in a documented way for invalid data.

### Stage 5: Make console conversion policy consistent

- Keep tmux core text UTF-8.
- Keep the real console writer as the only UTF-16 text sink.
- Decide whether invalid UTF-8 at the console boundary is rejected or
  sanitized, then apply that rule consistently.
- Avoid using console code page changes as part of correctness.

### Stage 6: Validation

- Add or update tests for:
  - non-ASCII `HOME`, `USERPROFILE`, `SHELL`, `EDITOR`, `VISUAL`, and `TMUX`;
  - non-ASCII config paths;
  - non-ASCII socket and IPC paths;
  - Unicode file read/write paths;
  - child process startup from a Unicode cwd;
  - console output that crosses multibyte boundaries;
  - invalid UTF-8 behavior at the console writer boundary.
- Verify that the Win32 path no longer depends on raw `_environ`,
  `fopen()`, `open()`, `unlink()`, or `access()` for OS-facing state.

## Priority Order

1. Environment import/export.
2. Filesystem and path helpers.
3. Shell and process-boundary validation.
4. Console conversion policy cleanup.
5. Test coverage and regression checks.

That order matters because environment and path bugs affect almost every
startup path, while console conversion bugs are narrower and more localized.

## Non-Goals

- Do not redesign relay versus direct-handle transport here.
- Do not redesign IPC authentication here.
- Do not replace UTF-8 as tmux's internal string model.
- Do not introduce a general-purpose wide-string API inside tmux core.

## Acceptance Criteria

This plan is complete when:

- tmux's canonical environment on Win32 is sourced from a wide boundary, not
  `_environ`;
- all Win32-facing filesystem and process paths cross through wide helpers;
- no Win32 startup or runtime path depends on narrow CRT semantics for
  environment or filesystem state;
- console output conversion policy is explicit and consistent;
- tests cover non-ASCII and invalid-input cases at the new boundary.
