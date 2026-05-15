# Win32 Console Relay First-Class Design

Date: 2026-05-15

Status: In progress; Stage 2 output-abort semantics implemented

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

Current code and findings show three protocol-level gaps that are acceptable
for a prototype but not for a first-class transport.

### 1. Output loss can deadlock close

Today the server only learns successful output completion through
`MSG_WIN32_TTY_OUTPUT_ACK`. If the client output writer fails after bytes were
accepted but before they are fully written, the client sends `MSG_EXITING`
without telling the server how much outstanding output will never be ACKed.

Result:

- `c->win32_tty_out_pending` can remain nonzero forever;
- graceful close can stall waiting for an ACK that cannot arrive.

### 2. Input relay has no tmux-level credit window

The client pauses console reads only while bytes remain in
`client_win32_input_pending`. Once bytes are moved into the peer imsg queue,
the console reader resumes.

Result:

- a long paste can grow input in flight without a tmux-level bound;
- pressure is controlled only by lower-level socket buffering, not by the
  terminal transport protocol.

### 3. Output ACK is too coarse

The client currently ACKs output at whole-drain granularity.

Result:

- redraw and close progress are tied to full writer drain events;
- heavy output behaves like stop-and-wait over large windows;
- responsiveness suffers even when correctness is preserved.

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

The current relay messages are insufficient for a first-class transport:

- `MSG_WIN32_TTY_INPUT`
- `MSG_WIN32_TTY_OUTPUT`
- `MSG_WIN32_TTY_OUTPUT_ACK`
- `MSG_WIN32_TTY_RESIZE`

The protocol should be extended with explicit credit and abort messages.

### New message family

- `MSG_WIN32_TTY_INPUT_CREDIT`
  Server to client. Adds input send credit in bytes.
- `MSG_WIN32_TTY_OUTPUT_PROGRESS`
  Client to server. Reports completed output bytes incrementally.
- `MSG_WIN32_TTY_OUTPUT_ABORT`
  Client to server. Reports output bytes dropped due to writer failure or
  console loss.
- `MSG_WIN32_TTY_TRANSPORT_LOST`
  Either direction. Terminal transport is no longer usable; detach or exit
  should stop waiting for further progress.

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

- Progress reports bytes actually written, not merely accepted by the client
  writer.
- Progress can be sent before full drain.
- The server decrements `c->win32_tty_out_pending` by progress amount.

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

- Extend the protocol with output-abort.
- Track output accepted, completed, and aborted separately.
- Make graceful close complete on either full progress or explicit abort.

### Stage 3: add input credit accounting

- Extend the protocol with server-issued input credits.
- Track total input bytes in flight across local staging and IPC queueing.
- Pause console reads based on transport credit, not only local buffer emptiness.

### Stage 4: add incremental output progress

- Extend writer callbacks or accounting so client can report completed bytes
  before full drain.
- Replace drain-only ACK logic with incremental progress updates.
- Relax redraw gating to threshold-based behavior.

### Stage 5: harden transport loss semantics

- Add explicit transport-lost state transitions.
- Audit detach, exit, shutdown, and peer-loss paths so none wait on impossible
  relay completions.
- Ensure logs explain whether exit was graceful, aborted, or transport-lost.

### Stage 6: test and tune

- Tune credit sizes and redraw thresholds using large paste and redraw-heavy
  workloads.
- Verify memory stays bounded under sustained input.
- Verify close and detach complete under console writer failure.

## Testing Plan

All runtime checks should run natively from PowerShell with `.\tmux.exe`, not
under MSYS2.

### Existing coverage

- `tools/win32-console-relay-smoke.ps1`
  Baseline attach and detach on the native-console relay path.
- `tools/win32-console-relay-smoke.ps1 -SimulateOutputLoss`
  Relay output-loss coverage. It verifies that output failure sends explicit
  abort accounting and that attach exits instead of hanging for an impossible
  ACK.
- `tools/win32-direct-handle-smoke.ps1`
  Regression coverage for non-console direct-handle mode.

### New relay-focused tests needed

1. Large paste:
   paste enough input to exceed the intended credit window and verify relay
   reading pauses and resumes without unbounded memory growth.
2. Redraw-heavy output:
   generate steady output and verify incremental progress keeps redraw and
   status updates responsive.
3. Detach under output backlog:
   detach while output is still in flight and verify close completes with
   either progress or abort.
4. Reader failure:
   lose console input while output remains healthy and verify explicit
   transport-lost handling.
5. Resize during backlog:
   resize during heavy output and verify resize messages are not starved behind
   coarse output waiting.

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
