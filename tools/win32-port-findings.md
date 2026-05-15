# Win32 Port Findings

Date: 2026-05-15

Scope: codebase-wide Win32 implementation review of IPC/auth, terminal
relay, ConPTY/jobs, command execution, and filesystem/path compatibility. This
document combines parallel subsystem reviews with local source verification.
It is a findings document only; it does not imply source changes.

Review method:

- Four parallel agents reviewed IPC/auth/startup, terminal/client/console,
  ConPTY/job/lifecycle, and filesystem/path/environment areas.
- Every finding below was checked against the current tree before this
  document was updated.
- Stale findings from earlier audits are moved to "Retired Findings" rather
  than left in the active priority list.

## Executive Summary

The Win32 port has moved past the first-order I/O service problems: ConPTY
pane/job I/O, regular-file I/O, terminal output chunking, UTF-8 continuation,
bounded worker teardown, and notify coalescing all have real implementations
now. The remaining risks are mostly at boundaries where tmux still exposes a
Unix-shaped contract while the underlying object is native Windows:

1. IPC authorization still has no authenticated Win32 peer identity. Socket
   reachability is effectively the auth boundary.
2. Server startup has a normal-path lock, but foreground startup, path aliases,
   slash-root paths, and unconditional socket unlink still leave race and
   confusion windows.
3. Console terminal relay now has explicit input flow control, incremental
   output progress, and transport-loss semantics. Remaining relay work is
   mostly tuning and broader native coverage rather than missing protocol
   primitives.
4. Pane/job lifecycle still conflates process exit, output EOF, dead-pane
   state, passive cleanup, and forced termination.
5. Native path, quoting, long-path, and Unicode environment support remains
   uneven outside the already-migrated file I/O service paths.

## Active Priority Findings

### P0: Win32 client authorization is not real yet

Files:

- [`proc.c`](../proc.c): `proc_add_peer()` sets Win32 peer UID to
  `(uid_t)-1`.
- [`server-acl.c`](../server-acl.c): `server_acl_join()` accepts every Win32
  client.
- [`server.c`](../server.c): accepted clients rely on `server_acl_join()`.
- [`cmd-server-access.c`](../cmd-server-access.c): mutating `server-access`
  is disabled on Win32.

Problem:

Once a Win32 client connects to the AF_UNIX socket, the server trusts it. There
is no peer credential equivalent plumbed into `struct tmuxpeer`, and the ACL
join path is compiled to unconditional success.

Why it matters:

On Unix, socket path permissions and peer credentials are separate layers. On
Win32, the second layer is missing. Any local process that can connect to the
socket can act as a full tmux client, including command execution and terminal
identify messages. This also means `server-access` semantics are not
enforceable on Win32.

Required direction:

- Add a Win32 authenticated client identity before admitting the client.
- Preserve the desired product model: multiple logon sessions of the same
  Windows user should be attachable, including later SSH logons.
- Prefer a same-user SID based model with an authenticated server secret or
  challenge/response carried over AF_UNIX.
- Do not rely on untrusted client identify messages as the proof of identity.

### P0: Custom `-S` and `$TMUX` socket paths are not hardened

Files:

- [`tmux.c`](../tmux.c): default `-L` labels go through `make_label()`, but
  explicit `-S` and `$TMUX` paths bypass that flow.
- [`win32-ipc.c`](../win32-ipc.c): `win32_ipc_server_create()` only ensures
  the parent directory exists.
- [`server.c`](../server.c): `server_create_socket()` does not distinguish
  default managed sockets from explicit custom paths on Win32.

Problem:

The managed default socket directory gets a Win32 DACL and integrity policy via
`win32_ipc_ensure_socket_dir()`. Explicit `-S` paths and inherited `$TMUX`
paths do not get equivalent hardening. The server create path only creates the
parent directory and then binds the AF_UNIX endpoint.

Why it matters:

Because client authorization is currently missing, the socket path is the
practical trust boundary. A socket placed in a weak or shared directory gives
any process with directory/socket access a trusted tmux connection.

Required direction:

- Either reject arbitrary `-S` paths outside the managed root on Win32, or
  explicitly harden and validate the parent and socket endpoint.
