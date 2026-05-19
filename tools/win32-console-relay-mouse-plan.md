# Win32 Console Relay Mouse Plan

Date: 2026-05-19

Status: Implemented; native relay path fixed, pane-app mode coverage still needs a dedicated native helper

Related docs:

- [`tools/win32-console-relay-plan.md`](win32-console-relay-plan.md)
- [`tools/win32-port-findings.md`](win32-port-findings.md)

## Goal

Make mouse work for native-console tmux clients on the Win32 console relay
path, including TUI applications inside tmux that rely on SGR mouse.

This plan keeps the existing tmux tty input model:

- the client still relays terminal input to the server;
- the server still parses ordinary tty byte streams;
- tmux's existing mouse parser and pane-side mouse forwarding remain the
  authoritative implementation.

The Win32 change is only at the native console boundary.

## Current Problem

Today the native-console relay client reads console input as a byte stream via
`ReadFile()`. That works for keyboard input, but Windows mouse input is not
delivered on that path. Mouse arrives as console input records and is currently
not translated into the xterm/SGR mouse sequences that tmux already expects.

That produces this split:

- resize works because the relay has an explicit `MSG_WIN32_TTY_RESIZE` path;
- mouse does not work because no mouse event reaches `tty_keys_mouse()`.

## Non-Goals

- Do not add a separate Win32-only mouse protocol carrying parsed tmux mouse
  events.
- Do not redesign the relay away from AF_UNIX.
- Do not move native-console input handling into the detached server.
- Do not do a full focus-event design pass here.

## Design

### 1. Keep the tty byte-stream contract

The relay must continue to feed normal tty input bytes into the server.

Reason:

- tmux already parses mouse in tty code;
- pane applications inside tmux already receive mouse through the existing
  input path;
- a new Win32-only mouse protocol would duplicate tmux semantics in a second
  transport layer.

### 2. Add relay mouse state from server to client

The client needs to know when tmux wants outer-terminal mouse capture.

Add a small server-to-client message that carries the current relay mouse mode:

- off
- standard
- button
- all-motion

This state is derived from the tty mode that tmux already computes for the
outer terminal.

### 3. Toggle native console mouse capture explicitly

When relay mouse is active on the client console:

- enable `ENABLE_MOUSE_INPUT`;
- set `ENABLE_EXTENDED_FLAGS`;
- clear `ENABLE_QUICK_EDIT_MODE`.

When relay mouse is inactive:

- restore the normal raw-input baseline derived from the saved console mode.

This makes relay mouse first-class without permanently stealing normal console
selection behavior when tmux is not using mouse capture.

### 4. Translate native mouse records into SGR mouse

Inside the Win32 console reader:

- inspect console input records when relay mouse is active;
- consume `MOUSE_EVENT_RECORD` entries with `ReadConsoleInputW()`;
- translate them into SGR mouse sequences;
- append those bytes into the existing relay input buffer.

SGR is the right synthesized format because tmux already preserves richer
release semantics when the outer event is SGR-form.

### 5. Keep keyboard input on the current path

Do not replace the current keyboard byte path with a full custom key
translator in this change.

When relay mouse is active, the console reader may need to filter obvious
non-byte records such as:

- mouse records;
- window records;
- menu/focus records;
- key-up and modifier-only key records that would otherwise stall the byte
  reader.

But ordinary keyboard bytes should still come from the existing `ReadFile()`
path so this work stays scoped to the mouse gap.

## Implementation Stages

### Stage 1. Relay state plumbing

- Add a new protocol message for relay tty state.
- Add a small state struct carrying mouse mode.
- Teach the server to send state updates for Win32 relay clients when outer
  mouse mode changes.
- Teach the client to receive and cache that state.

### Stage 2. Console mode management

- Refactor Win32 client console input mode setup so runtime mouse-mode toggles
  reuse one helper.
- Apply the relay mouse state to console mode changes at runtime.

### Stage 3. Console reader translation

- Extend the Win32 console reader endpoint with relay mouse state.
- Add a console-reader setter so the client can update that state.
- When relay mouse is active, inspect console input records before falling back
  to `ReadFile()`.
- Translate:
  - button press;
  - button release;
  - drag;
  - all-motion hover;
  - wheel.
- Feed translated SGR bytes into the existing reader buffer and event path.

### Stage 4. Validation

Implemented for the relay boundary.

- Build with MSYS2 UCRT64 `make`.
- Re-run native PowerShell smoke for the relay path to catch regressions.
- Extend `tools/win32-console-relay-smoke.ps1` with `-ExerciseMouse` so it:
  - injects native mouse records through `CONIN$`;
  - verifies relay mouse-state toggles on the client;
  - verifies the server logs parsed SGR mouse input and reaches the pane mouse
    binding path.

Remaining coverage gap:

- the current PowerShell pane helper used by the smoke does not successfully
  establish pane mouse mode (`mouse_any_flag` stays `0`), so end-to-end
  pane-side byte capture still needs a smaller dedicated native helper rather
  than a PowerShell host.

## Risks

### 1. Console queue ordering

Mixing `ReadConsoleInputW()` for mouse records with `ReadFile()` for keyboard
bytes must not strand the reader on non-byte records.

Mitigation:

- only enter record-aware mode when relay mouse is active;
- explicitly consume records that cannot produce tty bytes.

### 2. Coordinate mismatch

Windows console mouse positions are screen-buffer coordinates, not necessarily
visible-window coordinates.

Mitigation:

- convert against `GetConsoleScreenBufferInfo()` before synthesizing SGR
  coordinates.

### 3. Quick Edit interaction

Quick Edit can steal mouse interaction from the relay client.

Mitigation:

- disable it only while relay mouse capture is active;
- restore the normal raw-input baseline otherwise.

## Exit Criteria

- Native-console relay clients can drive tmux mouse bindings.
- TUI applications inside tmux receive SGR mouse on Win32 relay.
- Relay keyboard behavior is not regressed.
- Existing relay resize behavior remains unchanged.
