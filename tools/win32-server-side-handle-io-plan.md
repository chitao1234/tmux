# Win32 Server-Side Handle I/O Plan

Date: 2026-05-15

Status: Implemented for non-console direct-handle I/O; maintained as a
complementary transport beside first-class native-console relay

Related docs:

- [`tools/win32-console-relay-plan.md`](win32-console-relay-plan.md)
- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)
- [`tools/win32-port-findings.md`](win32-port-findings.md)

## Goal

Define the role of server-side handle I/O in the Win32 product model now that
the detached server cannot directly drive duplicated native console handles.

This plan no longer treats server-owned terminal handles as the inevitable
replacement for every Win32 client. Instead it captures the narrower, real
contract that the codebase can currently support:

- native-console clients use the console relay path as a primary product mode;
- non-console or redirected clients use duplicated direct handles through the
  Win32 I/O service;
- control clients continue to avoid both terminal transports.

The purpose of this plan is to keep the direct-handle path explicit, reliable,
and bounded to the client shapes where it is actually useful.

## Why Server-Side Handles Still Matter

Server-side handle I/O is not useless. It solves the right problem for the
non-console client shapes that do not need the relay:

- redirected attach and detach where stdin or stdout is a pipe or file handle;
- automation and harnesses that run tmux behind non-console stdio;
- pipe-backed or pseudo-terminal frontends that present usable non-console
  handles to the detached server;
- half-close behavior where direct stdin EOF should not kill output delivery;
- future helper or broker designs that may hand the server usable non-console
  handles without using the relay protocol.

What direct handles are not, under the current detached-server model, is a
replacement for the common native PowerShell, `cmd.exe`, Windows Terminal, or
ConHost attach path.

## Product Model

### 1. Native console transport

Clients attached to a real Windows console stay on the explicit relay path.

This is the main interactive tmux use case on Win32, so it is a supported
product mode rather than a compatibility accident. The detached server cannot
currently read or write duplicated native console std handles reliably, so the
relay remains the intended transport for this client shape.

### 2. Non-console direct-handle transport

Clients whose stdio is not a native console use duplicated server-owned handles
through the Win32 I/O service.

This transport is a supported product mode for:

- redirected stdio;
- pipe-backed shells and harnesses;
- non-console attachers;
- future non-console frontends.

### 3. No mixed transport mode

One attached client chooses exactly one terminal transport:

- relay terminal identity with relay input, output, and resize; or
- direct-handle identity with duplicated stdin/stdout and initial size.

Do not support product hybrids such as relay input plus direct output or the
reverse.

## Current State

- `client.c` uses relay for native console clients and direct duplicated
  handles for non-console stdio handles.
- `server-client.c` accepts `MSG_IDENTIFY_WIN32_STDIN` and
  `MSG_IDENTIFY_WIN32_STDOUT`, validates them against
  `MSG_IDENTIFY_CLIENTPID`, duplicates them into the server process, and
  rejects mixed relay/direct identity.
- `server-client.c` treats `MSG_IDENTIFY_WIN32_TERMINAL` as the relay terminal
  identify path.
- `tty.c` opens `c->win32_stdin` and `c->win32_stdout` through the Win32 I/O
  service when the client is on the direct-handle path.
- `tty.c` treats direct stdin EOF as an input half-close, which is correct for
  pipes and redirected stdio.
- `tools/win32-direct-handle-smoke.ps1` exercises the supported non-console
  direct-handle path.
- `tools/win32-console-relay-smoke.ps1` exercises the supported native-console
  relay path.
- `TMUX_WIN32_HANDLE_TTY=force` remains a diagnostic probe only. It is not a
  supported native-console product mode.

## Boundary Rule

Direct-handle mode is selected only when the client can give the server handles
that are usable from the detached server process.

That distinction matters:

- transferable is not enough;
- `DuplicateHandle()` success is not enough;
- initial size identify is not enough.

A handle is only "usable" if the detached server can perform the actual
terminal job through it for the client shape in question:

- input;
- output;
- detach and reattach behavior;
- EOF and close behavior;
- size preservation or query behavior.

Native console handles currently do not meet that bar. Non-console handles do.

## Attachment Model

The control transport stays AF_UNIX. Direct handles are terminal attachments
within that control connection, not a replacement control plane.

Direct-handle attach:

1. The client connects over AF_UNIX and sends normal identify metadata.
2. The client sends `MSG_IDENTIFY_CLIENTPID`.
3. The client sends `MSG_IDENTIFY_WIN32_STDIN`,
   `MSG_IDENTIFY_WIN32_STDOUT`, and `MSG_IDENTIFY_WIN32_SIZE`.
