# Win32 I/O Service Plan

This document tracks the planned replacement of ad hoc Win32 handle I/O
bridges with one tmux-owned Win32 I/O service.

## Problem

tmux assumes that terminals, PTYs, jobs, pipes, and IPC endpoints are
nonblocking file descriptors that can be driven directly by libevent. Native
Win32 does not provide that uniform model. The Windows port currently bridges
some HANDLEs with per-endpoint worker threads and socketpair wakeups, while
other paths still call Win32 I/O APIs directly.

This keeps creating the same bug class:

- synchronous writes can block the server thread;
- process exit can be confused with output EOF;
- output handles can be closed before buffered data is proven drained;
- EOF and errors are collapsed into weak state;
- every new pane, job, terminal, or IPC path has to rediscover handle lifetime
  rules.

The durable fix is not to replace tmux's main reactor. The durable fix is to
put a Win32-native I/O service below the existing libevent loop and make all
Win32 HANDLE I/O report ordered completions back to the tmux thread.

## Design Contract

The service owns all blocking or potentially blocking Win32 I/O. Worker or IOCP
threads must never call tmux core directly. They only enqueue completions. The
tmux thread drains those completions from the existing libevent loop.

Core invariants:

- no Win32 HANDLE read or write blocks the tmux server thread;
- all tmux-visible callbacks run on the tmux thread;
- every HANDLE has a single close owner;
- process exit, output EOF, stdin close, write drain, cancel, and error are
  separate states;
- process exit does not imply output EOF;
- output EOF does not imply buffered data has been consumed by tmux;
- queued writes are ordered and eventually bounded by explicit watermarks;
- closing stdin means flush queued writes before closing unless explicitly
  aborted.

## Target Architecture

The final layout should be:

```text
tmux core/libevent reactor
        ^
        | one completion bridge
        v
Win32 I/O service
        ^
        | IOCP backend where possible, worker fallback where necessary
        v
HANDLE / pipe / ConPTY / process / console / job objects
```

The service should expose endpoint-style objects for:

- ConPTY pane stdin and stdout;
- PTY and non-PTY job stdin/stdout/stderr;
- client terminal input and output handles;
- process and job-object lifetime notifications;
- future handle-based IPC or terminal handoff.

The backend can differ per handle type, but the event contract must not.

## Event Model

The endpoint API should converge on explicit completion events:

```c
enum win32_io_event_type {
	WIN32_IO_READ,
	WIN32_IO_READ_EOF,
	WIN32_IO_WRITE_DRAINED,
	WIN32_IO_WRITE_CLOSED,
	WIN32_IO_PROCESS_EXIT,
	WIN32_IO_ERROR,
	WIN32_IO_CANCELED
};
```

For this port, the critical state machine is:

```text
running
    process exit -> record exit status, close input side, keep output alive
    output EOF   -> mark output stream complete
    buffer empty -> pane/job may report final completion to tmux
```

This is the rule that prevents losing trailing ConPTY or job output.

## Migration Plan

1. Introduce a process-wide Win32 I/O service wakeup queue.
2. Move existing read-handle worker completions from per-endpoint socketpairs
   onto that shared queue.
3. Move existing write workers behind the same endpoint vocabulary.
4. Add explicit endpoint state for EOF, error, cancel, write close, and buffered
   byte counts.
5. Convert pane ConPTY I/O to depend on endpoint state instead of direct handle
   worker details.
6. Convert PTY and non-PTY job I/O to the same endpoint state machine.
7. Add process-handle completion delivery through the same service, using
   `RegisterWaitForSingleObject` or a dedicated wait backend.
8. Replace worker-backed pipe reads/writes with IOCP-backed overlapped pipe
   endpoints where possible.
9. Add read and write watermarks so a stuck child cannot grow unbounded memory.
10. Move client terminal handle I/O onto the endpoint API when server-side
    terminal handle ownership is revived.
11. Delete old one-off Win32 I/O helpers once no caller depends on them.

## First Implementation Slice

The first slice intentionally kept behavior equivalent while changing the
ownership shape:

- keep `win32_handle_event_*` and `win32_handle_writer_*` as compatibility APIs;
- replace per-read-endpoint socketpair/libevent events with one process-wide
  Win32 I/O service event;
- keep the existing synchronous worker read backend for now;
- move writer drain/error notifications onto the shared service queue while
  keeping the synchronous worker write backend;
- close Win32 job stdin from the job write-drain callback for non-`JOB_KEEPWRITE`
  jobs, matching the existing Unix job semantics.

