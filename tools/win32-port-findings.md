# Win32 Port Findings

Date: 2026-05-22

Scope: current codebase-wide Win32 implementation review after the AF_UNIX
auth work landed. This document reflects the current tree, not earlier design
states. It is a findings document only; it does not imply source changes.

Current baseline:

- AF_UNIX auth is implemented in the current tree: protocol `17`,
  `MSG_WIN32_AUTH_CHALLENGE` / `MSG_WIN32_AUTH_BIND` /
  `MSG_WIN32_AUTH_RESULT`, same-user SID admission, integrity-floor checks,
  and direct handle duplication bound to the authenticated peer process.
- Win32 ACL now consumes authenticated SIDs for peer admission:
  `struct tmuxpeer` stores the authenticated SID, `server_acl_join()` is
  SID-keyed on Win32, `server_acl_init()` seeds the ACL from the server SID,
  and `server-access -l` lists the real Win32 principal.
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

The biggest stale claims from earlier Win32 audits are now wrong: Win32 client
authorization is no longer "missing", and authenticated peers are no longer
disconnected from tmux ACL semantics. The port now has a real AF_UNIX
challenge/bind auth path, direct handle duplication tied to the authenticated
client process, and SID-keyed ACL admission for Win32 peers.

The remaining problems are the second-order gaps around that progress:

1. Native Windows command launch now covers popup editor argv handling and
   generic popup PTY argv execution, but broader `cmd.exe` edge-case coverage
   still needs expansion.
2. Relay is now explicitly a first-class transport for native console clients,
   not just a fallback. Detached server-side native-console handle I/O still
   does not work.
3. Several Win32 filesystem and regression-coverage edges remain incomplete.
   Basic `source-file` glob expansion now has tmux-side segment matching, but
   glob escape policy and native Windows path coverage still need more work.

## Active Priority Findings

### P1: Relay is first-class because detached server-side native console handles still do not work

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

### P2: Win32 glob escape policy and path matrix coverage remain incomplete

Files:

- [`win32-glob.c`](../win32-glob.c): `fnmatch()` now matches UTF-8 input via
  the Win32 wide-character boundary, and `glob()` now enumerates directories
  and applies tmux-side segment matching instead of handing the full pattern to
  `FindFirstFileW`.
- [`cmd-source-file.c`](../cmd-source-file.c): `source-file` consumes globbed
  paths.
- [`tools/win32-glob-smoke.ps1`](win32-glob-smoke.ps1): covers bracket
  expressions, wildcard directory components, ordinary case-insensitive
  matching, mixed slash/backslash input, and a Unicode filename baseline.

Problem:

The raw Windows wildcard delegation problem is fixed for the covered
`source-file` cases, but the port still does not have a complete product policy
for glob escaping with native backslash paths, UNC path edge cases, literal
glob metacharacters in filenames, POSIX character classes/collation, or
case-sensitive Windows directory configurations.

Why it matters:

Globbed path consumers can still behave differently on Win32 than on Unix at
the edges where Windows path syntax and POSIX glob escape syntax overlap.

Required direction:

- Decide and document how `source-file` should distinguish a native backslash
  path separator from a glob escape before normalizing the path for `glob()`.
- Expand native PowerShell coverage for escapes, literal `[` / `]` filenames,
  drive paths, UNC paths, long paths, and any case-sensitive directory
  configurations this port needs to support.

## Retired Findings

The following earlier findings were verified as fixed or superseded by the
current implementation:

- Win32 client auth is no longer "missing". The current tree has AF_UNIX
  challenge/bind auth, same-user SID admission, integrity-floor checks, and
  direct handle duplication bound to the authenticated peer process.
- Win32 authenticated peers are now integrated into tmux ACL semantics.
  `struct tmuxpeer` stores the authenticated SID on Win32,
  `server_acl_join()` now consults the Win32 ACL tree, `server_acl_init()`
  seeds that tree from the server SID, and `server-access -l` lists the
  actual SID principal. `server-access` mutation remains intentionally
  disabled on Windows for now because all same-user attaches share the same
  SID principal.
- ConPTY resize failures are no longer ignored. The current tree validates
  pseudoconsole sizes before narrowing them into `COORD`, rejects impossible
  sizes during pane/job creation, skips resize after exit, and logs
  `ResizePseudoConsole()` failures with pane/job identity.
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
- Popup editor launch on Win32 no longer concatenates `editor` and the temp
  path into one shell string. The current tree splits the editor command line
  into argv, appends the temp file path explicitly, and native PowerShell
  smoke now covers both generic popup argv execution and the real
  `choose-buffer` editor flow.
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
- Win32 shell-command, popup argv, and popup editor launch:
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
   a redirected-runner launcher now exists for baseline attach, input credit,
   detach under backlog, UTF-8 split, invalid UTF-8, output loss, and
   transport loss, but resize-under-backlog and output-progress remain
   host-sensitive and still need continued native-host validation.

6. Real relay mouse generation:
   the relay mouse path is now intentionally direct `ReadFile()` VT input,
   so synthetic `WriteConsoleInputW()` mouse-record injection is no longer an
   authoritative test. A desktop-input harness or equivalent real-console
   validation is still needed to prove that supported native console hosts
   produce VT mouse bytes for actual mouse interaction.

7. `pipe-pane` lifecycle:
   ordinary pane death with Win32 `pipe-pane -O`, `-I`, and `-IO` helpers
   still needs native coverage; current smoke only covers explicit pipe close.

8. ConPTY resize rejection path:
   injected or teardown-time `ResizePseudoConsole()` failure still needs
   targeted validation so the new logging and guard behavior stays correct.

9. Command quoting:
   keep broadening `cmd.exe` texts containing quotes, `|`, `^`, and
   parentheses beyond the popup editor and popup PTY argv baseline.

10. Win32 path-policy regression matrix:
    keep verifying `/foo`, `\foo`, drive-qualified paths, UNC paths, `~/...`,
    `~\...`, and drive-letter path lists so future changes do not regress the
    now-shared Win32 path grammar.

11. Long and Unicode path shapes:
    start tmux from long and non-ASCII cwd / executable / config paths and
    verify spawn, config lookup, logs, `PWD`, and reconnect behavior.

12. Glob semantics:
    baseline coverage now exists for `*`, `?`-style segment matching through
    bracket/wildcard cases, wildcard directory components, ordinary
    case-insensitive matching, mixed slash/backslash input, and Unicode
    filenames through `tools/win32-glob-smoke.ps1`. Remaining coverage should
    target escapes, literal glob metacharacter filenames, drive paths, UNC
    paths, long paths, and case-sensitive directories.

## Recommended Implementation Order

1. Win32 ACL and socket boundary cleanup:
   integrate authenticated peers with `server-access` semantics, then decide
   whether explicit socket coordination should stay adjacent to the endpoint or
   move into a backend-private namespace.

2. Native command and remaining path-surface cleanup:
   broaden `cmd.exe` coverage, Win32 glob semantics, and regression coverage
   for the shared path policy.

3. Relay-first terminal consolidation:
   keep relay as the supported native-console mode, continue native coverage,
   and only revisit server-side native-console handle mode if detached-server
   usability is proven.

4. Broader native regression coverage:
   keep runtime verification in native PowerShell or equivalent Windows hosts,
   not under MSYS2 tmux execution.