- Fail closed if the parent is world-writable, owned by another user, or has a
  lower-integrity access path.
- Decide a policy for reparse points and junctions before treating an existing
  directory as trusted.

### P1: Win32 startup locking still has bypasses and aliasing

Files:

- [`client.c`](../client.c): `CLIENT_NOFORK` returns `server_start()` before
  taking `win32_ipc_startup_lock()`.
- [`win32-ipc.c`](../win32-ipc.c): the startup mutex is keyed from
  `win32_ipc_path_hash(path)`, which only folds slash direction and ASCII
  case.
- [`win32-ipc.c`](../win32-ipc.c): server creation still unconditionally
  `unlink()`s the normalized socket path before `bind()`.

Problem:

The normal autostart path now has a named mutex, but not every server creation
path goes through it. `tmux -D` / `CLIENT_NOFORK` bypasses the lock entirely.
The lock name is based on a raw path spelling rather than a canonical object
path. Equivalent paths with `.`/`..`, different drive casing, junctions, or UNC
aliases can get distinct mutexes. Server create still removes the socket path
before binding.

Why it matters:

Two foreground starts or two differently spelled paths can still race into
server creation for the same endpoint. The unconditional unlink means the loser
can remove an endpoint that another process expects to own.

Required direction:

- Centralize all Win32 server creation behind one locked helper, including
  `CLIENT_NOFORK`.
- Canonicalize socket paths before hashing, connecting, or creating. At
  minimum use `GetFullPathNameW()`; consider stronger final-object resolution
  if reparse-point aliases matter.
- Do stale-socket validation before unlinking and never remove a live server's
  endpoint.
- Bound `WaitForSingleObject(..., INFINITE)` or make waits observable in logs
  and diagnostics.

### P1: Shell command construction for `cmd.exe` is unsafe

Files:

- [`win32-conpty.c`](../win32-conpty.c): `win32_build_shell_command()` embeds
  raw command text inside `cmd.exe /d /s /c "..."`
- [`popup.c`](../popup.c): `popup_editor_open()` builds `editor path` with
  `"%s %s"` and no quoting.

Problem:

The `cmd.exe` path interpolates arbitrary command strings into a quoted
command line without implementing `cmd.exe` escaping rules. Popup editor
launch builds a shell command by concatenating the editor string and temp path.

Why it matters:

Common commands with embedded quotes, `&`, `|`, `^`, parentheses, or paths
under `C:\Program Files` can fail or execute with different structure than tmux
intended. Popup editing is especially fragile when the editor path contains
spaces or arguments.

Required direction:

- Avoid shell interpolation for structured launches where argv is known.
- For actual shell commands, implement a deliberate `cmd.exe` quoting/escaping
  policy or route through a documented shell-specific builder.
- Treat popup editor invocation as an argv problem, not as a raw command string
  problem.

### P1: Pane/job teardown still conflates passive cleanup and forced kill

Files:

- [`server-fn.c`](../server-fn.c): `server_destroy_pane()` calls
  `win32_pane_close()` before `remain-on-exit` handling.
- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_close()` always calls
  `win32_child_kill()`.
- [`win32-conpty.c`](../win32-conpty.c): `win32_child_kill()` uses
  `TerminateJobObject()` or `TerminateProcess()`.
- [`spawn.c`](../spawn.c): respawn uses the same close path.

Problem:

The Win32 "close pane" primitive is also a hard-kill primitive. Natural pane
destruction, dead-pane transition, remain-on-exit cleanup, and explicit
kill/respawn teardown all route through termination of the pane job/process.

Why it matters:

This is stricter than tmux's normal lifecycle semantics. A pane whose root
process has exited can still have descendants in the job object; passive
cleanup should not automatically mean "terminate the whole tree" unless the
operation is explicitly a kill or a timed fallback.

Required direction:

- Split Win32 pane teardown into passive disconnect/handle-close and forced
  termination paths.
- Use hard termination for explicit kill, failed cleanup, or timed fallback,
  not as the default bookkeeping close.
- Document how background descendants should behave after the root ConPTY
  process exits.

### P1: Win32 `pipe-pane` helper jobs are not part of destroy-readiness

Files:

- [`cmd-pipe-pane.c`](../cmd-pipe-pane.c): Win32 `pipe-pane` uses
  `wp->pipe_job`.
- [`window.c`](../window.c): `window_pane_destroy_ready()` checks ConPTY
  output and `PANE_EXITED`, but not `wp->pipe_job`.
- [`window.c`](../window.c): `window_pane_close_pipe()` frees `wp->pipe_job`
  during pane destruction.

Problem:

The Win32 pipe helper ownership leak was fixed, but destroy-readiness still
does not wait for the helper job. A pane that is otherwise ready to destroy can
call `window_pane_close_pipe()`, which frees the helper job and terminates its
Win32 process tree.

Why it matters:

`pipe-pane -O` output may be truncated, and `pipe-pane -I` or bidirectional
helpers may be killed as a side effect of pane teardown rather than completing
their own lifecycle. Unix has pipe fd drain checks before destruction; the
Win32 job-backed equivalent is incomplete.

Required direction:

- Add a Win32 destroy-readiness gate for `wp->pipe_job`.
- Decide whether normal pane destruction should wait for helper completion,
  drain queued helper output, or explicitly mark the helper as abandoned.
- Keep forced pane close able to kill the helper intentionally.

### P2: Pane process exit and output EOF need separate tmux-visible state

Files:

- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_exit_cb()` sets
  `PANE_STATUSREADY` but not `PANE_EXITED`.
- [`window.c`](../window.c): `window_pane_error_callback()` sets
  `PANE_EXITED` when the output side reaches EOF/error.
- [`window.c`](../window.c): `window_pane_exited()` reports liveness from
  `PANE_EXITED`.
- [`spawn.c`](../spawn.c): respawn rejects a pane while `PANE_EXITED` is not
  set unless forced.

Problem:

The current Win32 implementation deliberately delays `PANE_EXITED` until
ConPTY output EOF to avoid losing tail output. That avoids the earlier bug
where process exit could close output too early, but it overloads
`PANE_EXITED` with "root process exited and output is fully drained."

Why it matters:

Commands that ask whether the process is gone still see the pane as active
until output EOF. `respawn-pane` can report "still active" even after the root
process has exited, because the output reader has not yet proven EOF. Simply
setting `PANE_EXITED` on process exit is not enough because current respawn
code would then be allowed to close the Win32 pane and could reintroduce tail
output loss.

Required direction:

- Add separate Win32-visible state for root process exit and output-drained EOF.
- Teach respawn/destroy paths which state they require.
- Preserve the invariant that ConPTY output is not closed before the reader has
  reached EOF or an explicit forced close is requested.

### P2: Transferable native console handles cannot yet back detached server-side terminal I/O

Files:

- [`client.c`](../client.c): direct Win32 handle I/O is gated to non-console
  stdio handles; native console clients stay on the explicit relay transport.
- [`win32-proc.c`](../win32-proc.c): the server is spawned with
  `DETACHED_PROCESS`.
- [`win32-event.c`](../win32-event.c): detached server reads/writes through the
  handle workers.
- [`tools/win32-server-side-handle-io-plan.md`](win32-server-side-handle-io-plan.md):
  records the native-console limitation.

Problem:

Native console stdin/stdout handles can be duplicated from the client process,
but they are not usable from the detached server process. Testing showed
duplicated console stdin fails on server-side `ReadFile()` with
`ERROR_INVALID_HANDLE`; duplicated console stdout also fails when the server
writer attempts to write. Non-console stdio handles such as pipes can use the
direct handle path.

The tracked `tools/win32-console-direct-probe.ps1` probe reproduced the input
side of this failure from a real Windows PTY: forced direct mode duplicated
stdin/stdout and sent size identify without relay, then the detached server
failed direct console input with `ERROR_INVALID_HANDLE`.

The relay path itself was also exercised from the same PTY and completed
attach/detach successfully while logging `IDENTIFY_WIN32_TERMINAL` and the
relay terminal-transport debug line.

Why it matters:

Handle transfer alone is not enough to replace the client console relay for
native PowerShell, `cmd.exe`, Windows Terminal, or ConHost clients. Dropping
relay based only on "the client can hand over handles" would break the common
native-console attach path unless follow-up testing proves the detached server
can also use those duplicated handles for read, write, size, detach, reattach,
EOF, and close behavior.

Required direction:

- Keep native console clients on relay until direct duplicated console handles,
  a console attachment design, or a helper design proves the detached server
  can safely use their terminal I/O. Use `TMUX_WIN32_CONSOLE_RELAY=0` to
  disable relay for no-relay testing.
- Use `TMUX_WIN32_HANDLE_TTY=force` only as a diagnostic matrix probe for
  native console direct handles. It intentionally bypasses the console handle
  exclusion, initializes/restores console modes, sends direct `MSG_RESIZE`
  nudges from the console resize poller, and is not a supported product mode.
- Treat any future relay replacement as gated on usable handles, not merely
  transferable handles.
- If a future separate design proves that the detached server can use native
  console handles across the supported client matrix, reevaluate whether
  keeping two terminal transports still makes sense.
- Continue using direct server-side handles for non-console stdio paths that
  pass native smoke tests.

### P2: Slash-rooted and backslash-rooted path semantics are inconsistent

Files:

- [`tmux.c`](../tmux.c): `path_is_absolute()` accepts `/foo`, drive-rooted
  paths, and UNC paths, but not single-leading-backslash paths.
