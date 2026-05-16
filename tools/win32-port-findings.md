# Win32 Port Findings

Date: 2026-05-16

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
2. Socket-path policy is still weak for explicit `-S` and inherited `$TMUX`
   paths, and startup control flow is still not fully unified across detached
   helper and foreground cases.
3. Native Windows command/path policy is still inconsistent with tmux's
   Unix-shaped assumptions, especially around `cmd.exe` quoting, `/foo` vs
   `\foo`, `~\`, drive-letter path lists, and basename parsing.
4. Pane/job lifecycle still conflates passive cleanup, forced termination,
   helper-job teardown, and the distinction between process exit and drained
   output EOF.
5. Relay is now explicitly a first-class transport for native console clients,
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

### P1: Custom `-S` and inherited `$TMUX` socket paths are still weak on Win32

Files:

- [`tmux.c`](../tmux.c): only default `-L` sockets go through
  `make_label()` and `win32_ipc_ensure_socket_dir()`.
- [`server.c`](../server.c): `server_create_socket()` directly calls
  `win32_ipc_server_create(socket_path, ...)`.
- [`win32-ipc.c`](../win32-ipc.c): `win32_ipc_server_create()` ensures parent
  directories exist, but explicit paths do not get the managed-root security
  treatment.

Problem:

The managed default socket root under `LOCALAPPDATA` gets Win32 DACL / SACL
hardening. Explicit `-S` paths and inherited `$TMUX` paths still bypass that
policy. They are normalized only by slash conversion and trailing-slash trim,
not by real canonical path resolution or ownership checks.

Why it matters:

Now that auth is implemented, the risk is no longer "custom path means no
auth". The remaining risk is endpoint ownership confusion, path aliasing,
shared-directory deletion or replacement, and policy drift between the managed
default path and every explicit path. This is especially relevant for startup
races, stale endpoint cleanup, and same-user / higher-integrity local
processes.

Required direction:

- Decide a Win32 product policy for explicit socket paths:
  reject outside the managed root, or harden and validate them explicitly.
- Canonicalize explicit paths before hashing, binding, reconnecting, or
  treating them as trusted.
- If explicit custom paths remain supported, validate parent ownership,
  reparse-point behavior, and endpoint cleanup rules before bind/unlink.

### P1: `cmd.exe` command building and popup editor launch are still fragile

Files:

- [`win32-conpty.c`](../win32-conpty.c): `win32_build_shell_command()` embeds
  raw command text inside `cmd.exe /d /s /c "..."`.
- [`popup.c`](../popup.c): `popup_editor_open()` still builds a shell command
  with `"%s %s"`.

Problem:

The Win32 shell path still relies on naive string interpolation for
`cmd.exe`. Popup editor launch still concatenates editor and path text into a
single command string instead of treating it as argv.

Why it matters:

Commands containing quotes, `&`, `|`, `^`, parentheses, or paths under
`C:\Program Files` can execute with different structure than tmux intended or
can fail outright. Popup editing is one of the easiest user-facing places to
hit this.

Required direction:

- Treat popup editor launch as an argv construction problem.
- Either implement a deliberate `cmd.exe` quoting policy or avoid `cmd.exe`
  when argv structure is already known.
- Keep shell-specific escaping rules explicit rather than relying on generic
  quote wrapping.

### P1: Win32 pane and job close paths still default to hard termination

Files:

- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_close()` always calls
  `win32_child_kill()`.
- [`win32-conpty.c`](../win32-conpty.c): `win32_job_close()` also hard-kills
  the job/process tree.
- [`server-fn.c`](../server-fn.c): normal pane destroy paths still use the
  Win32 close helper.
- [`spawn.c`](../spawn.c): respawn paths still use the same Win32 close
  helper.

Problem:

The Win32 close helpers are still also kill helpers. Passive teardown,
remain-on-exit cleanup, respawn bookkeeping, helper-job cleanup, and explicit
kill operations are not clearly separated.

Why it matters:

