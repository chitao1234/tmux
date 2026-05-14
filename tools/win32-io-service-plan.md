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

## Overlapped Reader Backend Slice

Pane and job output readers now use an IOCP-backed overlapped read backend:

- the named-pipe helper can create parent-side output read handles with
  `FILE_FLAG_OVERLAPPED`;
- ConPTY pane output and Win32 job stdout pipes use overlapped parent-side read
  handles while keeping the child-side write handles synchronous and
  inheritable;
- `win32_io_reader_new_overlapped()` associates eligible read handles with a
  shared IOCP and reports completions through the existing service completion
  bridge;
- pane and job output keep the explicit read, EOF, and error event semantics
  already used by the worker-backed reader API;
- cancellation waits for any pending overlapped read to complete before freeing
  the endpoint, so the IOCP thread cannot enqueue stale endpoint pointers after
  teardown;
- worker-backed readers remain in use for client console input, direct tty
  input, and client file reads;
- writers still use the worker-thread backend, and process waits still use
  `RegisterWaitForSingleObject`.

This is the first real backend replacement slice. It validates the endpoint
model against the two output streams most affected by process-exit-versus-EOF
ordering, while intentionally leaving stdin writers and console/file readers on
the existing backend until their handle-specific semantics are migrated.

## Overlapped Pipe Writer Backend Slice

Pane and job stdin writers now use an IOCP-backed overlapped write backend:

- the named-pipe helper can create parent-side input write handles with
  `FILE_FLAG_OVERLAPPED`;
- ConPTY pane stdin and Win32 job stdin use overlapped parent-side write
  handles while keeping the child-side read handles synchronous and inheritable;
- `win32_io_writer_new_overlapped()` associates eligible write handles with the
  shared IOCP and reports write-drained, write-closed, and error completions
  through the existing service bridge;
- the service keeps one overlapped write in flight per writer endpoint, leaving
  queued bytes in the writer evbuffer until the IOCP completion proves they
  were written;
- writer drain and close predicates account for in-flight writes, preserving
  the existing stdin-close rule that queued input is flushed before the handle
  is closed;
- cancellation waits for a pending overlapped write to complete before freeing
  the endpoint;
- console, tty, and client file writers remain on the worker-backed writer
  path because they need console-specific UTF-8/`WriteConsoleW` behavior or are
  not yet opened as overlapped handles;
- process waits still use `RegisterWaitForSingleObject`.

This removes the most important remaining synchronous pipe write path from the
server-side pane and job lifecycle without changing the public tmux stdin
acceptance contract.

## IOCP Endpoint Teardown Hardening Slice

IOCP-backed endpoints now use their completion event as a stronger lifetime
barrier:

- starting an overlapped read or write resets the endpoint completion event;
- completion handlers only signal the event when no replacement operation was
  chained;
- endpoint free marks the endpoint as stopping, cancels any pending operation,
  and waits for the completion event before deleting endpoint state;
- this covers both a pending kernel operation and an IOCP completion handler
  that has already dequeued the completion but has not yet returned.

This prevents teardown from freeing reader or writer endpoint memory while the
IOCP thread can still reference it.

## Overlapped File Endpoint Slice

Path-backed Win32 client file transfers now use IOCP-backed regular-file
endpoints:

- the IOCP reader and writer backends can issue offset-aware operations for
  regular files while preserving the offsetless pipe mode used by pane and job
  pipes;
- path-backed `MSG_READ_OPEN` opens regular files with `CreateFileW` and
  `FILE_FLAG_OVERLAPPED`, then sends `MSG_READ` and `MSG_READ_DONE` from the
  shared service reader instead of a synchronous client event-path read loop;
- path-backed `MSG_WRITE_OPEN` opens regular files with `CreateFileW` and
  `FILE_FLAG_OVERLAPPED`, then sends `MSG_WRITE_ACK` only after IOCP write
  completions drain bytes to the destination;
- append writes use the Windows append-offset sentinel while normal writes
  advance an explicit endpoint offset after each successful completion;
- stdin, stdout, stderr, and console text output remain on the existing
  worker-backed borrowed-handle path so console UTF-8 and CRLF behavior is not
  changed by the regular-file migration.

This removes regular filesystem transfers from the blocking worker backend
without changing the file-transfer protocol window, ACK, close-after-drain, or
stdio/console behavior.

## Process Wait State Hardening Slice

