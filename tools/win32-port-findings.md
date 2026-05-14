# Win32 Port Findings

Date: 2026-05-13

Scope: codebase-wide Win32 implementation review of IPC/auth, terminal
relay, ConPTY/jobs, and filesystem/path compatibility. This document combines
parallel subsystem reviews with a local cross-cutting pass. No source changes
are implied by this document.

## Executive Summary

The current Win32 port has made good progress on AF_UNIX transport, ConPTY pane
execution, non-PTY jobs, and basic terminal relay. The highest remaining risks
are not isolated bugs; they are boundary and lifecycle problems where Unix
tmux assumptions were copied into native Windows semantics.

The most important areas to fix next are:

1. IPC startup and authorization:
   add a single-owner server startup lock, fail closed when the managed socket
   root cannot be built, secure custom socket paths, and define a real Windows
   client authorization model.

2. Win32 handle event/writer reliability:
   avoid lost notifications, avoid unbounded server-thread waits during
   teardown, and move blocking cancellation/join work out of hot polling paths.

3. Terminal relay correctness:
   fix output ACK accounting, support incremental UTF-8 decoding across IPC
   message boundaries, and avoid input loss on `proc_send()` failure.

4. `pipe-pane` lifecycle:
   make Win32 pipe jobs owned, killable, and fully detached or fully reaped
   when a pipe is toggled off or a pane is destroyed.

5. Unicode and native path support:
   replace narrow CRT path APIs, import the Unicode environment, fix path-list
   parsing for drive letters, and pin down slash-rooted path semantics.

## Priority Findings

### P0: Concurrent Win32 client startup can spawn multiple servers

Files:

- [`client.c`](../client.c): `client_connect()` spawns a server directly after
  a failed connect.
- [`win32-ipc.c`](../win32-ipc.c): `win32_ipc_server_create()` unconditionally
  unlinks the socket path before binding.

Problem:

The Unix path has a `.lock` file around startup. The Win32 path does not. Two
clients racing to autostart the same socket can both spawn servers. Each server
then tries to remove and bind the same socket pathname. This can produce
split-brain behavior or make a later server replace the path of an already
running server.

Required direction:

- Add a named mutex or lock-file equivalent keyed by the normalized socket path.
- Hold it across connect, spawn, and reconnect polling.
- Do stale-socket validation before unlinking.
- Do not let a second server unlink a live server's endpoint.

### P0: Win32 client authorization is too weak

Files:

- [`proc.c`](../proc.c): Win32 peers get `uid = (uid_t)-1`.
- [`server-acl.c`](../server-acl.c): `server_acl_join()` accepts all clients on
  Win32.
- [`cmd-server-access.c`](../cmd-server-access.c): server-access is mostly
  disabled on Win32.

Problem:

Once a client connects to the socket, the server effectively trusts it. That is
only safe if socket path security is perfect. It is not perfect, especially for
custom `-S` paths.

Required direction:

- Treat filesystem ACLs as one layer, not the entire auth model.
- Add a Windows peer authorization step based on user SID and logon-session
  policy, or use an authenticated challenge/response token over the AF_UNIX
  connection.
- Keep the selected model compatible with the desired behavior: multiple logon
  sessions of the same user should be attachable.

### P0: Custom `-S` socket paths are not secured

Files:

- [`tmux.c`](../tmux.c): `make_label()` hardens the managed default directory.
- [`win32-ipc.c`](../win32-ipc.c): custom server paths only ensure parent
  existence before bind.
- [`server.c`](../server.c): Win32 `server_create_socket()` ignores default vs
  explicit socket flags.

Problem:

Default `-L` sockets go through the managed socket directory flow. Explicit
`-S` paths do not get equivalent ACL hardening. If a user places a socket in a
shared or weakly protected directory, unintended clients can connect. Combined
with the ACL bypass above, this is a direct security problem.

Required direction:

- Apply security to custom socket parents and, where possible, the socket
  endpoint.
- Reject unsafe existing parent directories unless explicitly allowed.
- Detect and reject reparse points/junctions in the parent chain unless a clear
  policy says otherwise.

### P0: Managed socket root failure falls back to `C:/Temp`

File:

- [`win32-ipc.c`](../win32-ipc.c): `win32_default_socket_dir()` returns
  `C:/Temp` if managed root creation fails.

Problem:

This is unsafe and operationally surprising. If `LOCALAPPDATA` or token
identity lookup fails, the default socket should not silently move to a shared
global temp directory. The current fallback can also try to rewrite ACLs on an
existing broad temp directory rather than a tmux-owned leaf.

Required direction:

- Fail closed if the managed root cannot be built.
- If a fallback is necessary, use `GetTempPathW()` plus a private
  user/integrity-specific leaf directory and secure only that leaf.

