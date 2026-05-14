# Win32 Server-Side Handle I/O Migration Plan

Date: 2026-05-15

Status: Draft

Related docs:

- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)
- [`tools/win32-port-findings.md`](win32-port-findings.md)

## Goal

Move the default Win32 terminal path from client console relay to server-owned
terminal handles. The Win32 I/O service is already in place; this plan uses it
to make direct handle I/O the normal path and keep console relay only as a
temporary compatibility fallback while the supported-client matrix is still
being proven.

The current target is the server-side handle path that already exists in the
codebase:

- `tty.c` already branches on `c->win32_stdin` and `c->win32_stdout`.
- `win32-event.c` already has reader and writer endpoint backends for console,
  stdio, terminal, file, and process lifetimes.
- `tmux-protocol.h` already defines `struct msg_win32_handle`.

The intended final product shape is simple: interactive Win32 clients hand
terminal handles to the server, and the server performs terminal I/O through
the Win32 I/O service. The client console relay is a migration aid only unless
testing proves a supported interactive client cannot supply handles that are
usable by the detached server.

## Design Summary

- Make server-owned direct-handle I/O the default terminal path.
- Keep the current client console relay only as a temporary fallback for
  clients whose transferred handles cannot yet drive detached server-side I/O.
- Treat the relay as removable compatibility scaffolding, not as a permanent
  supported path. If testing finds no supported Win32 client path that cannot
  provide usable server-side handles, drop the relay path instead of carrying it
  forward.
- Migrate output first. The output path is already closer to the final shape
  because the server-side writer plumbing exists and already integrates with
  redraw and close accounting.
- Migrate input second, after the output path is stable.
- Defer the full same-user / multi-logon authentication model to a separate
  design pass.
- Add only the minimal handle-claim / duplication infrastructure needed to
  unblock implementation if the current reject-all path becomes the blocker.

## Relay Removal Decision

The client console relay should not become a second production terminal stack.
It exists to keep the port usable while direct server-side handle I/O is being
implemented and tested.

The removal decision is data-driven:

- If every supported interactive Win32 client shape can transfer usable
  stdin/stdout handles, remove the relay path after direct-handle input,
  output, resize, detach, reattach, and close behavior are covered.
- If every supported client shape can transfer handles and testing proves those
  handles are usable from the detached server, remove the relay path. Do not
  keep it as a second implementation just because it exists.
- If one supported client shape can transfer handles but the detached server
  cannot use them for terminal input/output/size, keep relay only behind an
  explicit compatibility gate and document that client shape as the reason.
- If no such client shape is found, do not keep relay as an indefinite fallback
  just because it already exists.

For this decision, a handle is not "usable" merely because `DuplicateHandle`
succeeds. A usable direct terminal handle must pass server-side read, write,
resize or initial-size preservation, detach, reattach, EOF, and close tests
while the server is running in its normal detached process model.

## Current State

- `client.c` uses the console relay path for native console clients. It sends
  direct stdin/stdout handle claims by default only when the stdio handles are
  non-console handles that the detached server can use. `TMUX_WIN32_HANDLE_TTY=0`
  disables this direct path for debugging.
- Native console relay is selected explicitly as a compatibility fallback and
  can be disabled for no-relay testing with `TMUX_WIN32_CONSOLE_RELAY=0`.
- `server-client.c` accepts `MSG_IDENTIFY_WIN32_STDIN` and
  `MSG_IDENTIFY_WIN32_STDOUT`, verifies that the claimed PID matches
  `MSG_IDENTIFY_CLIENTPID`, duplicates the handles from the client process,
  and rejects mixed relay/direct identify state.
- `server-client.c` still treats `MSG_IDENTIFY_WIN32_TERMINAL` as the legacy
  console relay size signal.
- Direct clients send `MSG_IDENTIFY_WIN32_SIZE` after stdin/stdout handles so
  the server can preserve the initial terminal size across `tty_init()`.
- `tty.c` opens server-owned Win32 input and output handles through the Win32
  I/O service when they are present on the client object.
- `tty.c` treats EOF on a server-owned direct input handle as an input
  half-close. This lets pipe-backed clients finish output and normal session
  teardown instead of being destroyed immediately when stdin closes.
- `tty.c` preserves the direct client's current size if the server cannot
  query a console screen buffer from the handed handles later.

## Attachment Model

The attach handshake should stay aligned with the existing tmux message model
instead of inventing a second control plane.

- The client still connects over AF_UNIX and still sends the normal identify
  metadata.
- The direct-handle path should use the existing Win32 handle identify message
  shape, not a new message family.
