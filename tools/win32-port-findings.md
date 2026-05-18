# Win32 Port Findings

Date: 2026-05-17

Scope: current codebase-wide Win32 implementation review after the AF_UNIX
auth work landed. This document reflects the current tree, not earlier design
states. It is a findings document only; it does not imply source changes.

Current baseline:

- AF_UNIX auth is implemented in the current tree: protocol `17`,
  `MSG_WIN32_AUTH_CHALLENGE` / `MSG_WIN32_AUTH_BIND` /
  `MSG_WIN32_AUTH_RESULT`, same-user SID admission, integrity-floor checks,
  and direct handle duplication bound to the authenticated peer process.
- The current Win32 build links and passes native `start-server` /
  `list-commands` smoke from PowerShell. Fresh `-vv` traces show auth
  completion and post-auth handle duplication.

Review method:

- Local source review of the current Win32 IPC/auth, terminal, ConPTY/job,
  filesystem/path, and command-launch paths.
- Every active finding below was rechecked against the current tree before
  this document was updated.
- Stale findings from earlier audits were either retired explicitly or removed
  when a narrower replacement finding was more accurate.

## Executive Summary

The biggest stale claim from earlier Win32 audits is now wrong: Win32 client
authorization is no longer "missing". The port now has a real AF_UNIX
challenge/bind auth path, and direct handle duplication is tied to the
authenticated client process rather than a claimed PID.

The remaining problems are the second-order gaps around that progress:

1. Win32 auth is not yet integrated into tmux's ACL and `server-access`
   semantics, so authenticated same-user clients still bypass the Unix-shaped
   read-only and deny model.
2. Native Windows command launch still has gaps around popup editor argv
   handling and broader `cmd.exe` edge-case coverage.
3. Relay is now explicitly a first-class transport for native console clients,
   not just a fallback. Detached server-side native-console handle I/O still
   does not work.

## Active Priority Findings

### P1: Win32 authenticated peers are not integrated into tmux ACL semantics

Files:

- [`proc.c`](../proc.c): `proc_add_peer()` still sets Win32 peer UID to
  `(uid_t)-1`.
- [`server-acl.c`](../server-acl.c): `server_acl_join()` returns success
  unconditionally on Win32.
- [`cmd-server-access.c`](../cmd-server-access.c): mutating `server-access` is
  still rejected on Win32.
- [`server-client.c`](../server-client.c): Win32 auth now finishes before
  `server_acl_join()`, but the ACL layer has no Win32 identity model to use.

Problem:

The new Win32 AF_UNIX auth path produces a real peer identity, but that
identity lives in `c->win32_peer` rather than in the generic `struct tmuxpeer`
/ ACL model. As a result, `server-access -a/-d/-r/-w` still has no functional
Win32 equivalent, `proc_get_peer_uid()` remains unusable on Win32, and
authenticated same-user clients always get full access.

Why it matters:

tmux now has Win32 admission control, but it still does not have Win32 ACL
semantics. The gap is no longer "any local process can attach"; the gap is
"all authenticated same-user clients are treated the same, and the
read-only/deny management surface is effectively absent on Windows."

Required direction:

- Decide whether Win32 should map ACLs to SID-based identities, logon-session
  identities, or a narrower same-user policy.
- Either plumb authenticated Win32 peer identity into `struct tmuxpeer` /
  `server_acl_*`, or add a dedicated Win32 ACL implementation with equivalent
  tmux behavior.
- Define what `server-access -r/-w` should mean for same-user multi-logon
  attaches.

### P1: Popup editor launch remains argv-fragile and broader `cmd.exe` coverage is still thin

Files:

- [`popup.c`](../popup.c): `popup_editor_open()` still builds a shell command
  with `"%s %s"`.
- [`win32-conpty.c`](../win32-conpty.c): the central pane/job shell-command
  path now resolves a real Win32 `cmd.exe` default and has native smoke for
  quoted-path pane and `run-shell` cases, but the remaining `cmd.exe` matrix is
  still incomplete.

Problem:

The central Win32 shell-command path is no longer falling back to `/bin/sh`:
tmux now resolves `COMSPEC` or system `cmd.exe`, and native smoke covers quoted
helper paths, spaces, and `&` through both pane and `run-shell` one-argument
command paths. The remaining issue is that popup editor launch still
concatenates editor and path text into a single command string instead of
treating it as argv, and the checked-in `cmd.exe` coverage still does not span
all of the edge characters we care about.

Why it matters:

Popup editing is one of the easiest user-facing places to hit argument-joining
bugs, and `cmd.exe` regressions tend to hide in edge combinations until a real
user trips them.

Required direction:

- Treat popup editor launch as an argv construction problem.
- Keep extending native `cmd.exe` coverage beyond the new quoted-path baseline
  so future changes do not silently regress `|`, `^`, parentheses, or popup
  entry points.
- Keep shell-specific escaping rules explicit rather than relying on generic
  quote wrapping.