- [`win32-ipc.c`](../win32-ipc.c): socket paths translate `/` to `\` before
  calling AF_UNIX.
- [`win32-error.c`](../win32-error.c): `win32_resolve_cwd()` rejects `/foo` as
  an invalid Win32 working directory.
- [`file.c`](../file.c), [`cfg.c`](../cfg.c), [`status.c`](../status.c):
  home expansion recognizes `~/` but not `~\`.

Problem:

Win32 path handling has multiple policies. `/tmp/x` is absolute to
`path_is_absolute()` and accepted by socket path handling, but rejected as a
working directory. `/tmp/sock` becomes `\tmp\sock`, which is current-drive
rooted rather than a stable drive-qualified path. `\tmp\x` and `~\x` are common
Windows forms but are treated as relative or unexpanded by several callers.

Why it matters:

Socket locking, reconnection, cwd validation, config lookup, file paths, and
history paths can disagree about what path the user requested. This is a
source of both correctness bugs and security mistakes around socket paths.

Required direction:

- Pick a Win32 policy for `/foo`: reject consistently or translate in one
  documented way at process startup boundaries.
- Treat single-leading-backslash paths and `~\` deliberately.
- Canonicalize socket paths before hashing and before AF_UNIX bind/connect.

### P2: Long-path and cwd handling are still capped by `MAX_PATH`

Files:

- [`compat/win32-compat.h`](../compat/win32-compat.h): `PATH_MAX` is defined
  as `MAX_PATH`.
- [`tmux.c`](../tmux.c): `find_cwd()` uses `static char cwd[PATH_MAX]` and
  `getcwd()`.
- [`win32-proc.c`](../win32-proc.c): server respawn uses
  `wchar_t exe[MAX_PATH]` with `GetModuleFileNameW()`.
- [`win32-error.c`](../win32-error.c): bundled terminfo discovery uses the same
  fixed module path pattern.

Problem:

Several startup and path discovery flows still use fixed `MAX_PATH` buffers or
narrow CRT `getcwd()`.

Why it matters:

Long-path-enabled Windows installs can still fail to start the detached server,
fail to discover bundled terminfo, or silently fall back from the real current
directory to a home/default directory. Relative paths and `PWD` then resolve
incorrectly.

Required direction:

- Replace fixed module path buffers with growable `GetModuleFileNameW()` loops.
- Replace `find_cwd()` on Win32 with dynamic `GetCurrentDirectoryW()` plus
  UTF-8 conversion.
- Stop treating `PATH_MAX == MAX_PATH` as a general limit for tmux path logic.

### P2: Startup environment import is still ANSI

Files:

- [`win32-error.c`](../win32-error.c): `win32_refresh_environ()` assigns
  `environ = _environ`.
- [`tmux.c`](../tmux.c): startup copies that environment into
  `global_environ`.

Problem:

The initial environment is read from the CRT narrow environment instead of the
Unicode environment block.

Why it matters:

Non-ASCII values in `HOME`, `USERPROFILE`, `PATH`, `SHELL`, `PWD`, and user
variables can be corrupted before tmux stores them. This affects path lookup,
shell choice, config expansion, and child process environments.

Required direction:

- Import the process environment from `GetEnvironmentStringsW()`.
- Convert names and values to UTF-8 before populating tmux's environment.
- Keep variable-name lookup case-insensitive on Win32.

### P2: Native command/path helpers still have Windows parsing gaps

Files:

- [`tmux.c`](../tmux.c): `areshell()` strips only `/`, so
  `C:\path\tmux.exe` is not compared by basename.
- [`tmux.c`](../tmux.c): `expand_paths()` splits path lists on `:`, which
  conflicts with drive letters.
- [`file.c`](../file.c): non-regular path fallback still uses narrow CRT
  `open()` after `CreateFileW()` determines the handle is not a regular file.
- [`cfg.c`](../cfg.c), [`log.c`](../log.c), [`tty.c`](../tty.c): some local
  file paths still use CRT `fopen()`/`open()` outside the migrated file service
  flows.

Problem:

The main local file service paths now use `CreateFileW`, but not every path
boundary has been normalized. Shell validation, path-list parsing, logs, tty
logs, and non-regular file fallbacks still use Unix assumptions.

Why it matters:

Backslash shell paths can bypass tmux-recursion checks. Drive-letter paths can
be split as lists. Non-ASCII or special Windows paths can still fail outside
the migrated buffer/source/history file paths.

Required direction:

- Use basename logic that understands both `/` and `\`.
- Use `;` as the native path-list separator on Win32, while preserving any
  intentional Unix-shaped built-in defaults.
- Continue replacing narrow CRT path APIs at Win32 boundaries with wide helper
  wrappers or `CreateFileW`.

### P3: Win32 glob and case folding remain incomplete

Files:

- [`win32-glob.c`](../win32-glob.c): `fnmatch()` uses byte-wise `tolower()`
  for `FNM_CASEFOLD`.
- [`win32-glob.c`](../win32-glob.c): globbing delegates much of the matching
  shape to `FindFirstFileW`.
- [`cmd-source-file.c`](../cmd-source-file.c): `source-file` consumes globbed
  paths.

Problem:

Case folding is ASCII-only over bytes. POSIX glob semantics, especially
bracket expressions, escapes, Unicode names, UNC paths, and slash/backslash
normalization, are not fully implemented.

Why it matters:

`source-file` and other globbed path consumers can behave differently on Win32
than on Unix, especially with non-ASCII filenames and bracket patterns.

Required direction:

- Enumerate directory entries with `FindFirstFileW`.
- Apply tmux/POSIX matching deliberately in UTF-8 or wide-character form.
- Add tests for `*`, `?`, bracket expressions, escapes, drive paths, UNC paths,
  and mixed slash/backslash input.

### P3: ConPTY resize failures are ignored

Files:

- [`window.c`](../window.c): `window_pane_send_resize()` calls
  `win32_pane_resize()`.
- [`job.c`](../job.c): `job_resize()` calls `win32_job_resize()`.
- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_resize()` and
  `win32_job_resize()` ignore the `ResizePseudoConsole()` `HRESULT`.

Problem:

tmux updates pane/job size state and calls `ResizePseudoConsole()`, but the
result is not checked or logged.

Why it matters:

If the ConPTY is tearing down or rejects the resize, tmux and the child process
can silently diverge on size. That makes lifecycle-edge rendering bugs hard to
diagnose.

Required direction:

- Check the `HRESULT` and log failures with pane/job identity.
- Suppress, retry, or mark dead according to a clear policy when resize fails
  during teardown.

## Retired Findings

The following earlier findings were verified as fixed or superseded by the
current implementation:

- Managed socket root failure no longer falls back to `C:/Temp`.
  `win32_default_socket_dir()` now returns `NULL` if the managed root cannot be
  built. The remaining `C:/Temp` reference is the generic `_PATH_TMP` compat
  definition, not the default socket path.
