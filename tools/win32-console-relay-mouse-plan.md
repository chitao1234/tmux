# Win32 Console Relay Mouse Plan

Date: 2026-05-19

Status: Implemented as a direct byte-path migration. Relay mouse-state
plumbing and runtime console-mode toggles landed; the console reader no
longer performs Win32 mouse-record translation. Real native-console mouse
generation still needs a stronger desktop-input validation harness.

Related docs:

- [`tools/win32-console-relay-plan.md`](win32-console-relay-plan.md)
- [`tools/win32-port-findings.md`](win32-port-findings.md)

## Goal

Keep relay mouse on Win32 inside tmux's normal tty byte-stream model.

That means:

- the client continues to relay terminal input bytes to the server;
- the server continues to parse normal tty input, including mouse;
- tmux's existing tty and pane-side mouse logic stays authoritative.

The Win32 work is only at the native console boundary.

## Problem Statement

The earlier Win32 relay mouse work translated `MOUSE_EVENT_RECORD`s into SGR
inside the client console reader. That fixed one host behavior, but it was
still a low-quality port boundary:

- it duplicated tty mouse semantics in a Win32-specific translator;
- it split keyboard and mouse across two different native input paths;
- it made relay mouse depend on console-record behavior rather than the same
  tty byte contract tmux already uses everywhere else.

For the final product direction, relay input should stay byte-oriented.

## Non-Goals

- Do not add a separate Win32-only mouse protocol.
- Do not redesign relay away from AF_UNIX here.
- Do not move native-console input handling into the detached server.
- Do not solve full auth or focus-event policy in this document.

## Chosen Design

### 1. Keep the tty byte-stream contract

Relay continues to feed ordinary tty bytes into the server.

Reason:

- tmux already parses mouse in tty code;
- pane applications already receive mouse through that path;
- a second Win32-only mouse interpretation layer is unnecessary duplication.

### 2. Keep relay mouse state from server to client

The client still needs to know when the outer terminal should be in mouse
capture mode.

That state is carried through `MSG_WIN32_TTY_STATE` and continues to represent
tmux's outer-terminal mouse mode:

- off;
- standard;
- button;
- all-motion.

### 3. Toggle console mode only

When relay mouse is active on the native console client:

- keep `ENABLE_VIRTUAL_TERMINAL_INPUT`;
- keep raw-input behavior;
- set `ENABLE_EXTENDED_FLAGS`;
- clear `ENABLE_QUICK_EDIT_MODE`.

When relay mouse is inactive:

- restore the normal raw-input baseline derived from the saved console mode.

This keeps relay mouse first-class without permanently stealing selection
behavior when tmux is not requesting mouse capture.

### 4. Keep mouse input on `ReadFile()` bytes

The native console reader no longer switches into a Win32 mouse-record path.

Specifically:

- no `ReadConsoleInputW()` mouse handling;
- no `MOUSE_EVENT_RECORD` to SGR translation;
- no relay-reader mouse state attached to the reader endpoint.

The mouse path stays a pure byte reader and relies on the native console host
to produce VT input bytes when mouse capture is enabled. The reader may still
consume non-mouse console key records for narrow keyboard compatibility fixes,
such as translating a physical-style `Ctrl+J` record that carries no byte into
the LF byte expected by tmux and panes.

### 5. Treat synthetic console-record injection as non-authoritative

`WriteConsoleInputW()` mouse-record injection is still useful as a lightweight
probe, but it is no longer treated as authoritative coverage for direct
`ReadFile()` VT mouse generation.

Reason:

- direct VT mouse generation is a host behavior at the real console boundary;
- synthetic `MOUSE_EVENT_RECORD` injection may not surface as VT bytes even if
  real desktop mouse input does.

So automated smoke can still verify relay mouse-state plumbing, but real
end-to-end generation needs a true desktop-input harness or manual validation.

## Implemented Work

### Stage 1. Relay state plumbing

Completed.

- `MSG_WIN32_TTY_STATE` carries relay mouse mode from server to client.
- the client caches that state and applies it at runtime.

### Stage 2. Console mode management

Completed.

- Win32 client console input mode uses one helper for runtime toggles.
- relay mouse state drives console-mode changes on the client.

### Stage 3. Direct byte-path reader

Completed.

- the console reader no longer stores relay mouse mode;
- the reader no longer peeks or consumes console input records;
- the reader stays on the existing `ReadFile()` byte path.

### Stage 4. Validation

Current state:

- build validation works with MSYS2 UCRT64 `make`;
- baseline native relay smoke still passes;
- relay mouse-state toggles are observable in native attach logs;
- synthetic `CONIN$` mouse-record injection is now only best-effort evidence.

Current validation gap:

- a synthetic mouse record can fail to produce VT bytes on the direct path even
  when the product design is correct;
- real native-console mouse generation still needs a desktop-input harness or
  explicit manual verification on supported console hosts.

## Risks

### 1. Console-host variability

Different native console hosts may differ in when and how VT mouse bytes are
generated for real mouse input.

Mitigation:

- keep the product boundary byte-oriented;
- validate on supported real native console hosts with actual desktop input.

### 2. Quick Edit interaction

Quick Edit can still steal mouse interaction from the relay client.

Mitigation:

- disable it only while relay mouse capture is active;
- restore the normal raw-input baseline otherwise.

### 3. False confidence from synthetic probes

A failing synthetic mouse-record probe does not necessarily mean real mouse
input is broken on the direct VT path.

Mitigation:

- keep smoke output explicit about what was and was not covered;
- treat real desktop-input coverage as a separate validation target.

## Exit Criteria

- relay mouse state is carried from server to client;
- the native console client toggles runtime console mode correctly;
- the console reader remains a pure byte path;
- baseline relay keyboard and resize behavior remain intact;
- real native-console VT mouse generation is validated separately with a
  desktop-input harness or manual native-host testing.