That is stricter than tmux's normal lifecycle semantics. It makes it too easy
for "close because tmux bookkeeping is done" to become "terminate the whole
process tree now", including helper jobs or descendants that were not part of
an explicit kill request.

Required direction:

- Split Win32 passive disconnect/handle-close from explicit forced
  termination.
- Use hard kill only for explicit kill paths, irrecoverable cleanup failures,
  or timed fallback.
- Keep the semantics of pane close, job close, and helper cleanup separate.

### P1: Win32 pane destroy-readiness still ignores `pipe-pane` helper jobs

Files:

- [`cmd-pipe-pane.c`](../cmd-pipe-pane.c): Win32 `pipe-pane` uses
  `wp->pipe_job`.
- [`window.c`](../window.c): `window_pane_destroy_ready()` checks Win32 pane
  output state and `PANE_EXITED`, but not `wp->pipe_job`.
- [`window.c`](../window.c): `window_pane_close_pipe()` frees `wp->pipe_job`
  during teardown.

Problem:

Win32 `pipe-pane` helper ownership is no longer leaked, but destroy-readiness
still does not wait for the helper job. Pane teardown can still free the
helper job while it has pending work.

Why it matters:

`pipe-pane -O` output can still be truncated, and bidirectional helpers can
still be killed by pane teardown rather than by an explicit pipe close or
helper lifecycle decision.

Required direction:

- Add `wp->pipe_job` readiness to Win32 pane destroy checks.
- Decide whether ordinary pane destruction should wait for helper completion,
  drain helper output, or explicitly abandon helper work.
- Keep explicit forced close able to kill the helper intentionally.

### P2: Win32 still overloads process exit and output-drained EOF

Files:

- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_exit_cb()` sets
  `PANE_STATUSREADY` when the process exits.
- [`window.c`](../window.c): `window_pane_error_callback()` sets
  `PANE_EXITED` only when the output side reaches EOF/error.
- [`spawn.c`](../spawn.c): respawn logic still uses `PANE_EXITED` as the
  main "pane is really gone" test.

Problem:

The earlier output-loss bug was fixed by delaying `PANE_EXITED` until reader
EOF, but tmux still only has one public state bit for both "root process has
exited" and "output is fully drained".

Why it matters:

Commands that care about process liveness still see an exited pane as active
until EOF, while respawn and destroy logic cannot distinguish "process is
dead" from "process is dead and output is fully drained".

Required direction:

- Add a separate Win32-visible state for root-process exit.
- Keep a distinct output-drained / EOF-complete state.
- Teach respawn, destroy, and status/reporting paths which state they need.

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

### P2: Win32 path policy is still inconsistent across tmux

Files:

- [`tmux.c`](../tmux.c): `path_is_absolute()` accepts `/foo` but not single
  leading `\foo`.
- [`win32-error.c`](../win32-error.c): `win32_resolve_cwd()` rejects `/foo`
  as an invalid Win32 working directory.
- [`tmux.c`](../tmux.c): `expand_path()` only expands `~/`, not `~\`.
- [`cfg.c`](../cfg.c), [`file.c`](../file.c), [`status.c`](../status.c):
  home-style path handling still assumes `~/`.
- [`tmux.c`](../tmux.c): `areshell()` only strips `/`, not `\`.
- [`tmux.c`](../tmux.c): `expand_paths()` still splits on `:`, which collides
  with drive letters.
- [`win32-ipc.c`](../win32-ipc.c): socket-path normalization still differs
  from general cwd/path policy.

Problem:

Win32 path policy is still split across multiple incompatible assumptions:
Unix-style absolute `/foo`, Windows drive paths, UNC paths, `\foo`, `~/`,
`~\`, Unix basename splitting, and Unix path-list separators.

Why it matters:

Socket paths, cwd resolution, config lookup, recursion checks, path-list
parsing, and user-visible command behavior can disagree about what path the
user actually asked for.

Required direction:

- Pick one explicit Win32 policy for `/foo`, `\foo`, `~/`, and `~\`.
- Use basename logic that understands both `/` and `\`.
- Use a Win32-native path-list separator where path lists are meant to accept
  Windows drive paths.
- Keep socket-path canonicalization aligned with the general Win32 path policy.

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
- The tree now has a shared `ipc-startup.c` service that owns endpoint
  canonicalization, connect-or-start sequencing, stale probing, listener
  creation retries, and backend startup handoff behind one abstraction. Native
  PowerShell validation now covers baseline startup, relative/absolute `-S`
  aliasing, stale detached autostart, stale foreground `-D`, concurrent
  autostart, and `-D` versus client races.
- The old polling-based child-exit path is gone. Win32 process exit now comes
  from process events and job checks.
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
  creation entry points. The remaining issues are path-policy and quoting
  mismatches rather than narrow-CRT boundary leaks.

## Missing Tests

Tracked native smoke coverage now exists for:

- non-console direct-handle I/O:
  `tools/win32-direct-handle-smoke.ps1`
- native-console relay:
  `tools/win32-console-relay-smoke.ps1`
- native-console direct-handle diagnostics:
  `tools/win32-console-direct-probe.ps1`
- UTF-8 / wide-char boundary:
  `tools/win32-utf8-boundary-smoke.ps1`

The main gaps that still matter are:

1. Auth admission matrix:
   same Windows user across multiple logon sessions attaches successfully;
   different-user and lower-integrity clients are rejected.

2. Auth plus non-console direct-handle coverage:
   redirected stdio attach/detach still works after auth, and reject paths are
   exercised explicitly.

3. Custom socket path policy:
   weak/shared `-S` parents, reparse-point parents, inherited `$TMUX` paths,
   and aliased paths behave according to the chosen Win32 policy.

4. Startup races:
   concurrent normal starts, concurrent `tmux -D` starts, and aliased socket
   paths produce one live server and never remove a live endpoint.

5. Relay-first native-console coverage:
   reader loss, output loss, resize-under-backlog, input credit, detach under
   backlog, UTF-8 split, invalid UTF-8, and transport loss all need continued
   native-host validation.

6. Pane lifecycle:
   root process exits with delayed ConPTY tail output; verify tail output,
   dead-pane state, destroy readiness, and respawn semantics.

7. `pipe-pane` lifecycle:
   destroy panes while Win32 `pipe-pane -O`, `-I`, and `-IO` helpers still
   have work pending; verify no unintended truncation or helper kill.

8. Passive vs forced teardown:
   verify natural pane exit, remain-on-exit, kill-pane, respawn, and helper-job
   cleanup all use the intended Win32 close/kill mode.

9. Command quoting:
   run popup editors and shell commands containing spaces, quotes, `&`, `|`,
   `^`, and parentheses under `cmd.exe`.

10. Win32 path policy:
    verify `/foo`, `\foo`, drive-qualified paths, UNC paths, `~/...`, `~\...`,
    and drive-letter path lists all behave consistently.

11. Long and Unicode path shapes:
    start tmux from long and non-ASCII cwd / executable / config paths and
    verify spawn, config lookup, logs, `PWD`, and reconnect behavior.

12. Glob semantics:
    test `*`, `?`, `[abc]`, escapes, drive paths, UNC paths, Unicode names,
    and mixed slash/backslash input for `source-file`.

## Recommended Implementation Order

1. Win32 ACL and socket boundary cleanup:
   integrate authenticated peers with `server-access` semantics, then harden
   explicit socket-path policy, canonicalization, and stale unlink behavior.

2. Pane/job lifecycle cleanup:
   split passive close from forced kill, separate process-exit from
   output-drained state, and add `pipe-pane` helper readiness.

3. Native command/path cleanup:
   fix `cmd.exe` quoting, popup editor argv handling, and the remaining
   Unix-shaped Win32 path-policy mismatches.

4. Relay-first terminal consolidation:
   keep relay as the supported native-console mode, continue native coverage,
   and only revisit server-side native-console handle mode if detached-server
   usability is proven.

5. Broader native regression coverage:
   keep runtime verification in native PowerShell or equivalent Windows hosts,
   not under MSYS2 tmux execution.