Process wait endpoints now carry explicit process-wait state around the
existing `RegisterWaitForSingleObject` backend:

- a process wait endpoint transitions from running to exited only once, so the
  threadpool wait callback and delayed job-exit replay both use the same
  one-shot process-exit event edge;
- process endpoint dispatch checks the endpoint state before delivering
  `WIN32_IO_EVENT_PROCESS_EXIT`, instead of treating any queued process event
  as sufficient by itself;
- endpoint free marks the process endpoint canceled before deactivation and
  wait unregistration, preventing delayed replay from delivering after
  teardown;
- `win32_io_process_notify()` now updates the same process state before
  enqueueing a replayed process-exit event for jobs that register an exit
  callback after the process has already exited.

This does not replace the threadpool wait backend with IOCP. It tightens the
state/lifetime contract so process waits behave more like the read and write
endpoints while preserving the existing process-exit delivery mechanism.

## Explicit Worker Fallback API Slice

Remaining worker-backed endpoints are now named as explicit fallbacks:

- `win32_io_reader_new_worker()` is used only for handles that still require
  the blocking worker backend, currently client console input, direct tty
  input, and stdio file reads;
- `win32_io_writer_new_worker_borrowed()` is used only for borrowed handles
  that still require the blocking worker backend, currently client console
  output, direct tty output, and stdout/stderr file writes;
- the generic owned worker writer constructor has been removed from the public
  Win32 platform surface because no caller needs it after pane, job, and
  path-backed file writes moved to overlapped endpoints;
- this makes the remaining non-IOCP paths visible at each call site instead of
  hiding them behind generic reader/writer constructor names.

This is an API clarity cleanup, not a backend migration. The worker fallback is
still intentional for console and stdio handles until a dedicated console
proactor preserves the current UTF-8 and `WriteConsoleW` behavior.

## Dead Pipe Helper Removal Slice

The stale synchronous pipe wrapper has been removed from the Win32 pane/job
implementation:

- pane and job creation already uses `win32_make_input_pipe()` and
  `win32_make_output_pipe()`, which request overlapped parent-side handles for
  the I/O service endpoints;
- the old `win32_make_pipe()` wrapper only created a synchronous pair and had
  no callers after the overlapped pipe migration;
- removing it eliminates a pre-IOCP helper and keeps the remaining pipe
  creation API aligned with endpoint direction and backend requirements.

This is intentionally narrow cleanup. The underlying named-pipe helper remains
because it still creates the direction-specific overlapped pipe pairs used by
ConPTY panes and Win32 jobs.

## Input Queue Cap Slice

Win32 pane and job stdin now have a hard cap across tmux-side queues plus the
service writer queue:

- pane input readiness now checks Win32 stdin writer state and the combined
  pending byte count before accepting normal key and paste input;
- `win32_pane_write()` refuses writes that would exceed the cap with `EAGAIN`
  instead of allowing the pane-owned queue to grow without bound;
- `win32_job_write()` applies the same cap to job stdin, covering popup and
  pipe-pane style producers that feed job input through tmux-side buffers;
- `paste-buffer` now propagates pane input write failure as a command error,
  so the largest direct pane input command does not silently ignore a capped
  Win32 stdin queue;
- the IOCP writer ordering and close-after-drain behavior are unchanged. This
  slice only bounds the compatibility queues that exist above the service
  writer because several tmux input producers still have fire-and-forget
  write semantics.

This is a defensive backpressure step, not full retry semantics for every pane
input producer. Direct callers that ignore `win32_pane_write()` failures can
still drop input under sustained overload, but they no longer grow server
memory indefinitely.

## Pipe Pane Input Backpressure Slice

Win32 `pipe-pane -I` now treats pane stdin backpressure as a retryable edge
instead of draining helper-job output after a failed pane write:

- `job_set_reading()` lets callers pause and resume job stdout reads through
  the normal job abstraction;
- Win32 jobs implement that pause through `win32_job_set_reading()`, which
  controls the service reader attached to the job output endpoint;
- `pipe-pane -I` checks pane input readiness before moving helper-job output
  into pane stdin;
- when pane input is saturated, the pipe job output reader is paused and a
  short tmux-thread retry timer is armed;
- the retry path attempts to deliver already buffered helper-job output before
  resuming new job reads, so the service does not keep pulling from the helper
  while pane stdin remains backpressured;