This gives the port a central completion bridge before introducing IOCP or
changing pane/job lifecycle behavior.

## Process Exit Slice

Process-handle readiness is now delivered through the same Win32 I/O service
bridge:

- `win32_process_event_*` wraps `RegisterWaitForSingleObject`;
- the Windows threadpool wait callback only enqueues a process completion;
- pane and job exit callbacks run on the tmux thread from the service event;
- process exit records status and closes child input, but still does not imply
  output EOF or close the ConPTY/output side;
- pane destruction remains gated on status ready, output EOF, empty output
  buffer, and the pane output error path;
- job completion remains gated on process exit plus output completion;
- the old child polling pass was kept as a temporary fallback while the
  migration continued.

## Polling Fallback Removal Slice

The legacy Win32 child polling pass has been removed:

- `server_loop()` no longer calls `win32_check_children()`;
- the 100 ms `server_ev_win32_children` timer has been deleted;
- pane status readiness is delivered by `win32_process_event_*`;
- pane final destruction is retried by the two real lifecycle edges: process
  exit and output EOF/error;
- Win32 jobs complete through their process-event callback plus output EOF/error
  callback, so the Win32 `job_check_died()` scan is gone;
- the Unix `job_check_died(pid, status)` path remains unchanged.

The next cleanup should move the pipe read/write backends from blocking worker
threads to overlapped I/O/IOCP where the handle type allows it.

## Read Watermark Slice

Worker-backed read handles now have service-level read watermarks:

- `win32_handle_event` has a ready event separate from its stop event;
- the read worker waits until the endpoint is both unpaused and below its high
  watermark before issuing the next blocking `ReadFile()`;
- once the service buffer reaches the high watermark, the worker pauses until
  tmux drains it below the low watermark;
- ConPTY pane output connects the existing pane-buffer backpressure decision to
  the service pause API, so Win32 panes can stop reading when pane output has no
  consumer instead of unboundedly growing memory;
- this is still a worker-backed compatibility layer, not the final IOCP
  endpoint backend.

## Input Write Queue Slice

Win32 pane and job stdin now have a durable tmux-side queue before data enters
the service writer:

- Win32 pane input writes append to a pane-owned input queue first, preserving
  the existing full-write acceptance contract for callers that ignore return
  values;
- pane input writer drain callbacks move bounded chunks from that tmux-side
  queue into the service writer instead of letting callers fill the worker
  buffer directly;
- Win32 job stdin follows the same queue/drain model using the job event output
  buffer;
- job stdin close now waits for both the tmux-side queue and the service writer
  queue to drain before closing, preserving non-`JOB_KEEPWRITE` semantics;
- the service writer side is capped while flushing, but this is still a
  compatibility slice. Hard caller-visible write watermarks require the
  remaining input producers to understand retry/backpressure instead of
  assuming every write is fully accepted.

## Helper Cleanup Slice

One unused synchronous compatibility helper has been removed:

- `win32_handle_event_write()` had no callers after pane, job, tty, and process
  paths moved to the shared service bridge;
- removing it eliminates one stale direct `win32_handle_write()` wrapper from
  the public Win32 platform surface;
- this is only a cleanup step. The remaining compatibility APIs still exist
  until the endpoint/event object model replaces them.

## Terminal Handle Output Slice

The server-side direct terminal output handle path now uses the shared service
writer for queued tty output:

- `struct tty` owns a `win32_handle_writer` for `client->win32_stdout`;
- the writer borrows this handle so existing client lifetime cleanup remains
  the close owner;
- normal queued tty writes are copied into the writer and drained on the
  writer completion callback, keeping `WriteFile`/`WriteConsoleW` off the tmux
  server thread;
- `tty->win32_out_pending` preserves redraw deferral semantics by treating
  bytes as complete only after the service reports writer drain;
- `tty_raw()` teardown writes for this direct-handle path are queued through
  the same writer instead of calling `win32_handle_write()` from the tmux
  server thread;
- client exit handshaking waits for the queued reset bytes to drain before
  replying with `MSG_EXITED`;
- `win32_handle_write()` is now backend-private to the worker writer instead
  of being a public Win32 platform helper.

The active console relay path is intentionally not migrated in this slice. Its
ACK is server backpressure and redraw accounting, so an asynchronous
client-side writer must ACK only after bytes are actually drained to the
console and must preserve the current UTF-8-to-`WriteConsoleW` behavior.

## Console Relay Output Slice

The active Win32 console relay output path now uses the shared service writer
inside the client process:

- `MSG_WIN32_TTY_OUTPUT` queues bytes to a borrowed stdout writer instead of
  calling `win32_handle_write()` synchronously from the client event callback;
- the client sends `MSG_WIN32_TTY_OUTPUT_ACK` only from the writer drain
  callback, so server-side backpressure and redraw accounting still mean
  output was drained to the console;
- client shutdown waits for pending console output before exiting on normal
  server exit messages;
- the writer chunk matches the 256 KiB server-side Win32 tty pending cap,
  avoiding new UTF-8 split points beyond the existing relay message boundary
  while that backpressure limit is respected.

## File Write ACK Slice

The client file-write protocol now has explicit completion ACKs:

- `MSG_WRITE_ACK` reports stream, byte count, and write error from the client
  back to the server;
- server-side file writes keep an in-flight byte count and stop sending once
  the 1 MiB file-write window is full;
- `MSG_WRITE_CLOSE` for non-stdio streams is deferred until both the local
  server buffer and client in-flight bytes are drained;
- client shutdown now treats both unflushed bufferevent output and unacknowledged
  file writes as pending work;
- Win32 client file writes use the shared service writer for both console and
  non-console destinations, preserving console CRLF text translation when the
  destination stream is an actual console handle;
- the client sends ACKs only after the writer drains, so the server's in-flight
  accounting reflects bytes accepted by the destination handle, not bytes merely
  queued in the client process.

This slice gives file-transfer output the protocol backpressure needed before
moving more generic fd write paths onto final endpoint objects.

## Read EOF State Slice

Worker-backed read handles now preserve EOF separately from hard read errors:

- `win32_handle_event` tracks running, EOF, and error states instead of a
  single generic error bit;
- zero-byte `ReadFile()` completion and EOF-style pipe statuses such as
  `ERROR_BROKEN_PIPE` are recorded as output EOF, while other failed
  `ReadFile()` calls and local buffer failures are recorded as errors;
- pane and job output callbacks drain any buffered data, then log EOF versus
  error before running the existing compatibility error callback path;
- pane and job helpers can now query output EOF directly, which is a bridge
  toward the final `WIN32_IO_READ_EOF` / `WIN32_IO_ERROR` event model;
- the read backend is still worker-thread based and the public
  `win32_handle_event_*` compatibility API remains until endpoint objects
  replace it.

## Writer State Slice

Worker-backed write handles now expose explicit write-side state queries:

- the writer backend uses one state machine for running, closing, closed, and
  error instead of independent `closing`, `closed`, and `error` booleans;
- the public compatibility API no longer exposes a generic
  `win32_handle_writer_done()` predicate;
- callers that want to enqueue more stdin bytes use
  `win32_handle_writer_writable()`;
- callers that only need to know whether queued bytes have drained use
  `win32_handle_writer_drained()`;
- `win32_handle_writer_closed()` and `win32_handle_writer_error()` are bridge
  helpers for the final write-closed/write-error event model;
- this keeps the existing worker backend and callbacks, but removes another
  collapsed lifecycle predicate from pane, job, tty, and file paths.

## Typed Completion Queue Slice

The Win32 I/O service now uses one typed completion queue internally:

- read handles, write handles, and process waits embed a common
  `win32_io_completion` record;
- the service has one FIFO pending queue instead of separate reader, writer,
  and process pending queues;
- enqueue, pending suppression, and deactivation are handled by common service
  helpers;
- the shared libevent wakeup drains typed completions and dispatches them to
  the existing compatibility callbacks on the tmux thread;
- this is still a compatibility-layer step. The public pane/job/tty/file code
  still uses the legacy `win32_handle_event_*`, `win32_handle_writer_*`, and
  `win32_process_event_*` objects, but the backend shape now matches the final
  ordered completion queue more closely.

## Completion Event Reason Slice

Typed completions now carry explicit event reasons:

- the internal completion record has an event-reason mask for read data, read
  EOF, write drained, write closed, process exit, error, and canceled;
- enqueue sites publish why they woke the tmux thread instead of making
  dispatch infer every edge from endpoint state alone;
- reasons coalesce while a completion is already pending, preserving the
  existing pending-suppression behavior while retaining event meaning;
- writer dispatch now calls write callbacks only for write-drained/write-closed
  reasons and error callbacks only for writer error state;
- reader and process dispatch use the event reasons while still preserving
  compatibility state checks;
- this is still internal metadata, not the final public endpoint API, but it is
  the bridge from kind-based completions to the planned `WIN32_IO_*` event
  vocabulary.