### P0: ConPTY/job output notifications can be lost

Files:

- [`win32-event.c`](../win32-event.c): `win32_handle_event_thread()` appends
  data then calls nonblocking `send()` on the notify socket and ignores errors.

Problem:

The notify socketpair is nonblocking. If the notify socket fills, `send()` can
fail with `WSAEWOULDBLOCK`. Data remains buffered in `whe->input`, but no
libevent readiness is guaranteed. If the producer goes quiet, output can remain
stranded forever.

Required direction:

- Make notification level-triggered: one pending notification should represent
  "there is data to drain", not one byte per read.
- Track a `notified` flag under the same lock as `whe->input`.
- Clear `notified` only after the event callback drains notification state and
  observes no buffered data.
- Alternatively replace the socketpair notifier with a proper Windows event
  integrated into libevent, if that is reliable in this environment.

### P0: Win32 handle I/O teardown can block the server indefinitely

Files:

- [`win32-event.c`](../win32-event.c): `win32_handle_event_free()` waits
  forever after `CancelSynchronousIo()`.
- [`win32-event.c`](../win32-event.c): `win32_handle_writer_free()` does the
  same for writer threads.
- [`win32-conpty.c`](../win32-conpty.c): pane/job teardown calls these frees
  from server lifecycle paths.

Problem:

Reader and writer threads use synchronous `ReadFile()` and `WriteFile()`.
Their free paths signal stop, call `CancelSynchronousIo()`, and then wait
forever. If cancellation does not complete promptly for a pipe or ConPTY handle,
the tmux server thread can hang during pane/job teardown.

Required direction:

- Close or cancel the underlying I/O handle in a defined order that guarantees
  the blocking call wakes.
- Use bounded waits and log/mark leaked helper threads instead of blocking the
  server forever.
- Do not do blocking teardown from child polling paths.
- Consider overlapped I/O for pipe/conpty handles in the final design.

### P0: Terminal output ACK can acknowledge bytes never written

Files:

- [`client.c`](../client.c): `client_win32_tty_output()` sets
  `ack.size = datalen`.
- [`win32-event.c`](../win32-event.c): `win32_handle_write()` can return a
  short write through the file fallback path.
- [`server-client.c`](../server-client.c): ACK decrements
  `c->win32_tty_out_pending`.

Problem:

The client ACKs the full IPC message size regardless of how many bytes reached
the console. If `win32_handle_write()` writes fewer bytes or falls back to a
partial `WriteFile()`, the server believes output has been delivered and drains
its pending accounting. This can permanently drop output.

Required direction:

- Define `win32_handle_write()` as all-or-error for console relay output, or
  ACK only the actual consumed byte count.
- If partial writes are possible, keep the unwritten suffix in a client-side
  output buffer and resume later.

### P0: Terminal UTF-8 decoding is not incremental

Files:

- [`tty.c`](../tty.c): Win32 console output is chunked into
  `MSG_WIN32_TTY_OUTPUT` messages.
- [`client.c`](../client.c): each chunk is decoded independently.
- [`win32-event.c`](../win32-event.c): `win32_handle_write()` uses
  `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` per call.

Problem:

An IPC chunk can split a multibyte UTF-8 sequence. The client then decodes an
incomplete sequence, fails, and falls back to raw `WriteFile()` to the console
handle. That produces mojibake or broken rendering even though the original
stream was valid UTF-8.

Required direction:

- Add an incremental UTF-8 decoder for console relay output.
- Carry incomplete trailing bytes across `MSG_WIN32_TTY_OUTPUT` messages.
- Avoid raw console `WriteFile()` fallback for chunk-boundary decode failures.

## Other High-Value Findings

### Win32 input forwarding can drop data

File:

- [`client.c`](../client.c): `client_win32_input_callback()` drains the
  Win32 input buffer, sends slices, and frees the temporary buffer even if
  `proc_send()` fails mid-loop.

Impact:

Pasted input, escape sequences, and key bursts can be silently lost when the
IPC peer is backpressured or shutting down.

Fix direction:

- Keep unsent input in a persistent client-side queue.
- Retry on writable/progress events.
- Treat peer death separately from transient send failure.

### Win32 terminal write path uses synchronous recursion

File:

- [`tty.c`](../tty.c): Win32 `tty_write_callback()` paths call themselves
  recursively until buffers are empty or pending output hits a limit.

Impact:

Large redraws can build stack depth and monopolize the event loop. This is a
fragile replacement for libevent write scheduling.

Fix direction:

- Replace recursive re-entry with scheduled callbacks or event-loop deferral.
- Preserve the pending-output limit but yield between chunks.

### Fixed: Win32 `pipe-pane` teardown loses helper job ownership

