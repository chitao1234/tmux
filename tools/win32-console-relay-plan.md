# Win32 Console Relay First-Class Design

Date: 2026-05-15

Status: In progress; Stages 2-5 implemented

Related docs:

- [`tools/win32-port-findings.md`](win32-port-findings.md)
- [`tools/win32-server-side-handle-io-plan.md`](win32-server-side-handle-io-plan.md)
- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)

## Goal

Make the Win32 console relay an explicit first-class terminal transport for
native-console tmux clients instead of treating it as temporary scaffolding.

This design accepts the current Windows platform constraint:

- non-console handles such as pipes can use server-owned handle I/O;
- native console handles cannot currently be driven by the detached tmux server
  merely by duplicating `STD_INPUT_HANDLE` and `STD_OUTPUT_HANDLE`.

So the product model becomes:

- native console clients use the console relay path;
- non-console clients use the direct server-side handle path;
- control clients continue to avoid both terminal transports.

The relay is no longer described as a fallback for the main native-console use
case. It is a supported terminal mode that must meet the same reliability bar
as the direct-handle path.

## Product Position

### 1. Native console mode

Clients attached to a real Windows console, Windows Terminal, or ConHost stay
on relay by default.

Why:

- the tmux server runs as `DETACHED_PROCESS`;
- direct duplication of native console std handles has been proven
  transferable but not usable from that detached process;
- users primarily run tmux from a console, so this path must be treated as a
  primary mode, not a compatibility accident.

### 2. Direct-handle mode

Clients whose stdio is not a native console use server-owned handles through
the Win32 I/O service.

This remains valuable for:

- redirected attach and detach;
- pipe-backed frontends;
- non-console automation and harnesses;
- future helper designs that may feed the server usable non-console handles.

### 3. No mixed transport mode

One client chooses exactly one terminal transport:

- relay terminal identity with relay input, output, and resize; or
- direct-handle identity with direct input, output, and initial size.

Do not support hybrid "relay input plus direct output" or the reverse as a
product mode.

## Why This Design Is Necessary

The existing native-console direct probe shows that `DuplicateHandle` success
is not enough. The detached server cannot currently use duplicated native
console handles as a working terminal backend.

That means the previous framing, where relay was "temporary until direct
handles take over everything," is wrong for the current product:

- it misstates which path real users actually depend on;
- it deprioritizes relay reliability work even though relay is carrying the
  main native-console workflow;
- it encourages judging the relay by lower standards than the direct-handle
  path.

The right correction is not to force more direct-console experiments into the
product. The right correction is to harden relay intentionally while leaving
open the option of a future console attach or helper design.

## Design Principles

- Treat relay as a real transport with explicit flow control, error semantics,
  and observability.
- Keep the tmux server detached. This design does not depend on `AttachConsole`
  or a helper process.
- Reuse the Win32 I/O service for client-side console reader and writer work.
- Keep relay and direct-handle states mutually exclusive in protocol and
  runtime state.
- Avoid auth redesign in this plan. Relay hardening is transport work.
- Preserve current user-visible behavior where possible, but choose correctness
  over accidental compatibility.

## Current Relay Problems

Current code and findings no longer show an open protocol-level gap for the
relay path. The earlier Stage 2-5 gaps are included here as completed
hardening work because they define the relay contract that native console
clients now rely on. The remaining work is tuning and broader native coverage.

### 1. Output loss previously deadlocked close

Stage 2 fixed this by adding explicit `MSG_WIN32_TTY_OUTPUT_ABORT`
accounting. Graceful close no longer depends on an ACK that can never arrive
after client-side console writer failure.

### 2. Input relay previously had no tmux-level credit window

Stage 3 fixed this with explicit `MSG_WIN32_TTY_INPUT_CREDIT` flow control.
The server now grants a bounded input window and the client reserves credit
before draining console input into the relay path.

### 3. Output progress previously was drain-only and too coarse

Stage 4 fixed this without adding a new protocol message name.

