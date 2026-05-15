# Win32 UTF-8 / Wide-Char Boundary Plan

Date: 2026-05-16

Status: Complete

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

The current tree now has the boundary helpers and the validation coverage
needed for this plan's scope.

Completed since this plan was drafted:

- Startup environment import now comes from `GetEnvironmentStringsW()` in
  [`win32-error.c`](../win32-error.c), not raw `_environ`.
- Win32 `setenv()` / `unsetenv()` now cross through wide helpers in
  [`compat/setenv.c`](../compat/setenv.c) and keep `global_environ`
  synchronized through [`win32-error.c`](../win32-error.c).
- Startup environment lookups now prefer canonical `global_environ` in
  [`tmux.c`](../tmux.c).
- Win32 client runtime knob and terminal metadata reads now use the explicit
  wide environment helper in [`client.c`](../client.c), not direct
  `getenv()` call sites.
- Unused public `wchar_t`-shaped UTF-8 helpers were removed from
  [`tmux.h`](../tmux.h) and [`utf8.c`](../utf8.c), so the core no longer
  advertises a misleading wide-character conversion boundary.
- Config-file loading now uses [`win32_fopen_utf8()`](../win32-error.c) from
  [`cfg.c`](../cfg.c).
- Win32 IPC cleanup paths now use [`win32_unlink_utf8()`](../win32-error.c)
  from [`win32-ipc.c`](../win32-ipc.c).
- Console writes in [`win32-event.c`](../win32-event.c) now reject invalid
  UTF-8 instead of silently falling back to permissive conversion.
- The helper layer no longer relies on fixed `MAX_PATH` module-path buffers.
- Win32 cwd discovery now comes from `GetCurrentDirectoryW()` through
  [`win32_getcwd_utf8()`](../win32-error.c), not narrow CRT `getcwd()`.
- [`tools/win32-utf8-boundary-smoke.ps1`](win32-utf8-boundary-smoke.ps1) now
  covers non-ASCII `HOME`, `USERPROFILE`, `SHELL`/`default-shell`,
  `VISUAL`, `EDITOR`, config paths, buffer file paths, Unicode child cwd
  startup, and Unicode `TMUX` reconnect.
- [`tools/win32-console-relay-smoke.ps1`](win32-console-relay-smoke.ps1) now
  covers both multibyte console-writer carry across output chunk boundaries
  and explicit invalid UTF-8 rejection at the native console writer boundary.

Boundary completion notes:

- The completion audit did not find a live Win32 environment or filesystem
  boundary that still depends on `_environ`, `fopen()`, `open()`, `unlink()`,
  or `access()` for OS-facing state.
- Remaining Win32 issues such as shell basename handling, path-list separators,
  slash-root policy, and command quoting are tracked separately in
  [`tools/win32-port-findings.md`](win32-port-findings.md). They are Windows
  path-policy problems, not remaining UTF-8 / wide-char boundary leaks.

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
- Done: replace direct Win32-facing `getenv()` uses with either
  `global_environ` lookups or explicit wide helpers.
- Done: replace `compat/setenv.c` / `unsetenv()` Win32 behavior with a wide
  boundary helper and keep tmux's UTF-8 environment state synchronized.
- Done: the client identify path sends normalized UTF-8 environment strings,
  not raw CRT snapshots.

### Stage 3: Close filesystem and path leaks

- Done: replace narrow config-file loading in `cfg.c` with a wide path helper.
- Done: re-audit `file.c`; the active Win32 read/write paths stay on
  helper-backed or handle-backed file I/O rather than falling back to narrow
  CRT opens for normalized filesystem paths.
- Done: convert socket cleanup and managed-root cleanup in `win32-ipc.c` to
  wide path operations.
- Done: audit `log.c`, `popup.c`, `status.c`, `server.c`, and `client.c`;
  the active Win32-facing file and cleanup paths already use wide helpers,
  `CreateFileW()`, or the Win32 file service.
- Done: remove `MAX_PATH`-limited assumptions from the helper layer where they
  affected Win32 module-path and cwd boundaries.

### Stage 4: Harden process and shell boundaries

- Done: shell validation is Windows-aware through
  [`win32_access_utf8()`](../win32-error.c) in [`tmux.c`](../tmux.c).
- Done: `default-shell` and related path checks are now validated in the same
  encoding used for process creation, with Unicode `SHELL` coverage in
  [`tools/win32-utf8-boundary-smoke.ps1`](win32-utf8-boundary-smoke.ps1).
- Done: `CreateProcessW()` remains the only process creation boundary.
- Done: child environment building preserves valid UTF-8 entries and exports
  them through the wide helper layer.

### Stage 5: Make console conversion policy consistent

- Keep tmux core text UTF-8.
- Keep the real console writer as the only UTF-16 text sink.
- Done: invalid UTF-8 at the console boundary is rejected rather than
  sanitized.
- Done: multibyte output carry across console writer chunk boundaries is
  validated explicitly.
- Avoid using console code page changes as part of correctness.

### Stage 6: Validation

- Add or update tests for:
  - Done: non-ASCII `HOME`, `USERPROFILE`, `SHELL`, `EDITOR`, `VISUAL`, and
    `TMUX`.
  - Done: non-ASCII config paths.
  - Done: non-ASCII socket and IPC paths through Unicode `TMUX` reconnect.
  - Done: Unicode file read/write paths are covered by
    [`tools/win32-utf8-boundary-smoke.ps1`](win32-utf8-boundary-smoke.ps1).
  - Done: child process startup from a Unicode cwd is covered by
    [`tools/win32-utf8-boundary-smoke.ps1`](win32-utf8-boundary-smoke.ps1).
  - Done: console output that crosses multibyte boundaries through
    [`tools/win32-console-relay-smoke.ps1 -ExerciseUtf8Split`](win32-console-relay-smoke.ps1).
  - Done: invalid UTF-8 behavior at the console writer boundary through
    [`tools/win32-console-relay-smoke.ps1 -ExerciseInvalidUtf8`](win32-console-relay-smoke.ps1).
- Done: verify that the Win32 path no longer depends on raw `_environ`,
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

## Follow-Up Work

1. Keep new Win32 environment, filesystem, and process-boundary code aligned
   with this contract: UTF-8 inside tmux, `wchar_t` only at the Win32 edge.
2. Track remaining Windows path semantics, quoting, and socket-path policy in
   [`tools/win32-port-findings.md`](win32-port-findings.md). Those are outside
   this plan's completed boundary scope.