Files:

- [`window.c`](../window.c): `window_pane_close_pipe()` now owns both Unix
  pipe-fd cleanup and Win32 helper-job teardown.
- [`cmd-pipe-pane.c`](../cmd-pipe-pane.c): Win32 toggle-off now uses the shared
  pipe close helper instead of only closing helper stdin.

Original impact:

`pipe-pane -I` and bidirectional pipe jobs can outlive the pane indefinitely.
After `wp->pipe_job` is cleared, later callbacks are no longer associated with
the pane, so tmux loses the control path for that logical pipe.

Resolution:

- Win32 now chooses kill-on-detach semantics for pipe helpers.
- Toggle-off and pane destruction clear pane ownership and then `job_free()`
  the helper, so the Win32 job object tears down the helper process tree.
- Native lifecycle smoke verified both toggle-off and pane destruction terminate
  long-running `pipe-pane -I` helper processes.

### Fixed: `pipe-pane -I` ignores pane write failure

File:

- [`cmd-pipe-pane.c`](../cmd-pipe-pane.c): Win32 `pipe-pane -I` now checks pane
  input readiness and the `win32_pane_write()` result.

Original impact:

If the target pane is closed or the writer rejects input, data is discarded even
though it was never queued.

Resolution:

- Helper-job output is not drained when pane input is saturated.
- The job output reader is paused and retried from a tmux-thread timer.
- Completion makes a final no-pause delivery attempt and logs if pane input is
  still saturated.

### Child exit polling does heavy synchronous teardown

Files:

