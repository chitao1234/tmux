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
- the old child polling pass remains as a temporary fallback while the migration
  continues.

The next cleanup should remove the fallback polling once process events have
enough runtime coverage, then move the pipe read/write backends from blocking
worker threads to overlapped I/O/IOCP where the handle type allows it.