### P2: Relay is first-class because detached server-side native console handles still do not work

Files:

- [`client.c`](../client.c): native console clients still choose the relay
  transport; direct handle mode is for non-console stdio or diagnostics.
- [`win32-proc.c`](../win32-proc.c): the server still runs detached.
- [`tools/win32-server-side-handle-io-plan.md`](win32-server-side-handle-io-plan.md):
  records the direct-handle migration constraints.
- [`tools/win32-console-direct-probe.ps1`](win32-console-direct-probe.ps1):
  is the native-console probe entry point.

Problem:

Duplicated native console handles are transferable, but the detached server
still cannot use them for native console I/O. That means server-side handle
mode is useful for redirected or non-console stdio, but not for ordinary
PowerShell / `cmd.exe` / Windows Terminal native-console clients.

Why it matters:

Relay is not a fallback anymore; it is one of the primary supported product
modes. Any design that treats relay as temporary must prove detached
server-side usability of native console handles first.

Required direction:

- Keep relay explicitly first-class for native console clients.
- Keep direct handle mode as the supported path for non-console stdio.
- Treat future relay replacement as gated on usable detached-server console
  handles, not merely on handle transfer.

### P3: Win32 glob and case folding remain incomplete

Files:

- [`win32-glob.c`](../win32-glob.c): `fnmatch()` still uses byte-wise
  `tolower()` for `FNM_CASEFOLD`.
- [`win32-glob.c`](../win32-glob.c): globbing still delegates much of the
  matching shape to `FindFirstFileW`.
- [`cmd-source-file.c`](../cmd-source-file.c): `source-file` consumes globbed
  paths.

Problem:

Case folding is still ASCII-only over bytes, and the Win32 glob path still
does not implement full tmux/POSIX matching semantics for Unicode names,
bracket expressions, escapes, UNC paths, or slash/backslash normalization.

Why it matters:

Globbed path consumers can still behave differently on Win32 than on Unix,
especially for non-ASCII filenames and more structured patterns.

Required direction:

- Enumerate entries with `FindFirstFileW`, but apply matching with a deliberate
  tmux/POSIX policy.
- Add tests for `*`, `?`, bracket expressions, escapes, drive paths, UNC
  paths, Unicode names, and mixed slash/backslash input.

### P3: ConPTY resize failures are still ignored

Files:

- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_resize()` ignores the
  `ResizePseudoConsole()` `HRESULT`.
- [`win32-conpty.c`](../win32-conpty.c): `win32_job_resize()` also ignores the
  `ResizePseudoConsole()` `HRESULT`.
- [`window.c`](../window.c): pane size state is updated regardless.
- [`job.c`](../job.c): job resize requests also assume success.

Problem:

tmux still updates logical size state and calls `ResizePseudoConsole()`
without checking whether the resize succeeded.

Why it matters:

If the ConPTY is tearing down or rejects the resize, tmux and the child can
silently diverge on size, which makes rendering-edge bugs harder to diagnose.

Required direction:

- Check and log `ResizePseudoConsole()` failures with pane/job identity.
- Decide whether failures should be ignored, retried, or treated as teardown
  signals.

## Retired Findings

The following earlier findings were verified as fixed or superseded by the
current implementation:

- Win32 client auth is no longer "missing". The current tree has AF_UNIX
  challenge/bind auth, same-user SID admission, integrity-floor checks, and
  direct handle duplication bound to the authenticated peer process.
- The earlier `SIO_AF_UNIX_GETPEERPID` direction was dropped and is no longer
  part of the active design.
- Managed socket-root failure no longer falls back to `C:/Temp`.
- The tree now has a shared `ipc-startup.c` service that owns default socket
  resolution, endpoint canonicalization, connect-or-start sequencing, stale
  probing, listener creation retries, and backend startup handoff behind one
  abstraction. Native PowerShell validation now covers default `-L` startup,
  relative/absolute `-S` aliasing, case-variant `-S` attach, stale detached
  autostart, stale foreground `-D`, concurrent autostart, concurrent
  `start-server`, `-D` versus client races, and `.lock` cleanup.
- Explicit `-S` and inherited `$TMUX` startup mutation on Win32 is no longer
  unchecked. The current tree validates the parent chain before directory
  creation, `.lock` acquisition, stale cleanup, or bind recovery, and native
  PowerShell smoke now covers safe inherited startup plus unsafe explicit
  rejection without parent creation.
- Win32 startup coordination no longer depends on sibling `.lock` files beside
  explicit endpoints. Startup guards now live in a backend-owned shared root
  under `LOCALAPPDATA`, keyed by endpoint comparison identity, and native
  validation confirms explicit paths no longer leave adjacent `<socket>.lock`
  files behind.
- Win32 detached startup no longer leaves stale `.lock` guard files behind
  after successful startup or shutdown.
- The old polling-based child-exit path is gone. Win32 process exit now comes
  from process events and job checks.
- Win32 pane/job lifecycle no longer collapses passive cleanup, explicit
  termination, process exit, drained output, and `pipe-pane` helper ownership
  into one close path. The current tree has distinct cleanup versus terminate
  operations, Win32 quiescence predicates, `pipe-pane` ownership gates, and
  native PowerShell smoke for dead-pane respawn, remain-on-exit, explicit
  kill, explicit pipe close, natural job completion, and `job_kill_all`.
- Worker-backed I/O service notifications are coalesced, bounded, and no
  longer lose wakeups in the previously observed ways.
- Terminal relay now has input credit, incremental output progress, explicit
  output abort, and explicit transport-loss handling.
- The relay UTF-8 writer boundary now carries multibyte splits incrementally
  and fails closed on invalid UTF-8 instead of silently pushing malformed data
  into the native console path.
- Recent IPC path work removed ANSI WinAPI calls from the socket-root security
  and directory-creation path.
- The wide/UTF-8 boundary work now covers environment import, cwd discovery,
  module path discovery, active config/history/buffer/log paths, and process
  creation entry points. The remaining issues are now command quoting, glob
  semantics, and regression-coverage gaps rather than narrow-CRT boundary
  leaks.
- The earlier broad "Win32 path policy is still inconsistent across tmux"
  finding is no longer accurate for the concrete gaps it named. The current
  tree now shares rooted-path handling, `~\` and `$VAR\` expansion,
  basename/dirname separator handling, drive-relative rejection, and
  drive-safe path-list parsing across the main filesystem consumers.

## Missing Tests

Tracked native smoke coverage now exists for:

- IPC startup, stale recovery, and startup races:
  `tools/win32-ipc-startup-smoke.ps1`
- non-console direct-handle I/O:
  `tools/win32-direct-handle-smoke.ps1`
- native-console relay:
  `tools/win32-console-relay-smoke.ps1`
- native-console direct-handle diagnostics:
  `tools/win32-console-direct-probe.ps1`
- UTF-8 / wide-char boundary:
  `tools/win32-utf8-boundary-smoke.ps1`
- Win32 shell-command default-shell, pane, and `run-shell` launch:
  `tools/win32-shell-command-smoke.ps1`

The main gaps that still matter are:

1. Auth admission matrix:
   same Windows user across multiple logon sessions attaches successfully;
   different-user and lower-integrity clients are rejected.

2. Auth plus non-console direct-handle coverage:
   redirected stdio attach/detach still works after auth, and reject paths are
   exercised explicitly.

3. Custom socket path policy:
   validated user-owned `-S` parents, reparse-point parents, inherited `$TMUX`
   paths, aliased paths, and the backend-private startup coordination path all
   behave according to the chosen Win32 policy.

4. Startup races:
   concurrent normal starts, concurrent `tmux -D` starts, and aliased socket
   paths produce one live server and never remove a live endpoint.

5. Relay-first native-console coverage:
   reader loss, output loss, resize-under-backlog, input credit, detach under
   backlog, UTF-8 split, invalid UTF-8, and transport loss all need continued
   native-host validation.

6. Popup PTY jobs:
   popup PTY job completion and replacement still need native coverage so the
   job-tree drain path is exercised outside ordinary panes.

7. `pipe-pane` lifecycle:
   ordinary pane death with Win32 `pipe-pane -O`, `-I`, and `-IO` helpers
   still needs native coverage; current smoke only covers explicit pipe close.

9. Command quoting:
   extend the new `tools/win32-shell-command-smoke.ps1` baseline to popup
   editors and additional `cmd.exe` texts containing quotes, `|`, `^`, and
   parentheses.

10. Win32 path-policy regression matrix:
    keep verifying `/foo`, `\foo`, drive-qualified paths, UNC paths, `~/...`,
    `~\...`, and drive-letter path lists so future changes do not regress the
    now-shared Win32 path grammar.

11. Long and Unicode path shapes:
    start tmux from long and non-ASCII cwd / executable / config paths and
    verify spawn, config lookup, logs, `PWD`, and reconnect behavior.

12. Glob semantics:
    test `*`, `?`, `[abc]`, escapes, drive paths, UNC paths, Unicode names,
    and mixed slash/backslash input for `source-file`.

## Recommended Implementation Order

1. Win32 ACL and socket boundary cleanup:
   integrate authenticated peers with `server-access` semantics, then decide
   whether explicit socket coordination should stay adjacent to the endpoint or
   move into a backend-private namespace.

2. Native command and remaining path-surface cleanup:
   fix `cmd.exe` quoting, popup editor argv handling, Win32 glob semantics,
   and add broader regression coverage for the shared path policy.

3. Relay-first terminal consolidation:
   keep relay as the supported native-console mode, continue native coverage,
   and only revisit server-side native-console handle mode if detached-server
   usability is proven.

4. Broader native regression coverage:
   keep runtime verification in native PowerShell or equivalent Windows hosts,
   not under MSYS2 tmux execution.