The client now reports output completion incrementally through the existing
`MSG_WIN32_TTY_OUTPUT_ACK` message semantics. The Win32 I/O service exposes
`WIN32_IO_EVENT_WRITE_PROGRESS`, the client and direct tty paths consume that
progress incrementally, and server redraw deferral is threshold-based instead
of "any pending byte means wait."

Result:

- redraw and close progress no longer depend on full writer drain events;
- heavy output no longer behaves like stop-and-wait over large windows;
- responsiveness improves while close accounting still stays explicit.

### 4. Transport-lost semantics are explicit

Stage 5 fixed the remaining relay completion ambiguity with
`MSG_WIN32_TTY_TRANSPORT_LOST`.

Result:

- client-declared terminal transport loss is explicit and logged;
- the server drops remaining relay output pending state and input credit;
- graceful close no longer waits for relay progress after transport loss;
- late relay input, resize, progress, and abort messages are ignored once
  transport loss has been declared;
- the Win32 client no longer waits for local relay output progress once the
  transport is already known lost.

## Target Relay Model

The hardened relay becomes a credit-based terminal proxy protocol between the
server and the native-console client.

### Output path

Server to client output uses explicit send accounting:

1. The server sends `MSG_WIN32_TTY_OUTPUT` chunks.
2. The client queues chunks into the Win32 console writer.
3. The client reports completed progress incrementally.
4. The server uses those completions to release pending bytes, continue redraw,
   and finish graceful close.
5. If the client loses the console writer, it sends an explicit output-abort
   message so the server can discard unreachable outstanding bytes.

### Input path

Client to server input uses explicit credits:

1. The server grants an input credit window.
2. The client reads console input only while it has credit.
3. `MSG_WIN32_TTY_INPUT` consumes credit as bytes are sent.
4. The server returns credit after those bytes are absorbed into tmux input
   processing.
5. Loss, detach, and shutdown explicitly cancel any remaining credit.

### Resize path

Resize remains relay-specific for native-console clients:

- client detects size changes locally;
- client sends `MSG_WIN32_TTY_RESIZE`;
- server updates tty size and redraw state.

No attempt is made in this design to move native-console resize to server-owned
handles.

## Protocol Changes

The relay started from this minimal message set:

- `MSG_WIN32_TTY_INPUT`
- `MSG_WIN32_TTY_OUTPUT`
- `MSG_WIN32_TTY_OUTPUT_ACK`
- `MSG_WIN32_TTY_RESIZE`

Hardening extends those semantics with explicit credit, abort, and
transport-loss handling while keeping `MSG_WIN32_TTY_OUTPUT_ACK` as the
completed-output progress message.

### New message family

- `MSG_WIN32_TTY_INPUT_CREDIT`
  Server to client. Adds input send credit in bytes.
- `MSG_WIN32_TTY_OUTPUT_ACK`
  Client to server. Reports completed output bytes incrementally. The message
  name is retained; the semantics are no longer limited to full-drain ACKs.
- `MSG_WIN32_TTY_OUTPUT_ABORT`
  Client to server. Reports output bytes dropped due to writer failure or
  console loss.
- `MSG_WIN32_TTY_TRANSPORT_LOST`
  Client to server in the current implementation. Terminal transport is no
  longer usable; detach or exit should stop waiting for further relay
  progress.

The names are illustrative. The important part is semantics, not the exact
enumerator spelling.

### Message semantics

#### Input credit

- Credits are monotonic additions, not absolute watermarks.
- Client must not send `MSG_WIN32_TTY_INPUT` bytes beyond available credit.
- Input already placed in the local staging buffer still counts against total
  in-flight bytes until the server returns credit.
- On peer loss or detach, remaining credit is discarded.

#### Output progress

- Progress reports bytes drained from the client writer toward the terminal,
  not merely bytes accepted from IPC into the relay transport.
- Progress can be sent before full drain.
- The server decrements `c->win32_tty_out_pending` by progress amount.
- Direct non-console tty output uses the same writer-progress primitive so
  relay and direct-handle modes share the same accounting model.

