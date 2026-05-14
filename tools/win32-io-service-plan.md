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
- synchronous `tty_raw()` teardown writes remain a temporary compatibility
  path until terminal shutdown is given explicit flush/cancel semantics.

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