- pipe job completion makes one final no-pause delivery attempt and logs if
  pane input is still saturated.

This gives live `pipe-pane -I` streams bounded read-side behavior while
preserving the existing job lifetime model. It does not keep a completed helper
job alive indefinitely solely to wait for future pane stdin capacity; the final
completion edge still drops undelivered helper output if the pane remains
saturated after the job has exited.

## Pipe Pane Helper Ownership Slice

Win32 `pipe-pane` teardown now owns the helper job until cleanup is complete:

- `window_pane_close_pipe()` centralizes pipe close logic for both Unix pipe
  fds and Win32 helper jobs;
- the Win32 path detaches `wp->pipe_job`, clears `pane_pipe_pid`, and frees the
  helper job, which terminates the process tree through the existing Win32 job
  object cleanup;
- `pipe-pane` toggle-off and pane destruction both use the same helper instead
  of only closing helper stdin and losing ownership;
- callbacks that arrive during teardown see the pane as no longer owning that
  job and discard any remaining helper output through the normal stale-job
  path.

This fixes the previous Win32-only leak where `pipe-pane -I` or bidirectional
helpers could survive after the pane stopped owning them. It intentionally
chooses kill-on-detach semantics for Win32 helper jobs, matching the fact that
tmux no longer has a logical pipe owner after `wp->pipe_job` is cleared.

## Console UTF-8 Writer Slice

Worker-backed console writers now preserve UTF-8 sequences across service
write chunks:

- `struct win32_handle_writer` keeps a bounded trailing UTF-8 partial sequence
  for console handles;
- console writes decode only complete UTF-8 prefixes with
  `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`;
- an incomplete trailing sequence is retained for the next write instead of
  forcing a raw `WriteFile()` fallback;
- invalid complete byte sequences are converted through Windows' replacement
  behavior and still go through `WriteConsoleW`, avoiding raw byte output to a
  console handle;
- non-console worker writes continue using `WriteFile()` unchanged.

This fixes the console relay class where a `MSG_WIN32_TTY_OUTPUT` boundary or
writer chunk boundary could split a multibyte UTF-8 character and produce
mojibake. The server-side ACK contract is unchanged: bytes are acknowledged only
after the writer drains them into either `WriteConsoleW` or the bounded
one-sequence pending state that must be completed by a later chunk.

## Worker Teardown Bound Slice

Worker-backed endpoints no longer wait forever during endpoint free:

- reader and writer worker frees still signal stop and call
  `CancelSynchronousIo()` for the worker thread;
- the subsequent thread join is bounded by `WIN32_WORKER_STOP_TIMEOUT`;
- if the worker does not stop in time, the endpoint is deactivated and the
  backend object is intentionally left allocated, avoiding a server-thread hang
  and avoiding use-after-free from a still-running worker thread;
- successful worker shutdown still frees handles, events, buffers, and endpoint
  memory normally.

This slice only applied to the explicit worker fallback backend used for
console, direct tty, and stdio handles. IOCP endpoint teardown was hardened in
the later IOCP teardown slice, and process waits still use the existing
`RegisterWaitForSingleObject` backend.

## IOCP Endpoint Teardown Bound Slice

IOCP-backed endpoints now have the same no-unbounded-server-wait rule as the
worker fallback backend:

- reader and writer frees still cancel outstanding overlapped I/O through
  `CancelIoEx()`;
- the wait for the endpoint completion event is bounded instead of using an
  infinite wait on the tmux thread;
- if a canceled overlapped operation does not report completion in time, the
  endpoint is deactivated and intentionally leaked so a later IOCP completion
  cannot use freed memory;
- the IOCP service thread now exits only on the explicit service-shutdown
  sentinel packet, so canceled-operation completions with a null overlapped
  pointer cannot accidentally stop the backend thread;
- this keeps the existing IOCP backend and completion contract, but removes
  another teardown path that could hang the server.

## Service Wakeup Coalescing Slice

The shared Win32 I/O service wakeup is now level-triggered instead of
one-byte-per-completion:

- the service tracks `notify_pending` under the same lock as the pending
  endpoint queue;
- enqueueing the first pending endpoint sends one byte to wake libevent and
  marks the service as notified;
- additional completions only update endpoint event masks while the wakeup is
  pending, avoiding notify socket growth under completion bursts;