## File Read Service Slice

Win32 client file reads now use the shared service reader for filesystem paths:

- `struct client_file` can own a `win32_handle_event` reader as well as a
  writer;
- client-side file reads opened from a path use the Win32 handle reader instead
  of a synchronous `read()` loop in the client event path;
- read data is still sent with the existing `MSG_READ` protocol and EOF/error
  completion is still reported with `MSG_READ_DONE`;
- cancel and setup-failure paths free the reader, close the descriptor, and
  remove the file record through the normal `file_free()` path;
- standard input stream reads still use the existing fd/bufferevent path until
  console/stdin handle ownership is migrated more broadly.

## AF_UNIX Service Wakeup Slice

The Win32 I/O service wakeup socketpair now uses AF_UNIX instead of a private
loopback TCP listener:

- the wakeup path is still a raw Winsock pair owned entirely by the I/O
  service, not a tmux IPC fd entry;
- the temporary AF_UNIX pathname is created under the managed Win32 socket
  directory and is unlinked immediately after the pair is connected;
- service wakeup semantics remain unchanged: worker/process threads enqueue
  completions and write one byte to wake the tmux libevent thread;
- this removes a leftover TCP transport from the service bridge while keeping
  the final endpoint/IOCP migration separate.

## Writer High Watermark Slice

Worker-backed write handles now have a service-level queue cap:

- `win32_handle_writer_write()` refuses new bytes with `EAGAIN` once the
  writer queue reaches 1 MiB;
- writes that would cross the cap are rejected rather than partially queued,
  preserving the existing all-or-error expectation of file, tty, and client
  console callers;
- existing pane and job stdin producers still keep their own tmux-side queues
  and feed the service writer in bounded chunks;
- tty, console relay, and file-write callers already send chunks below the cap,
  so this is a defensive service invariant rather than a protocol redesign.

## Explicit Reader Event API Slice

The service now exposes explicit read event reasons to callers:

- `enum win32_io_event` is public in the Win32 platform header and matches the
  planned read, EOF, write, process, error, and cancel event vocabulary;
- `win32_handle_event_new_events()` lets callers receive a reason mask instead
  of separate read and generic error callbacks;
- the legacy `win32_handle_event_new()` constructor was kept temporarily while
  pane, job, tty, and client paths migrated to explicit events;
- Win32 client file reads are the first migrated consumer, using explicit
  `WIN32_IO_EVENT_READ`, `WIN32_IO_EVENT_READ_EOF`, and
  `WIN32_IO_EVENT_ERROR` delivery before sending the existing tmux protocol
  messages.

## Pane And Job Reader Event Slice

ConPTY pane output and Win32 job output now consume explicit reader events:

- pane and job output readers use `win32_handle_event_new_events()` instead of
  separate legacy read and generic error callbacks;
- event callbacks drain `WIN32_IO_EVENT_READ` data first, then handle EOF,
  error, or cancel as the terminal output edge;
- pane destruction and job completion still use the existing lifecycle gates,
  so process exit remains separate from output EOF and buffered output drain;
- this removes two more callers from the legacy reader callback API without
  changing the worker-backed read backend.

## Client And Tty Reader Event Slice

Client console input and direct tty input now consume explicit reader events:

- client-side console relay input uses `WIN32_IO_EVENT_READ` for forwarded
  input bytes and EOF/error/cancel as the lost-terminal edge;
- server-side direct tty input uses the same explicit event callback shape,
  preserving the existing `server_client_lost()` behavior for terminal close;
- all current `win32_handle_event` consumers now use the explicit reader event
  constructor, leaving the legacy split read/error constructor unused.

## Legacy Reader Constructor Removal Slice

The old split read/error reader constructor has been removed:

- `win32_handle_event_new()` is no longer part of the public Win32 platform
  surface;
- `struct win32_handle_event` stores only one explicit event callback instead
  of separate read, error, and event callbacks;
- reader dispatch now always delivers the explicit reason mask, matching all
  remaining reader consumers.

## Explicit Writer Event API Slice

The service now exposes explicit writer event reasons to callers:

- `win32_handle_writer_new_events()` and
  `win32_handle_writer_new_events_borrowed()` let callers receive write
  drained, write closed, and error reason masks;
- the existing split write/error constructors remain as compatibility shims
  while pane, job, tty, and client writers migrate;
- Win32 client file writes are the first migrated consumer, using explicit
  writer events before sending existing write ACKs or failure callbacks.

## Terminal Writer Event Slice

Win32 terminal output writers now consume explicit writer events:

- the client-side console relay output writer uses write-drained/write-closed
  events before sending `MSG_WIN32_TTY_OUTPUT_ACK`;
- the server-side direct terminal output writer uses the same event API before
  updating redraw accounting or completing graceful tty close;
- writer event dispatch filters stale drain reasons so callers only receive
  `WIN32_IO_EVENT_WRITE_DRAINED` when the service writer buffer is actually
  empty;
- pane and job stdin writers still use the split compatibility constructor and
  are the next writer migration targets.

## Pane And Job Stdin Writer Event Slice

Win32 pane and job stdin writers now consume explicit writer events:

- pane stdin uses write-drained/write-closed events to flush the pane-owned
  input queue into the service writer;
- pane stdin error/cancel events preserve the existing behavior of dropping the
  queued input and freeing the writer;
- job stdin uses write-drained/write-closed events to flush queued job input,
  close stdin once requested and drained, and notify the bufferevent write
  callback;
- job stdin error/cancel events still affect only stdin, keeping stdout EOF and
  process exit as separate lifecycle edges.

## Legacy Writer Constructor Removal Slice

The old split write/error writer constructors have been removed:

- `win32_handle_writer_new()`, `win32_handle_writer_new_cb()`, and
  `win32_handle_writer_new_borrowed()` are no longer part of the public Win32
  platform surface;
- `struct win32_handle_writer` stores only one explicit event callback instead
  of separate write, error, and event callbacks;
- writer dispatch now always delivers an explicit reason mask, matching all
  remaining writer consumers.

## Explicit Process Event API Slice

Process wait completions now use the explicit event callback vocabulary:

- `win32_process_event_new_events()` replaces the single-purpose process exit
  callback constructor;
- pane and job process waits receive `WIN32_IO_EVENT_PROCESS_EXIT` from the
  shared service queue before running their existing process-exit handling;
- `win32_process_event_notify()` still replays a process-exit event for delayed
  job exit callbacks, preserving the existing job callback registration
  behavior;
- the backend is still `RegisterWaitForSingleObject`, so this is an API and
  dispatch cleanup, not an IOCP/process backend replacement.

## Writer Query Cleanup Slice

Two unused writer compatibility queries have been removed:

- `win32_handle_writer_closed()` and `win32_handle_writer_error()` had no
  callers after writer consumers moved to explicit event callbacks;
- removing them keeps the public writer surface focused on enqueue,
  queue-depth, drain, writable, and close operations that current callers still
  need.

## Endpoint Scaffold Slice

The internal service queue now carries endpoint records instead of generic
completion records:

- `struct win32_io_endpoint` owns the queued event mask, pending/active state,
  endpoint kind, owner pointer, and tmux-thread event callback;
- reader, writer, and process compatibility objects embed this common endpoint
  record instead of each keeping separate callback fields;
- dispatch still routes through reader, writer, and process compatibility
  wrappers, so public pane/job/tty/file code remains unchanged;
- this is a structural step toward final endpoint objects. The read and write
  backends are still worker-thread based, and process waits still use
  `RegisterWaitForSingleObject`.

## Public Endpoint API Slice

The public Win32 I/O service surface now exposes endpoint objects instead of
reader, writer, and process-specific opaque handles:

- `win32_io_reader_new`, `win32_io_writer_new`,
  `win32_io_writer_new_borrowed`, and `win32_io_process_new` all return
  `struct win32_io_endpoint *`;
- callers free every service object through `win32_io_endpoint_free`;
- reader, writer, and process operations are named under the `win32_io_*`
  endpoint API while preserving the existing behavior and event masks;
- `struct win32_handle_event`, `struct win32_handle_writer`, and
  `struct win32_process_event` are now private backend details in
  `win32-event.c`;
- this removes another compatibility seam before the overlapped/IOCP backend
  work. The underlying read/write backends are still synchronous worker
  threads, and process waits still use `RegisterWaitForSingleObject`.

## Named Pipe Creation Slice

Pane and job pipes are now created through a named-pipe helper instead of
`CreatePipe`:

- the helper preserves the existing synchronous behavior for current callers;
- each pipe endpoint can independently request inheritable handles and
  `FILE_FLAG_OVERLAPPED` when a later backend is ready to consume it;
- current pane and job call sites still pass no overlapped flags, so this slice
  is a behavior-preserving prerequisite rather than an I/O backend change;
- this removes the `CreatePipe` limitation that prevented parent-side pipe
  handles from being opened in overlapped mode for future IOCP endpoints.