#### Output abort

- Abort reports bytes accepted from the server but never written to the
  console.
- After abort, the server drops matching pending bytes and stops waiting for
  normal progress for them.
- Abort should be idempotent or generation-scoped so duplicate delivery does
  not corrupt accounting.

#### Transport lost

- This is a terminal-transport event, not generic server shutdown.
- It exists to break ambiguous waits and make loss explicit in logs and state.
- After transport loss, the server drops remaining relay input credit and
  output pending state and ignores any late relay progress, abort, resize, or
  input messages that arrive after the loss declaration.

## State Machines

### Relay client state

- `RELAY_IDLE`
  No active terminal transport.
- `RELAY_ACTIVE`
  Input reader and output writer available.
- `RELAY_OUTPUT_FAILED`
  No further output progress will arrive; client sends abort and transport-lost.
- `RELAY_CLOSING`
  Client has sent `MSG_EXITING` or received detach/exit and is draining what it
  still can.
- `RELAY_CLOSED`
  End state.

### Relay server state

- `SERVER_RELAY_ACTIVE`
  Normal relay mode; pending bytes and input credit tracked.
- `SERVER_RELAY_OUTPUT_WAIT`
  Output bytes in flight; redraw and close may be partially gated.
- `SERVER_RELAY_LOST`
  Console transport unusable; no further waits for relay progress.
- `SERVER_RELAY_CLOSE_PENDING`
  TTY close requested while bytes or credits remain outstanding.
- `SERVER_RELAY_CLOSED`
  End state.

These states do not need to be represented as a single enum immediately, but
the implementation should behave as though they exist.

## Flow Control Policy

### Input

Use byte credits with explicit high and low watermarks.

Suggested policy:

- target in-flight input window: 64 KiB to 256 KiB;
- top up credit when remaining budget drops below a low watermark;
- pause local console reads when total in-flight bytes reach the configured
  high watermark.

The important rule is that "bytes in the imsg queue" count as in flight, not
just bytes still sitting in `client_win32_input_pending`.

### Output

Keep the existing bounded output policy on the server, but make redraw gating
threshold-based instead of "any pending byte means wait."

Suggested policy:

- allow small pending output without freezing redraw;
- gate redraw only after pending output crosses a threshold;
- ungate as incremental progress reduces pending bytes.

This keeps the server responsive during steady output instead of waiting for a
full writer drain every cycle.

## Error Handling

### Output loss

If the console writer fails:

- client records how many bytes were accepted but not completed;
- client sends output-abort with that amount or the current output generation;
- client sends transport-lost if the terminal cannot continue;
- server clears matching pending bytes and completes close or detach without
  waiting for impossible progress.

### Input loss

If the console reader fails:

- client stops reading immediately;
- client discards unused credits;
- client sends transport-lost or `MSG_EXITING` depending on session state;
- server detaches or exits without treating missing input as recoverable.

### Detach and exit

Detach should no longer rely on accidental writer-drain timing:

- successful output completion uses progress messages;
- failed completion uses abort;
- close logic accepts either path as terminal completion.

## Observability

A first-class transport needs logs and counters that describe relay state
directly.

Minimum logging:

- input credit granted, consumed, and exhausted;
- output progress bytes and pending bytes;
- output abort amount or generation;
- transport-lost reason;
- redraw gating and ungating decisions.

Minimum metrics in debug logs:

- current input credit;
- input bytes in flight;
- output bytes pending;
- output bytes progressed since last callback;
- number of abort events.

## Compatibility Rules

- Preserve the existing relay identify message:
  `MSG_IDENTIFY_WIN32_TERMINAL`.
- Keep relay opt-out with `TMUX_WIN32_CONSOLE_RELAY=0` for testing.
- Keep `TMUX_WIN32_HANDLE_TTY=force` as diagnostic-only direct-console probing.
- Do not make native-console behavior depend on direct-handle success.
- Do not regress existing non-console direct-handle clients.