- `WSAEWOULDBLOCK` on wakeup send is treated as already-notified because a full
  notify socket necessarily has readable wakeup bytes;
- the tmux-thread service callback drains the notify socket and clears
  `notify_pending` before dispatching all queued endpoint completions.

This prevents buffered reader/writer/process completions from being stranded if
the AF_UNIX wakeup socket reaches its nonblocking send limit.

## Console Input Queue Slice

The Win32 console relay input path now keeps client-side bytes in an owned
pending buffer before sending them to the server:

- console input drained from the service reader is appended to
  `client_win32_input_pending` instead of being sent directly from a temporary
  buffer;
- delivery still uses the existing `MSG_WIN32_TTY_INPUT` protocol and splits
  messages at the imsg payload limit;
- bytes are drained from the pending buffer only after `proc_send()` accepts the
  corresponding message;
- if the peer cannot accept a queued input message, the client records a lost
  server condition and exits instead of silently dropping the remaining
  already-read console input;
- the Win32 input reader is paused whenever pending input remains owned by the
  client, keeping the service reader from continuing to pull console input after
  IPC delivery has failed.

This does not add a new IPC writable callback because `proc_send()` currently
reports only queueing success or hard failure, not socket-level backpressure.
The slice fixes the data-loss edge in the existing contract by making unsent
input ownership explicit and making failure terminal rather than silent.

## Terminal Output Deferral Slice

Win32 terminal output no longer recursively re-enters `tty_write_callback()` to
continue draining buffered output:

- `tty->event_out` is initialized for the active console relay path and for
  server-side direct Win32 output handles;
- Win32 output progress now schedules a zero-timeout libevent callback through
  `tty_write_schedule()` instead of calling the write callback directly;
- ACK-driven console relay backpressure is preserved: new output is scheduled
  after `MSG_WIN32_TTY_OUTPUT_ACK` frees pending capacity;
- direct Win32 output-handle drains still resume from the service writer event
  callback, but continuation is deferred through libevent instead of stack
  recursion;
- Unix fd output scheduling remains unchanged and still uses the writable fd
  event.

This keeps all terminal output continuation on the tmux event loop, avoids
unbounded callback recursion during large redraws, and leaves the existing
Win32 service writer/console ACK accounting intact.

## Console Close ACK Slice

The active Win32 console relay path now waits for outstanding console-output
ACKs before completing terminal close:

- `tty_close_graceful()` treats `client->win32_tty_out_pending` as a close
  barrier for console-relay clients, matching the existing direct Win32 writer
  drain barrier;
- `tty_stop_tty()` may enqueue terminal reset bytes through `tty_raw()`, and
  those bytes are included in `win32_tty_out_pending`;
- `MSG_EXITED` is deferred while console relay output remains unacknowledged;
- `MSG_WIN32_TTY_OUTPUT_ACK` retries the graceful close once pending console
  output reaches zero.

This prevents the server from telling the client to exit before the active
console relay has acknowledged teardown/reset output that was already handed to
the client-side I/O service writer.

## Local File Endpoint Slice

Win32 local path-backed file commands now use I/O service endpoints instead of
blocking CRT reads and writes on the tmux server thread:

- server-local `file_write()` opens path-backed destinations with `CreateFileW`
  and `FILE_FLAG_OVERLAPPED`, then feeds data through a
  `win32_io_writer_new_file_borrowed()` endpoint;
- server-local `file_read()` opens path-backed sources the same way and drains a
  `win32_io_reader_new_file()` endpoint into the existing file buffer;
- command callbacks still complete through the existing `file_fire_done()` and
  `file_fire_read()` contract, so callers such as `load-buffer`, `save-buffer`,
  and `source-file` keep their asynchronous `CMD_RETURN_WAIT` behavior;
- large local writes are fed to the service writer in bounded chunks rather
  than attempting to queue the entire file into the writer at once;
- stdio `-` paths remain on the existing client/stdio path because they are not
  path-backed regular files.

This removes another class of direct server-thread filesystem I/O from the
Win32 port and reuses the same IOCP regular-file backend already used for
client file-transfer messages.

## Stdio Read Endpoint Slice

Win32 client-side file reads from inherited stdin now use the I/O service
reader instead of the Unix fd bufferevent path:

- `file_read_open()` now calls `file_read_win32_start()` for both path-backed
  reads and `STDIN_FILENO` reads on Win32;