- [`win32-proc.c`](../win32-proc.c): `win32_check_children()` scans panes/jobs.
- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_exited()` and
  `win32_job_exited()` disconnect handles immediately.

Impact:

The child polling timer can block on handle writer/reader cleanup, causing
head-of-line stalls across all Win32 process lifecycle handling.

Fix direction:

- Polling should mark state and schedule cleanup.
- Cleanup should be bounded and preferably asynchronous.

## Filesystem, Environment, and Path Compatibility

### Narrow CRT path APIs remain in file operations

Files:

- [`file.c`](../file.c): `fopen()` and `open()` are still used with `char *`
  paths for several read/write flows.

Impact:

Native Windows narrow CRT paths are codepage-based, not UTF-8. Config files,
buffer files, and source paths under non-ASCII directories can fail even though
other parts of the port use wide APIs.

Fix direction:

- Add UTF-8 to wide wrappers for `open`, `fopen`, and related path operations.
- Use `_wopen`, `_wfopen`, or Win32 `CreateFileW` as appropriate.
- Keep POSIX-like semantics at the tmux layer, but make the Windows boundary
  Unicode-first.

### Startup environment import is ANSI

Files:

- [`win32-error.c`](../win32-error.c): `win32_refresh_environ()` assigns
  `environ = _environ`.
- [`tmux.c`](../tmux.c): startup copies that environment into tmux globals.

Impact:

Non-ASCII values in `HOME`, `USERPROFILE`, `PATH`, `SHELL`, `PWD`, and user
variables can be corrupted before tmux sees them.

Fix direction:

- Import the process environment from the Unicode environment block.
- Convert names and values to UTF-8 before populating tmux's environment.
- Keep case-insensitive environment lookup on Win32.

### Path-list splitting conflicts with drive letters

Files:

- [`tmux.c`](../tmux.c): `expand_paths()` splits lists on `:`.
- [`Makefile.am`](../Makefile.am): default config paths are Unix-shaped.

Impact:

`C:\...` and `C:/...` conflict with colon-separated path lists. This can break
config search paths and future list-style options on Win32.

Fix direction:

- Use `;` as the native Win32 path-list separator.
- Keep compatibility for built-in Unix-style defaults only where intentional.
- Add tests for drive-letter paths in config search.

### Slash-rooted absolute path semantics are inconsistent

Files:

- [`tmux.c`](../tmux.c): `path_is_absolute()` accepts `/foo`.
- [`win32-error.c`](../win32-error.c): `win32_resolve_cwd()` rejects `/foo`.
- [`win32-error.c`](../win32-error.c): `win32_sanitize_cwd()` rejects
  slash-rooted cwd values.

Impact:

Different Win32 code paths disagree on whether `/tmp/x` is a valid absolute
path. This affects `-S`, `-f`, cwd handling, and paths inherited from
MSYS/Cygwin-like environments.

Fix direction:

- Pick a native policy:
  either reject slash-rooted paths consistently, or translate them in a defined
  way.
- Do not let generic `path_is_absolute()` contradict Win32 cwd resolution.

### Glob and fnmatch are incomplete for Win32

Files:

- [`win32-glob.c`](../win32-glob.c): `glob()` detects POSIX metacharacters but
  delegates matching to `FindFirstFileW`.
- [`win32-glob.c`](../win32-glob.c): `fnmatch()` is byte-oriented and ASCII
  case-folded.
- [`cmd-source-file.c`](../cmd-source-file.c): source-file uses `glob()`.

Impact:

POSIX glob semantics diverge on Win32, especially bracket expressions like
`foo[ab].conf`. Unicode-aware case folding is also not implemented.

Fix direction:

- Enumerate directory entries with `FindFirstFileW`.
- Apply tmux/POSIX glob matching to returned names in UTF-8 or wide form.
- Add tests for `*`, `?`, bracket expressions, escapes, UNC paths, and
  slash/backslash inputs.

### Long-path support is not native

Files:

- [`compat/win32-compat.h`](../compat/win32-compat.h): `PATH_MAX` maps to
  `MAX_PATH`.
- [`win32-error.c`](../win32-error.c): fixed `MAX_PATH` buffers are used for
  executable path discovery.
- [`win32-proc.c`](../win32-proc.c): server spawn uses `MAX_PATH` for module
  filename.

Impact:

Modern Windows long paths fail in arbitrary places. Failures may surface as
server spawn failure, terminfo discovery failure, cwd truncation, or file open
failure.

Fix direction:

- Replace fixed `MAX_PATH` module filename buffers with dynamic
  `GetModuleFileNameW()` loops.
- Avoid exposing `PATH_MAX == MAX_PATH` as a general truth.
- Decide whether to add `\\?\` normalization internally or rely on long-path
  policy support.

## Acceptable or Improving Areas

- The AF_UNIX wrapper maps many Winsock errors to `errno` and sets accepted
  sockets nonblocking in a centralized path.
- The port correctly rejects unimplemented Win32 handle-transfer identify
  messages instead of pretending descriptor transfer works.
- Environment comparison is at least case-insensitive on Win32, which matches
  Windows variable-name behavior.
- Recent ConPTY output lifetime fixes improved pane/job tail-output handling by
  separating process exit from output EOF.
- Recent wide WinAPI cleanup removed ANSI WinAPI calls from IPC path security
  and directory creation.

## Missing Tests

High-priority native PowerShell tests:

1. IPC startup race:
   start two or more clients concurrently against the same empty socket path and
   verify only one server survives and the socket path is not replaced.

2. Default IPC root failure:
   simulate missing or invalid `LOCALAPPDATA` or token identity failure and
   verify tmux fails closed rather than using `C:/Temp`.

3. Custom `-S` security:
   create sockets under weak/shared directories and verify the server rejects or
   secures them according to policy.

4. ConPTY notify stress:
   produce bursty pane/job output large enough to fill the notify socket and
   verify no output remains stranded.

5. Teardown hang:
   kill panes/jobs while reader or writer threads are blocked and verify server
   teardown is bounded.

6. Terminal UTF-8 chunking:
   force `MSG_WIN32_TTY_OUTPUT` boundaries through multibyte UTF-8 characters
   and verify rendered output is correct.

7. Terminal ACK accounting:
   simulate or force partial writes and verify the server only ACKs delivered
   bytes or retries the rest.

8. Input backpressure:
   force `proc_send()` failure or backpressure during a large paste and verify
   unsent bytes are preserved or the client fails explicitly.

9. `pipe-pane` lifecycle:
   toggle `pipe-pane -I` and `pipe-pane -IO` off, destroy the pane, and verify
   helper jobs do not leak.

10. Unicode filesystem:
    source configs, save/load buffers, and open files under non-ASCII paths.

11. Unicode environment:
    launch tmux with non-ASCII `HOME`, `USERPROFILE`, and custom variables and
    verify values survive as UTF-8 inside tmux.

12. Win32 glob semantics:
    test `*`, `?`, `[abc]`, escapes, relative paths, absolute drive paths, and
    UNC paths for `source-file`.

## Recommended Implementation Order

1. IPC/auth hardening:
   startup lock, fail-closed managed root, custom path security, stale socket
   validation, and explicit peer authorization.

2. Win32 event helper reliability:
   notification coalescing/level-triggering, bounded teardown, and cleanup
   outside polling paths.

3. Terminal relay correctness:
   exact ACK semantics, incremental UTF-8 decoding, persistent input queue, and
   nonrecursive output scheduling.

4. `pipe-pane` lifecycle:
   owned helper job cleanup, correct detach semantics, and partial-write/error
   handling.

5. Unicode filesystem/environment:
   wide file wrappers, Unicode environment import, long-path-safe module path
   helpers.

6. Path semantics and glob:
   Win32 path-list separator, consistent slash-root policy, case/normalization
   rules, and POSIX-compatible glob behavior over wide directory enumeration.