- The server must own the duplicated handles before `tty_open()` consumes them.
- `MSG_READY` remains the attach-complete signal; it does not become a new
  handle-transfer protocol.
- Legacy console relay remains the fallback when a client cannot supply direct
  handles that the detached server can use during staged rollout. It should not
  remain a fallback after testing proves that every supported interactive Win32
  client can supply usable handles.

Open design point:

- The final peer-authentication and same-user logon-session policy is not
  decided here.
- If a minimal ownership check is needed to stop arbitrary handle claims from
  blocking the migration, keep it narrowly scoped to handle transfer and do not
  treat it as the final auth design.

## Handle Transfer Model

The preferred first implementation should use the existing
`MSG_IDENTIFY_WIN32_STDIN` and `MSG_IDENTIFY_WIN32_STDOUT` messages with
`struct msg_win32_handle`, but the messages must stop being arbitrary
`pid,handle` claims.

Minimum transfer rules:

- The client sends `MSG_IDENTIFY_CLIENTPID` before any Win32 handle identify
  messages.
- `MSG_IDENTIFY_WIN32_STDIN` and `MSG_IDENTIFY_WIN32_STDOUT` name handles in
  that client process.
- The server opens only the identified client process, duplicates only those
  two handles into the server process, and stores the duplicated handles in
  `c->win32_stdin` and `c->win32_stdout`.
- The server rejects Win32 handle messages if the client PID is missing, if the
  message PID does not match `c->pid`, if duplication fails, or if the duplicated
  handle does not have the expected terminal direction.
- The server owns the duplicated handles from that point. Client-side handle
  lifetime must not affect the server-side tty after duplication succeeds.

This is intentionally not the final authentication model. It is only the
minimum anti-footgun needed to avoid making the server an arbitrary cross-user
handle duplication oracle while the terminal migration is underway. The durable
same-user / multi-logon authorization model remains a separate design.

The implementation should also keep the direct-handle and relay identities
mutually exclusive:

- A direct-handle client sends stdin/stdout handle identify messages and does
  not send `MSG_IDENTIFY_WIN32_TERMINAL`.
- A relay fallback client sends `MSG_IDENTIFY_WIN32_TERMINAL` and does not send
  stdin/stdout handle identify messages.
- The server rejects mixed identify state rather than guessing which path wins.

## Proposed Message Flow

Direct-handle attach should follow this sequence:

1. The client connects over the existing AF_UNIX socket.
2. The client sends normal identify metadata: flags, term, tty name, cwd,
   features, terminfo, and environment.
3. The client sends `MSG_IDENTIFY_CLIENTPID`.
4. Direct-handle clients send both `MSG_IDENTIFY_WIN32_STDIN` and
   `MSG_IDENTIFY_WIN32_STDOUT`, followed by `MSG_IDENTIFY_WIN32_SIZE` for the
   initial terminal size. Native console clients should use this flow only
   after tests prove that the detached server can actually read, write, and
   preserve size through the duplicated console handles.
5. The server duplicates and validates the handles before
   `MSG_IDENTIFY_DONE` completes.
6. `MSG_IDENTIFY_DONE` calls the existing terminal initialization path.
7. `tty_open()` creates `tty->win32_out` and, once enabled,
   `tty->win32_in`.
8. Commands attach normally and the server sends `MSG_READY`.
9. Direct-handle clients use the direct size identify path for initial size.
   Relay-specific `MSG_WIN32_TTY_RESIZE` remains relay-only during migration.

Relay fallback should keep its current handshake while it exists:

1. The client enables the explicit console relay fallback unless
   `TMUX_WIN32_CONSOLE_RELAY=0` is set.
2. The client sends `MSG_IDENTIFY_WIN32_TERMINAL` with the initial size.
3. The server marks `c->win32_console`.
4. Input, output, ACK, and resize continue through the relay messages.

The two flows should not be combined for one client.

## Non-Goals

- Do not design the final Win32 peer-authentication model here.
- Do not resolve the full same-user multi-logon attach policy here.
- Do not replace the AF_UNIX transport here.
- Do not re-plan the I/O service itself.

## Migration Plan

### 1. Unblock direct-handle attach with minimal ownership plumbing

Before the output migration can use the server-side handle path, the server has
to be willing to accept a direct handle claim and turn it into owned handles.
If the current reject-all behavior blocks that, land only the smallest amount of
handle-claim validation / duplication needed to let the path exist.

- Keep this infrastructure narrow.
- Reorder Win32 identify so `MSG_IDENTIFY_CLIENTPID` is available before
  handle messages.