- path-backed regular files continue to use the IOCP regular-file reader;
- stdin handles use the explicit worker reader fallback through
  `win32_io_reader_new_worker()`, matching the fallback policy for inherited
  stdio handles;
- read data and read completion still use the existing `MSG_READ` and
  `MSG_READ_DONE` protocol.

This removes the last Win32 client file-read path that tried to treat an
inherited Windows stdio handle as a normal libevent fd.

## File Read Send Failure Slice

Win32 client-side file reads now treat IPC send failure as a terminal file-read
error instead of silently draining already-read bytes:

- `file_read_win32_callback()` reports failure if `proc_send(MSG_READ)` cannot
  queue a read-data message to the peer;
- the event callback funnels that failure through the same close path as
  EOF/error/cancel, avoiding a second close from inside the read-drain helper;
- `MSG_READ_DONE` reports the recorded `EIO` instead of replacing it with the
  reader backend state;
- bytes are only drained from the service reader's temporary buffer after the
  corresponding `MSG_READ` has been accepted by the IPC layer.

This hardens the I/O service migration boundary: once data has moved from the
Win32 reader endpoint into the tmux file-transfer protocol, failure to hand it
to IPC is no longer hidden as a successful read.

## Startup Config Read Slice

Win32 startup configuration file reads now use the I/O service regular-file
path instead of synchronous `fopen()` on the server thread:

- `start_cfg()` resolves the startup config file list before asynchronous
  reads begin, preserving relative-path behavior even if the first client later
  disconnects;
- detached Win32 server startup carries explicit client `-f` arguments into the
  spawned native server process, so the async reader sees the same startup
  config list that the client requested;
- each startup config file is read through `file_read(NULL, ...)`, reusing the
  Win32 `CreateFileW` plus `win32_io_reader_new_file()` path used by
  `source-file`;
- parsed command lists are stored and appended to the global command queue only
  after all startup config reads complete, preserving the old behavior where
  every startup config file was parsed before any config command ran;
- parse-time format expansion still sees the initial client while it is alive,
  but a client-lost hook clears the startup client pointer before free;
- the existing `cfg_done` callback remains the startup barrier that releases
  the first client command queue.

This removes another direct server-thread filesystem read from the Win32 port
without changing the startup command ordering model.

## Popup Editor File Slice

Win32 popup editor temp-file I/O now uses the I/O service path:

- initial popup editor contents are written with `file_write(NULL, ...)` instead
  of synchronous `fdopen()` plus `fwrite()` on the tmux server thread;
- edited popup contents are read back with `file_read(NULL, ...)` instead of
  synchronous `fopen()` plus `fread()` from the popup close callback;
- the popup editor state keeps a client reference while the initial async write
  is pending, so the delayed popup open cannot dereference a freed client;
- Win32 editor temp files are created under `TMP`, `TEMP`, or `USERPROFILE`
  instead of relying on the stale `_PATH_TMP` / `C:/Temp` fallback;
- the editor process runs in that temp directory and receives only the temp
  file basename, avoiding a new shell-command quoting dependency on full
  Windows temp paths with spaces.

The Unix popup editor path remains synchronous and unchanged in backend policy.

## Prompt History Load Slice

Win32 startup prompt-history loading now uses the I/O service path:

- `cfg_done()` keeps the first-client startup barrier in place while prompt
  history is loaded asynchronously;
- `status_prompt_load_history()` reads the configured history file through
  `file_read(NULL, ...)` on Win32, reusing the regular-file I/O service reader;
- the completion callback parses the final accumulated buffer, then releases
  both the first-client barrier and the waiting global queue item;
- missing, invalid, or unreadable history files continue to be logged at debug
  level without failing server startup;
- the Unix load path remains synchronous and unchanged in backend policy.

## Prompt History Save Slice

Win32 shutdown prompt-history saving now uses the I/O service path:

- `status_prompt_save_history()` serializes prompt history into an evbuffer
  instead of writing each line through `fputs()` on Win32;
- the configured history file is written through `file_write(NULL, ...)`,
  reusing the regular-file I/O service writer and its close-after-drain
  semantics;
- server shutdown pumps the existing libevent loop until the write completion
  callback fires, so `exit(0)` is reached only after the service has reported
  the file handle closed or failed;
- empty history still truncates the configured file through the same path;
- the Unix save path remains synchronous and unchanged in backend policy.
