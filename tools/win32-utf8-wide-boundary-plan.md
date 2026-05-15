# Win32 UTF-8 / Wide-Char Boundary Plan

Date: 2026-05-16

Status: In Progress

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

The current tree has the core boundary helpers in place, but a few important
edges are still incomplete.

Completed since this plan was drafted:

- Startup environment import now comes from `GetEnvironmentStringsW()` in
  [`win32-error.c`](../win32-error.c), not raw `_environ`.
- Win32 `setenv()` / `unsetenv()` now cross through wide helpers in
  [`compat/setenv.c`](../compat/setenv.c) and keep `global_environ`
  synchronized through [`win32-error.c`](../win32-error.c).
- Startup environment lookups now prefer canonical `global_environ` in
  [`tmux.c`](../tmux.c).
- Config-file loading now uses [`win32_fopen_utf8()`](../win32-error.c) from
  [`cfg.c`](../cfg.c).
- Win32 IPC cleanup paths now use [`win32_unlink_utf8()`](../win32-error.c)
  from [`win32-ipc.c`](../win32-ipc.c).
- Console writes in [`win32-event.c`](../win32-event.c) now reject invalid
  UTF-8 instead of silently falling back to permissive conversion.
- The helper layer no longer relies on fixed `MAX_PATH` module-path buffers.

Remaining boundary issues:

- `file.c` still has non-Win32 `fopen()` / `open()` branches in shared code,
  and the Win32 path should be re-audited to confirm there is no remaining
  fallback to narrow CRT semantics for Windows-facing operations.
- Some Win32 runtime knob lookups in [`client.c`](../client.c) still read the
  process environment directly via `getenv()`. That is acceptable only if we
  intentionally treat them as process-local diagnostics rather than tmux
  canonical environment state.
- `utf8.c` still exposes `utf8_towc()` / `utf8_fromwc()` even though Windows
  `wchar_t` is UTF-16 code-unit sized, not a stable internal scalar type.
- Validation coverage is still incomplete for some Unicode file and cwd cases.

## Implementation Stages

### Stage 1: Make the boundary explicit

- Define the Win32 UTF-8 / wide-char boundary in code comments and helper
  names.
- Separate strict conversion helpers from any display-only or compatibility
  fallback helpers.
- Stop treating `wchar_t` as a general-purpose internal tmux text type.

### Stage 2: Fix environment ingress and egress

- Done: replace the `_environ` startup import with a wide environment import
  path.
- Done: normalize imported environment entries into tmux's UTF-8 environment
  store.
- Mostly done: replace direct Win32-facing `getenv()` uses with either
  `global_environ` lookups or explicit wide helpers.
- Done: replace `compat/setenv.c` / `unsetenv()` Win32 behavior with a wide
  boundary helper and keep tmux's UTF-8 environment state synchronized.
- Done: the client identify path sends normalized UTF-8 environment strings,
  not raw CRT snapshots.

### Stage 3: Close filesystem and path leaks

- Done: replace narrow config-file loading in `cfg.c` with a wide path helper.
- Remaining: re-audit `file.c` and remove any narrow fallback where a Windows
  path has already been normalized to UTF-16.
- Done: convert socket cleanup and managed-root cleanup in `win32-ipc.c` to
  wide path operations.
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
- Done: invalid UTF-8 at the console boundary is rejected rather than
  sanitized.
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

## Next Concrete Work

1. Re-audit [`file.c`](../file.c), [`status.c`](../status.c),
   [`popup.c`](../popup.c), and [`log.c`](../log.c) to document or eliminate
   any remaining Win32-facing narrow CRT path use.
2. Decide whether the remaining Win32-only `getenv()` reads in
   [`client.c`](../client.c) should stay process-local or move behind an
   explicit helper.
3. Add smoke coverage for Unicode file read/write paths and child startup from
   a Unicode cwd.
4. Decide whether `utf8_towc()` / `utf8_fromwc()` should be removed,
   Win32-scoped, or left as internal-only helpers with clearer documentation.