## Implementation Plan

### Stage 1: make relay primary in design and docs

- Update design docs to describe relay as first-class for native-console
  clients.
- Stop describing native-console relay as temporary fallback in product terms.
- Keep direct-handle mode documented as primary for non-console clients.

### Stage 2: add output abort semantics

Implemented.

- Extend the protocol with output-abort.
- Track output accepted, completed, and aborted separately.
- Make graceful close complete on either full progress or explicit abort.

### Stage 3: add input credit accounting

Implemented.

- Extend the protocol with server-issued input credits.
- Track total input bytes in flight across local staging and IPC queueing.
- Pause console reads based on transport credit, not only local buffer emptiness.

### Stage 4: add incremental output progress

Implemented.

- Writer backends now publish `WIN32_IO_EVENT_WRITE_PROGRESS`.
- Client relay output and direct tty output both consume incremental writer
  progress.
- `MSG_WIN32_TTY_OUTPUT_ACK` is now used as incremental completed-byte
  progress.
- Redraw gating is threshold-based rather than blocked by any pending byte.

### Stage 5: harden transport loss semantics

Implemented.

- Added explicit `MSG_WIN32_TTY_TRANSPORT_LOST`.
- Relay loss now clears server-side relay pending state and input credit.
- Graceful close no longer waits on relay completion after transport loss.
- Late relay progress, abort, resize, and input messages are ignored once the
  relay transport is declared lost.
- Native relay smoke now covers simulated transport loss under output backlog.

### Stage 6: test and tune

In progress.

- Native detach-under-backlog smoke now covers the close path where relay
  output is still pending at detach time. The server logs the close-pending
  state and the smoke verifies that detach completes under backlog.
- Native input-credit smoke now injects more than the 64 KiB relay credit
  window through the real console input buffer, verifies a client-side credit
  pause and resume, checks that the server returns credit repeatedly while
  attach still exits cleanly, and records peak tmux-owned reserved plus reader-
  buffered bytes so bounded relay memory is measured directly.
- Native output-progress smoke now also forces a status redraw while output
  backlog is active and verifies redraw deferral plus redraw-release behavior,
  not only the existence of progress ACKs. It now also snapshots live logs
  across a lighter output phase and a heavy backlog phase so it can verify that
  status redraws continue without Win32 threshold deferral below the redraw
  limit, then confirm that the heavy phase does cross the Win32 threshold and
  later releases redraw again.
- Native resize-under-backlog smoke now generates sustained relay output,
  resizes the real console while backlog is active, verifies that tmux updates
  the attached client's `client_width` and `client_height` before detach, and
  checks both client-side and server-side relay resize logs.
- Native UTF-8 split smoke now forces a writer-side split through a multibyte
  console code point, verifies the intact text in the visible console buffer,
  and checks the carried-byte logs at the console writer boundary.
- Native invalid UTF-8 smoke now forces invalid bytes at the console writer
  boundary, verifies `MultiByteToWideChar` failure logging, and checks that
  attach aborts through the explicit output-abort and transport-loss path.
- A dedicated native reader-loss harness now launches the relay attach inside a
  child console window, waits until relay output progress is visible, then
  closes that child console window and checks for either the real input-closed
  transport-loss path or the server-side relay peer-loss fallback when Windows
  tears down the client before it can send the in-band transport-loss message.
  This still needs validation across supported native console hosts before the
  gap is fully retired.
- Tune credit sizes and redraw thresholds using large paste and redraw-heavy
  workloads.
- Verify memory stays bounded under sustained input.
- Verify close and detach complete under console writer failure.

## Testing Plan

All runtime checks should run natively from PowerShell with `.\tmux.exe`, not
under MSYS2.

### Existing coverage

- `tools/win32-console-relay-smoke.ps1`
  Baseline attach and detach on the native-console relay path. It now also
  checks that relay-mode logs include Win32 input credit activity.