- Normal Win32 autostart has a startup mutex. The active findings are the
  `CLIENT_NOFORK` bypass, noncanonical lock key, infinite wait, and stale
  unlink behavior.
- The old 100 ms child polling path was removed. Process exit now comes from
  Win32 process events and job checks.
- Win32 I/O service notifications are coalesced with `notify_pending`; a full
  notify socket no longer loses the readiness edge.
- Worker-backed handle event/writer frees use bounded waits and leak inactive
  endpoints rather than blocking the server indefinitely.
- Terminal output ACKs now happen after client-side writer drain, not when the
  IPC message is merely received.
- Native-console relay output loss now sends explicit
  `MSG_WIN32_TTY_OUTPUT_ABORT`, and relay close no longer waits forever for an
  ACK that cannot arrive. The tracked smoke entry point is
  `tools/win32-console-relay-smoke.ps1 -SimulateOutputLoss`.
- Terminal UTF-8 output decoding is incremental across client writer chunks.
- Win32 console relay input now has explicit `MSG_WIN32_TTY_INPUT_CREDIT`
  flow control. Input is staged locally, reserved against server-issued
  credit, and is not silently discarded on `proc_send()` hard failure.
- Win32 console relay output now reports incremental progress instead of
  waiting for full writer drain. Writer backends emit
  `WIN32_IO_EVENT_WRITE_PROGRESS`, relay/direct tty accounting consumes it
  incrementally, redraw deferral uses a threshold, and the tracked smoke entry
  point is `tools/win32-console-relay-smoke.ps1 -ExerciseOutputProgress`.
- Win32 console relay now has explicit
  `MSG_WIN32_TTY_TRANSPORT_LOST` handling. Server-side relay close no longer
  waits on progress after transport loss, late relay messages are ignored once
  loss is declared, and the Win32 client no longer waits on local relay output
  progress once transport loss is known. The tracked smoke entry point is
  `tools/win32-console-relay-smoke.ps1 -SimulateTransportLost`.
- Win32 terminal output scheduling no longer recurses synchronously through
  `tty_write_callback()`.
- Win32 `pipe-pane` helper ownership is no longer leaked on toggle-off or pane
  destruction; the active problem is now destroy-readiness/drain semantics.
- `pipe-pane -I` now checks pane write readiness and retries instead of
  blindly discarding input on saturation.
- Recent IPC path work removed ANSI WinAPI calls from the socket root security
  and directory creation path.

## Missing Tests

Tracked native smoke coverage now exists for the non-console direct-handle
path in `tools/win32-direct-handle-smoke.ps1`. It verifies direct stdin/stdout
handle identify, direct size identify, relay absence under
`TMUX_WIN32_CONSOLE_RELAY=0`, redirected attach/detach behavior, and sustained
direct-output stress. It also verifies that direct stdin EOF is a half-close
and does not kill output delivery, and that direct output handle loss does not
make the server unusable. The tests below remain the broader coverage needed
before treating the Win32 port as complete.

Manual native-console direct-handle probing is tracked in
`tools/win32-console-direct-probe.ps1`. It must be run from a real Windows
console because redirected Codex or CI stdio is not a native console client
shape. Current probe evidence shows duplicated console handles are
transferable but not usable for detached server-side input.

Native-console relay smoke remains the supported primary path for that client
shape. The tracked manual smoke entry point is
`tools/win32-console-relay-smoke.ps1`.

Tracked native-console relay output-loss smoke is now also available through
`tools/win32-console-relay-smoke.ps1 -SimulateOutputLoss`. It verifies that a
relay output failure sends an explicit abort and that attach exits promptly
instead of hanging for an impossible ACK.

Tracked native-console relay output-progress smoke is now also available
through `tools/win32-console-relay-smoke.ps1 -ExerciseOutputProgress`. It
verifies that sustained output produces multiple incremental relay progress
events, forces a status redraw while backlog is active, and records redraw
deferral plus redraw-release behavior rather than only waiting for a full
writer drain.