- Duplicate handles from the identified client process into the server process.
- Reject mixed relay and direct-handle identify messages.
- Do not decide the durable peer-authentication model here.
- Do not redesign the logon-session policy here.
- Do not replace the existing AF_UNIX connection model here.

### 2. Prove Win32 output through server-owned handles

Prefer `tty->win32_out` over the client relay output path for targeted
direct-handle testing first. Do not make a mixed direct-output / relay-input
hybrid into the final default.

This is the first migration slice because it is already structurally close to
the target design:

- queued terminal output should be written through the server-side I/O service;
- redraw and close accounting should continue to use writer drain callbacks;
- the client-side console relay output path should remain the staged fallback
  until the full direct input/output path is ready.

Native console testing found that console handles are transferable but not
usable from the detached server process: duplicated console stdin fails on
`ReadFile` with `ERROR_INVALID_HANDLE`, and duplicated console stdout also
fails when the server writer attempts to write. Therefore handle transfer alone
does not make native console terminal I/O server-owned. Until a separate
console-attachment or helper design exists, native console clients must stay
on the relay path. The direct-handle path is limited to non-console handles
such as pipes or redirected stdio that the detached server can actually use.

### 3. Move the normal Win32 input path to server-owned handles

Prefer `tty->win32_in` over the client relay input staging path for the default
terminal flow.

This means:

- the server should read from the handle it owns, not from the client console
  relay;
- EOF, error, and cancellation should propagate through the existing tty and
  client loss paths;
- direct stdin EOF should be a half-close rather than immediate client loss,
  while hard input errors still detach the client;
- the reader backend choice for direct terminal handles must be made
  deliberately, because actual console input needs the console reader backend
  while pipe or redirected terminal input can use the worker-backed terminal
  reader.
- direct console stdin must not be promoted until there is a proven design that
  works from the detached server process; handle transfer alone is insufficient
  for native console input.

After this slice, direct input and direct output can become the default
interactive terminal path together.

### 4. Switch size and terminal-state handling to the server-owned path

When the server owns the terminal handles, it should also own the size and
state decisions for that session.

This means:

- direct-handle sessions should use the server-side size helpers rather than
  the legacy console-relay identify path;
- direct-handle sessions should preserve an initial size supplied by the
  client when the server cannot query a Win32 console size for non-console
  handles;
- direct-handle sessions should not be reset to 80x24 just because later size
  queries fail;
- the client-side console mode shim should remain compatibility-only;
- resize propagation must stay correct when the direct-handle path is active.

### 5. Prove whether relay fallback is still needed

Before making server-side handle I/O the only path, enumerate and test the
supported Win32 client shapes:

- native console launched from PowerShell or `cmd.exe`;
- Windows Terminal / ConHost console clients;
- OpenSSH interactive sessions;
- nested or inherited console clients where stdin/stdout are redirected;
- non-interactive command/control clients.

For each interactive client shape, record two separate results:

- whether the client can transfer stdin/stdout handles to the server;
- whether the detached server can use those transferred handles for terminal
  read, write, size, detach, reattach, EOF, and close behavior without relying
  on relay-only console mode behavior.

If every supported interactive client can transfer usable handles, do not keep
the relay as a compatibility path.

### 6. Drop or quarantine the console relay

Only remove the relay from the common case after direct-handle attach, output,
input, resize, close, and reconnect are covered by tests.

There are two possible outcomes:

- If testing finds no supported client shape that needs relay, remove the
  client console relay messages and code paths.
- If testing finds a real supported path whose transferred handles are not
  usable by the detached server, keep the relay quarantined behind an explicit
  compatibility gate and document the reason. Do not leave it as the implicit
  default or an untracked fallback.
  The current gate is the native console fallback controlled by
  `TMUX_WIN32_CONSOLE_RELAY`.

## Implementation Slices

### Slice A: accept duplicated stdout behind the direct path

- Teach the client to send `MSG_IDENTIFY_CLIENTPID` before Win32 handle
  messages.
- Add the minimal server-side handle duplication helper.
- Accept `MSG_IDENTIFY_WIN32_STDOUT` for a client process handle claim.
- Use `TMUX_WIN32_HANDLE_TTY=0` as the native debugging escape hatch while the
  direct path is being promoted.
- Keep this path capability-gated while it is becoming the normal non-console
  attach mode.
- Do not enable direct stdout for native console clients until the server has a
  proven way to use the duplicated console output handle from its detached
  process.
- Do not combine direct stdout with relay stdin as a final product path.
- Verify `tty->win32_out` is used and `MSG_WIN32_TTY_OUTPUT` is not used for
  direct-handle output.