4. The server duplicates and validates those handles before
   `MSG_IDENTIFY_DONE` completes.
5. `tty_open()` consumes the duplicated handles through the Win32 I/O service.
6. Runtime terminal I/O happens server-side through the direct-handle path.

Relay attach:

1. The client connects over AF_UNIX and sends normal identify metadata.
2. The client sends `MSG_IDENTIFY_WIN32_TERMINAL`.
3. The server marks the client as relay-backed.
4. Runtime terminal I/O uses the relay messages for input, output, progress,
   resize, abort, and transport loss.

The server must reject mixed identify state rather than guessing which
transport wins.

## Security Scope

This plan does not redesign the final Win32 authentication model. It only
depends on the existing minimal handle-claim validation:

- handle claims are tied to the identified client PID;
- the server duplicates handles only from that client process;
- direct-handle and relay identities are mutually exclusive.

Durable same-user and multi-logon authorization remains a separate design
track.

## What The Direct-Handle Path Must Guarantee

The direct-handle transport should be judged by the same quality bar as the
relay, but for its own client shapes.

Required properties:

- no tmux server-thread blocking on terminal input or output;
- explicit ownership of duplicated handles inside the server;
- correct stdin EOF half-close behavior;
- correct output drain and close accounting through the Win32 I/O service;
- no dependence on client-side terminal relaying after attach completes;
- no hidden fallback from direct-handle mode to relay mode after attach.

## Non-Goals

- Do not replace native-console relay in this plan.
- Do not treat relay as temporary scaffolding in product terms.
- Do not redesign AF_UNIX transport here.
- Do not design the final same-user authentication model here.
- Do not make `TMUX_WIN32_HANDLE_TTY=force` a supported configuration.

## Maintenance Plan

### 1. Keep transport selection explicit

- Native console selects relay.
- Non-console stdio selects direct handles.
- Mixed relay/direct identity remains rejected.
- Debug knobs remain diagnostic only and must not silently alter product
  defaults.

### 2. Keep direct-handle behavior reliable for non-console clients

- Maintain server-owned direct stdin/stdout through the Win32 I/O service.
- Preserve stdin EOF half-close semantics.
- Preserve output progress, drain, and close behavior without relay messages.
- Keep direct-size identify as the initial size source for this path.

### 3. Keep relay and direct docs separate

- Relay hardening belongs in
  [`tools/win32-console-relay-plan.md`](win32-console-relay-plan.md).
- Non-console direct-handle behavior belongs in this plan.
- Do not describe one path as an accidental fallback for the other.

### 4. Extend coverage where direct handles are actually supported

- Keep the non-console smoke and stress path in
  `tools/win32-direct-handle-smoke.ps1`.
- Add more redirected-stdio and pipe-backed cases there rather than trying to
  force native consoles through direct mode.
- Keep native-console probing separate and diagnostic.

### 5. Reevaluate only after a new native-console design exists

If a future attach-helper or console-attachment design gives the detached
server a robust native-console terminal backend, reevaluate the product model
then. Until that exists, do not frame relay removal as the purpose of this
plan.

## Testing Plan

All runtime checks should run natively from PowerShell with `.\tmux.exe`, not
under MSYS2.

Supported direct-handle smoke:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\win32-direct-handle-smoke.ps1
```

This covers the supported non-console path:

- direct stdin/stdout handle identify;
- direct size identify;
- relay absence under `TMUX_WIN32_CONSOLE_RELAY=0`;
- redirected attach and detach;
- sustained direct-output stress;
- direct stdin EOF half-close;
- direct output handle loss without server corruption.

Supported native-console relay smoke:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\win32-console-relay-smoke.ps1
```

This remains the primary interactive native-console coverage entry point.

Diagnostic native-console direct probe:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\win32-console-direct-probe.ps1
```

Run it only from a real Windows console. It exists to confirm the current
detached-server limitation or to validate a future native-console design. It is
not a supported product path.

## Exit Criteria

- The product model is explicit: relay for native console, direct handles for
  non-console.
- Direct-handle attach remains reliable for redirected or pipe-backed clients.
- Relay and direct-handle state remain mutually exclusive in code and protocol.
- Docs, smoke tests, and debug logging describe relay as first-class for
  native-console clients rather than as temporary scaffolding.
- Any future attempt to replace relay for native-console clients must come from
  a separate proven design, not from accidental direct-handle drift.

## Future Work

Possible future directions that do not change the current product model:

- broaden non-console direct-handle coverage;
- improve auth and same-user admission independently of terminal transport;
- evaluate helper-based native-console handoff in a separate design;
- reduce duplicated transport-specific bookkeeping where relay and direct paths
  can share lower-level I/O-service primitives.