Tracked native-console relay input-credit smoke is now also available through
`tools/win32-console-relay-smoke.ps1 -ExerciseInputCredit`. It injects more
than the 64 KiB credit window through the real console input buffer, verifies a
client-side credit pause and resume, and checks that the server returns credit
repeatedly while attach still exits cleanly.

Tracked native-console relay transport-loss smoke is now also available
through `tools/win32-console-relay-smoke.ps1 -SimulateTransportLost`. It
verifies that a declared relay transport loss while output is already in
flight causes attach to exit promptly instead of waiting for impossible relay
completion.

Tracked native-console relay detach-backlog smoke is now also available
through `tools/win32-console-relay-smoke.ps1 -ExerciseDetachBacklog`. It
detaches while relay output is still pending, verifies that the server logs the
relay close-pending state, and checks that detach still completes cleanly.

High-priority native PowerShell tests:

1. Auth admission:
   same Windows user across multiple logon sessions can attach; different user
   and lower-integrity clients are rejected.

2. Custom socket security:
   `-S` under weak/shared directories is rejected or hardened according to the
   chosen policy.

3. Startup races:
   concurrent normal clients, concurrent `tmux -D` clients, and aliased socket
   paths produce only one server and never unlink a live endpoint.

4. Slash/root path policy:
   verify `/tmp/sock`, `\tmp\sock`, drive-qualified paths, UNC paths, and
   `~\...` expansion behave consistently.

5. Lost console output:
   close or invalidate the client console while output bytes are pending and
   verify the server completes exit instead of waiting forever.

6. Input credit tuning:
   extend the current input-credit smoke with stronger peak-buffer or process
   memory checks so bounded relay memory is measured directly, not only
   inferred from the credit-window pause/resume cycle.

7. Output responsiveness:
   extend the current output-progress coverage across more output patterns and
   threshold sizes so redraw responsiveness is tuned broadly, not only
   demonstrated on the current stress case.

8. Native reader failure:
   reproduce an actual console-input loss without the test knob and verify it
   follows the explicit transport-lost path already covered by simulated
   smoke.

9. Handle relay:
   attach from a non-console Win32 frontend only after authenticated handle
   transfer is implemented.

10. Pane lifecycle:
   root process exits with delayed ConPTY tail output; verify tail output is
   preserved, dead-pane state is correct, and respawn behavior matches policy.

11. `pipe-pane` lifecycle:
    destroy panes while `pipe-pane -O`, `-I`, and `-IO` helpers still have
    pending data; verify no unintended truncation or helper leak.

12. Passive vs forced teardown:
    verify natural pane exit, remain-on-exit, kill-pane, and respawn use the
    intended Win32 close/kill mode.

13. Command quoting:
    run popup editors and shell commands with spaces, quotes, `&`, `|`, `^`,
    and parentheses under `cmd.exe`.

14. Long paths:
    start tmux from a long cwd and long executable path; verify server spawn,
    terminfo discovery, `PWD`, and relative paths.

15. Unicode environment and filesystem:
    launch with non-ASCII environment values and access configs, buffers,
    logs, and history files under non-ASCII paths.

16. Glob semantics:
    test `*`, `?`, `[abc]`, escapes, drive paths, UNC paths, Unicode names, and
    mixed slash/backslash input for `source-file`.

## Recommended Implementation Order

1. IPC/auth and socket boundary hardening:
   authenticated same-user peer model, custom path policy, canonical socket
   paths, startup-lock bypasses, bounded lock wait, and stale unlink defense.

2. Terminal relay tuning and coverage:
   redraw/credit tuning, native reader-failure coverage, and authenticated
   terminal handle transfer.

3. Pane/job lifecycle:
   split process-exited vs output-drained state, split passive cleanup from
   forced kill, add `pipe-pane` destroy-readiness, and log resize failures.

4. Native command/path/Unicode cleanup:
   `cmd.exe` quoting or argv-based launches, Unicode environment import,
   dynamic cwd/module paths, path-list separators, home/backslash expansion,
   and remaining wide path wrappers.

5. Test coverage:
   add native PowerShell regression tests for the behavior above. Do not rely
   on running tmux under MSYS2 for these runtime checks.