### Slice B: accept full direct-handle attach behind the direct path

- Require both stdin and stdout handles for interactive direct-handle attach.
- Prefer direct stdout when the client supplies usable handles.
- Keep the relay path as the default until direct input is usable.
- Preserve direct writer drain and graceful close semantics.

### Slice C: migrate stdin and promote direct handles to default

- Accept `MSG_IDENTIFY_WIN32_STDIN` using the same ownership rules.
- Select the correct reader backend for actual console input versus pipe or
  redirected terminal input.
- Stop starting `client_win32_input_*` for direct-handle clients.
- Verify EOF, cancellation, detach, and lost-client behavior.
- Make direct input/output the default for clients that can supply usable
  handles.

### Slice D: move resize and terminal state

- Use server-side size helpers for direct-handle clients.
- Preserve the client-supplied initial size with `MSG_IDENTIFY_WIN32_SIZE`.
- Keep the current direct size if later size queries fail.
- Keep `MSG_WIN32_TTY_RESIZE` relay-only.
- Remove assumptions that a Win32 terminal client implies
  `c->win32_console`.

### Slice E: remove or quarantine relay

- Run the client-shape matrix in the testing plan.
- Remove relay messages and client relay code if no supported client needs
  them.
- If a real supported exception remains, require an explicit compatibility gate
  and keep its tests separate from the direct-handle default path.

## Design Risks

- The handle claim path is the only likely implementation blocker. If the
  server cannot accept the direct-handle messages at all, the migration cannot
  start.
- The final auth model is intentionally out of scope here, so avoid baking any
  temporary policy into the migration path.
- Input is more sensitive than output because the current reader path has a
  console-specific backend and a generic terminal fallback backend. Keep output
  first so the input design can be validated separately.
- Resize semantics must not regress to client relay assumptions once the
  server owns the terminal handles.

## Testing Plan

All runtime checks should run natively from PowerShell with `.\tmux.exe`, not
under MSYS2.

The tracked non-console direct-handle smoke entry point is:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\win32-direct-handle-smoke.ps1
```

This script creates isolated temporary log directories, runs native `tmux.exe`
clients, disables the console relay with `TMUX_WIN32_CONSOLE_RELAY=0`, and
asserts that the server receives direct stdin/stdout handles and direct size
messages without using `MSG_IDENTIFY_WIN32_TERMINAL`.

### Baseline Regression

- Start a normal Win32 session with the existing console relay path.
- Attach, split panes, redraw, detach, reattach, and exit.
- Verify the legacy path still behaves exactly as it does today.

### Direct-Handle Smoke

- Launch a client that supplies duplicated stdout first, then stdin/stdout
  handles to the server.
- Verify the server opens the terminal through `tty.c`'s handle-backed path.
- Verify the output path works without the client relay being in the data
  path.
- Verify input reaches panes once the input slice is enabled.

### Resize Smoke

- Resize the terminal window while the direct-handle path is active.
- Verify the server observes the size change and redraws correctly.
- Verify the resize path does not depend on the console relay messages.

### Exit and EOF Smoke

- Close the client-side console input or output handle while bytes are pending.
- Verify the server completes exit cleanly instead of hanging on stale
  backpressure state.
- Verify tail output is not lost before close.

### Stress Cases

- Run large paste and sustained redraw scenarios through the direct-handle
  path.
- Verify the server thread does not block on terminal I/O.
- Verify backpressure remains bounded and the I/O service continues to drain.

### Compatibility Cases

- Verify that the relay fallback still works when direct handle transfer is not
  available during the staged migration.
- Verify that older or unsupported clients do not regress while the new path is
  being landed.
- Verify that the client relay and direct-handle paths remain mutually
  exclusive in the session state.
- Verify whether any supported interactive Win32 client cannot transfer handles
  that the detached server can use. If none are found, the expected final state
  is relay removal, not indefinite relay support.

## Exit Criteria

- The default Win32 terminal path no longer depends on client console relay.
- Server-owned handles drive the normal input and output path.
- Console relay is removed if no supported client needs it, or quarantined
  behind an explicit compatibility gate with a documented reason if one does.
- Native console clients either have a proven server-side console
  attachment/helper design or remain the documented reason for relay
  quarantine.
- Output migration lands before the input migration, and each step has native
  smoke coverage.
- The direct-handle path passes the native smoke and stress tests above.

## Deferred Work

- Full peer authentication and logon-session policy.
- Any cross-session security model changes beyond the minimum needed to accept
  trusted handle messages.
- Removal of the relay path once the direct-handle path is stable and covered.