- `tools/win32-console-relay-smoke.ps1 -ExerciseOutputProgress`
  Relay output-progress coverage. It generates sustained console output,
  forces status redraws across both a lighter output phase and a heavy backlog
  phase, detaches the correct attach client by PID, and verifies multiple
  incremental output-progress events plus both sides of redraw-threshold
  behavior: no Win32 threshold deferral below the limit, then explicit Win32
  threshold deferral and redraw release once the heavy phase crosses it.
- `tools/win32-console-relay-smoke.ps1 -ExerciseResizeBacklog`
  Relay resize-under-backlog coverage. It generates sustained console output,
  resizes the real console while relay output is still pending, verifies that
  tmux updates the attached client's size through `list-clients`, and checks
  both client-side and server-side relay resize logs before detach.
- `tools/win32-console-relay-smoke.ps1 -ExerciseInputCredit`
  Relay input-credit coverage. It injects more than the 64 KiB credit window
  through `CONIN$`, verifies that console input pauses and resumes at the
  client when credit is exhausted and returned, checks that the server returns
  credit repeatedly, and records peak tmux-owned reserved plus reader-buffered
  bytes.
- `tools/win32-console-relay-smoke.ps1 -ExerciseUtf8Split`
  Relay UTF-8 split coverage. It forces a multibyte character to cross relay
  output chunk boundaries inside the client writer, checks that the visible
  console output still shows the intact text, and verifies the carried-byte
  logging that proves incremental UTF-8 decoding.
- `tools/win32-console-relay-smoke.ps1 -ExerciseInvalidUtf8`
  Relay invalid UTF-8 coverage. It forces invalid UTF-8 into the console
  writer boundary, verifies that conversion fails explicitly, and checks that
  output abort plus transport-loss handling detach the client instead of
  hanging.
- `tools/win32-console-relay-smoke.ps1 -SimulateOutputLoss`
  Relay output-loss coverage. It verifies that output failure sends explicit
  abort accounting and that attach exits instead of hanging for an impossible
  ACK.
- `tools/win32-console-relay-smoke.ps1 -SimulateTransportLost`
  Relay transport-loss coverage. It simulates relay transport loss after
  output is already queued, then verifies that attach exits promptly and logs
  explicit transport-loss handling rather than waiting for impossible relay
  completion.
- `tools/win32-console-relay-smoke.ps1 -ExerciseDetachBacklog`
  Relay detach-under-backlog coverage. It detaches while output is still in
  flight, verifies that relay close enters the logged pending state, and checks
  that the attach still exits cleanly.
- `tools/win32-console-relay-reader-loss-smoke.ps1`
  Native reader-loss harness. It launches the relay attach in its own child
  console window, waits for live relay output progress, closes that child
  console window with `WM_CLOSE`, and checks whether the client and server logs
  follow either the explicit input-closed transport-loss path or the server-side
  relay peer-loss fallback for hard console teardown.
- `tools/win32-direct-handle-smoke.ps1`
  Regression coverage for non-console direct-handle mode.

### New relay-focused tests needed

1. Native reader failure without test knobs:
   validate the child-console-close harness across supported native console
   hosts and confirm it reliably follows either the explicit transport-lost
   path or the server-side relay peer-loss fallback used for hard console close.

## Exit Criteria

- Native-console relay is documented as a supported primary terminal mode.
- Relay input bytes in flight are bounded by explicit tmux-level credit.
- Relay output completion no longer depends solely on full-drain ACKs.
- Output loss cannot strand graceful close waiting for impossible ACKs.
- Detach and exit paths complete correctly for normal drain, output abort, and
  transport loss.
- Direct-handle mode continues to pass non-console smoke coverage.

## Non-Goals

- Do not remove the direct-handle path for non-console clients.
- Do not design the final auth model here.
- Do not redesign tmux transport away from AF_UNIX here.
- Do not introduce a native-console attach/helper process in this plan.

## Future Work

If a future native-console attach/helper design proves robust, it may replace
the relay path for native-console clients. That would be a separate design and
should be judged against this hardened relay mode, not against the earlier
prototype relay.
