# Win32 Pane / Job Lifecycle Plan

Date: 2026-05-17

Status: In progress

Related docs:

- [`tools/win32-port-findings.md`](win32-port-findings.md)
- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)
- [`tools/win32-console-relay-plan.md`](win32-console-relay-plan.md)

## Goal

Restore a coherent tmux-shaped lifecycle model for Win32 panes and jobs now
that the Win32 I/O service can already distinguish:

- root process exit;
- reader EOF versus reader error;
- buffered output still waiting inside tmux;
- writer drain and half-close state.

This plan is not about transport redesign. It is about making pane and job
ownership use those existing primitives correctly instead of collapsing them
back into ad hoc kill-on-close behavior.

## Core Rule

Object destruction must stop being process policy.

Today too much Win32 cleanup still assumes:

- "free the pane/job object" means "terminate the process tree";
- "output side closed" means "pane exited";
- "close the pipe helper" means "kill the helper right now".

The final model must separate:

1. process state;
2. transport drain state;
3. passive resource cleanup; and
4. explicit forced termination.

That is the structural change this plan is defining.

## Existing Useful Primitives

The codebase already has the low-level signals needed for this redesign:

- [`win32-event.c`](../win32-event.c) delivers
  `WIN32_IO_EVENT_PROCESS_EXIT` separately from reader EOF and reader error;
- [`win32-event.c`](../win32-event.c) exposes
  `win32_io_reader_done()` and `win32_io_reader_eof()` so callers can tell
  "reader no longer running" apart from "reader reached clean EOF";
- [`win32-conpty.c`](../win32-conpty.c) already tracks process exit for panes
  and jobs internally;
- [`job.c`](../job.c) already has a partial dead-versus-output-complete split
  for Win32 jobs through `JOB_DEAD`.

So this work does not need a new reactor, a new worker model, or a second
transport abstraction. It needs the lifecycle layer above those primitives to
use them deliberately.

## Current Mismatch

### 1. Close helpers still hard-kill

Relevant code:

- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_close()`
- [`win32-conpty.c`](../win32-conpty.c): `win32_job_close()`
- [`server-fn.c`](../server-fn.c): `server_destroy_pane()`
- [`spawn.c`](../spawn.c): respawn cleanup
- [`job.c`](../job.c): `job_free()`

Today the backend close helpers always call `win32_child_kill()`. That means
natural completion cleanup, remain-on-exit cleanup, helper teardown, popup/job
replacement, and explicit kill requests all reuse the same destructive path.

### 2. Pane exit is still overloaded with output EOF

Relevant code:

- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_exit_cb()`
- [`win32-conpty.c`](../win32-conpty.c): `win32_pane_output_event_cb()`
- [`window.c`](../window.c): `window_pane_error_callback()`
- [`spawn.c`](../spawn.c): respawn active check

The current tree delays `PANE_EXITED` until output EOF to avoid losing ConPTY
tail output. That fixed an output-loss bug, but it also made "process exited"
and "pane transport drained" indistinguishable again at the public pane flag
level.

### 3. Destroy readiness ignores `pipe-pane` helper lifetime

Relevant code:

- [`window.c`](../window.c): `window_pane_destroy_ready()`
- [`window.c`](../window.c): `window_pane_close_pipe()`
- [`cmd-pipe-pane.c`](../cmd-pipe-pane.c): Win32 helper callbacks

Ordinary Win32 pane destruction still does not wait for `wp->pipe_job`, while
the shared pipe-close helper still destroys the helper job immediately.

### 4. Generic job freeing still implies terminate

Relevant code:

- [`job.c`](../job.c): `job_free()`
- [`job.c`](../job.c): `job_complete()`
- [`popup.c`](../popup.c): popup teardown
- [`format.c`](../format.c): format-job replacement

Natural Win32 job completion currently flows through `job_free()`, which still
calls `win32_job_close()`. So even the successful completion path is still
shaped like explicit cancellation.

## Non-Goals

This plan does not:

- redesign relay versus direct-handle transport;
- redesign auth or ACL semantics;
- change the Win32 path policy;
- redefine tmux's generic Unix pane/job lifecycle;
- promise a full generic state-machine rewrite if smaller shared predicates are
  enough.

## Lifecycle Vocabulary

This plan uses the following terms explicitly.

### 1. Process exited

The root pane or job process has exited and exit status is known.

This is about child liveness, not about whether all output has been consumed by
tmux yet.

### 2. Output drained

The backend reader has reached a terminal state and tmux has no buffered output
left to parse or deliver for that pane/job.

On Win32 this is a distinct property from process exit.

### 3. Passive cleanup

Free the tmux-side backend resources for a pane or job without treating the
child as something to kill.

Examples:

- free handles after natural process exit and output drain;
- free a helper job after it completed normally;
- free a pane backend during final pane destruction after all lifecycle gates
  were already satisfied.

### 4. Explicit terminate

Force the child process tree to stop because the caller intentionally wants it
gone, or because the backend has entered an irrecoverable broken state.

Examples:

- `kill-pane`;
- `kill-window`;
- popup close that intentionally cancels its helper job;
- format-job replacement that intentionally abandons the previous job.

### 5. Broken transport

The pane or job output transport ended in a way that does not represent normal
child completion.

For Win32 this must not silently masquerade as graceful process exit. It is a
backend failure path that may require explicit terminate or dedicated logging.

## Target Contract

### Pane Model

### Public pane flags

The intended meaning should be:

- `PANE_STATUSREADY`: pane exit status is known;
- `PANE_EXITED`: root pane process has exited;
- output drain remains a backend predicate, not a second overloaded use of
  `PANE_EXITED`.

This deliberately restores `PANE_EXITED` to the Unix-shaped meaning of "the
pane process is dead" while keeping Win32 output-drain completion explicit.

### Pane predicates

Win32 pane callers need at least three distinct questions:

1. Is the process dead?
   This should drive `window_pane_exited()`, dead-pane user behavior, and
   logic that must stop sending input to a dead child.

2. Is the pane backend quiescent?
   This means process exited, reader reached a terminal state, and no buffered
   output remains inside tmux.

3. Is pane destruction allowed?
   This means the backend is quiescent and any owned pipe helper is also in an
   allowed teardown state.

Respawn without forced kill must use the quiescent predicate, not just the
process-dead predicate. Otherwise the old tail-output loss bug returns under a
different name.

### Output failure policy

If Win32 output closes before process exit is known, that is not a normal dead
pane. It is a broken backend edge. The design should log it as such and route
it through an explicit failure policy rather than pretending the pane exited
cleanly.

### Job Model

Jobs need the same separation, but the generic job layer already has part of
the structure:

- `JOB_RUNNING`
- `JOB_DEAD`
- `JOB_CLOSED`

The intended Win32 semantics are:

- process exit moves the job to the dead state;
- output drain completion allows final completion callbacks and passive free;
- explicit cancellation uses a separate terminate path instead of reusing the
  successful completion cleanup path.

The important structural fix is that generic `job_free()` should stop being the
only public way to both cancel and destroy a job. Natural completion and
explicit cancel must not share the same backend operation.

### Pipe Pane Helper Model

This plan distinguishes between two different events that the current code
collapses together.

### 1. Ordinary pane destruction after pane process exit

If the pane still owns a Win32 `pipe-pane` helper, ordinary pane destruction
should wait until that helper reaches its chosen completion point.

That means `window_pane_destroy_ready()` on Win32 must consider `wp->pipe_job`
instead of only the pane output reader.

### 2. Explicit pipe close or toggle-off

When the user explicitly closes the pipe, tmux may detach the pane from that
helper immediately:

- clear `wp->pipe_job`;
- stop any new pane-to-helper flow;
- close helper stdin politely if that direction is active;
- allow stale callbacks to discard future helper stdout because the pane no
  longer owns that job.

The helper may then finish asynchronously and free itself through normal job
completion without still being a pane-owned object.

That is different from ordinary pane destruction and must stay different in the
implementation.

## API Direction

Exact names may change, but the code should move toward explicit lifecycle
operations such as:

```c
void win32_pane_cleanup(struct window_pane *);
void win32_pane_terminate(struct window_pane *);

void win32_job_cleanup(struct win32_job *);
void win32_job_terminate(struct win32_job *);

int win32_pane_process_exited(struct window_pane *);
int win32_pane_quiesced(struct window_pane *);
```

The important point is not the exact identifier. The important point is that
callers choose between cleanup and terminate intentionally instead of getting
terminate as an accidental side effect of freeing an object.

## Call-Site Classification

The implementation should classify current call sites into deliberate groups.

### Explicit terminate callers

- [`server-fn.c`](../server-fn.c): `server_kill_pane()`
- [`cmd-kill-pane.c`](../cmd-kill-pane.c): explicit pane kill commands
- [`popup.c`](../popup.c): popup/job cancellation
- [`format.c`](../format.c): format-job replacement
- [`job.c`](../job.c): `job_kill_all()`
- respawn with explicit kill semantics

These paths may terminate the child tree first and then destroy tmux objects.

### Passive cleanup callers

- [`server-fn.c`](../server-fn.c): `server_destroy_pane()` after natural exit
- [`window.c`](../window.c): final pane object destruction
- [`job.c`](../job.c): normal job completion
- Win32 pane/job spawn failure unwind after the child never became a live owned
  object

These paths must not inherit explicit kill semantics by default.

### Detach-with-grace callers

- [`window.c`](../window.c): `window_pane_close_pipe()`
- [`cmd-pipe-pane.c`](../cmd-pipe-pane.c): explicit pipe close or toggle-off

These paths must break pane ownership cleanly without pretending that the only
correct result is immediate helper termination.

## Implementation Stages

### Stage 1: Define shared lifecycle predicates

- Add explicit Win32 helper predicates for process-exited, output-drained, and
  pane/job quiescent state.
- Rework `window_pane_exited()` on Win32 to answer the process-liveness
  question rather than the reader-EOF question.
- Keep `window_pane_destroy_ready()` as the destroy gate, but make it express
  quiescent state explicitly.

Success condition:

- process death, backend quiescence, and destroy-readiness are no longer the
  same question.

### Stage 2: Split pane terminate from pane cleanup

- Replace the current Win32 pane close helper with separate terminate and
  cleanup operations.
- Make natural pane destruction use cleanup.
- Make explicit kill paths terminate first.
- Route broken transport failure through explicit terminate or another clearly
  named failure path instead of reusing graceful cleanup.

Success condition:

- freeing a dead pane backend no longer implies hard-killing its process tree.

### Stage 3: Fix `pipe-pane` ownership semantics

- Make Win32 destroy-readiness wait for owned helper jobs when the pane is
  dying naturally.
- Change `window_pane_close_pipe()` so explicit pipe close detaches and closes
  helper stdin politely instead of immediately calling `job_free()`.
- Keep stale helper callbacks harmless after detach by letting them see that
  pane ownership is already gone.

Success condition:

- ordinary pane death no longer truncates or kills owned helpers by accident,
  while explicit pipe close still detaches immediately.

### Stage 4: Split generic job cancel from generic job free

- Introduce a generic distinction between explicit job cancellation and final
  passive destruction.
- Make Win32 normal job completion free the backend without implicit
  termination.
- Update popup, format jobs, and kill-all paths to use the explicit cancel
  path.

Success condition:

- successful Win32 job completion is no longer structurally the same as manual
  cancellation.

### Stage 5: Native coverage and failure policy hardening

- Add native Windows coverage for pane tail-output, remain-on-exit, respawn,
  explicit kill, and `pipe-pane` helper teardown.
- Add targeted logging for broken output-before-exit and resize/reader failure
  edges that may still need policy decisions after the main lifecycle split.

Success condition:

- the lifecycle contract is validated in native PowerShell or `cmd`, not just
  by code inspection.

## Test Plan

Build:

- use MSYS2 `bash`
- prepend `/ucrt64/bin:/usr/bin` to `PATH`
- run `make`

Runtime validation:

- use native PowerShell or `cmd`
- do not run tmux itself under MSYS2

Native cases to cover:

1. Natural pane exit with delayed ConPTY tail output:
   - process exits first;
   - tail output still reaches the pane;
   - pane becomes logically dead before backend destruction is allowed.

2. `remain-on-exit`:
   - dead-pane formatting still appears correctly;
   - keypress dismissal still destroys only after the backend is quiescent.

3. Respawn without explicit kill:
   - reject respawn while Win32 output is still draining;
   - allow respawn after the old backend is quiescent.

4. Respawn with explicit kill:
   - terminate the old process tree;
   - do not wait for a normal tail-output path.

5. `kill-pane` and `kill-window`:
   - still terminate promptly;
   - do not depend on passive cleanup semantics.

6. `pipe-pane -O`, `-I`, and `-IO` with ordinary pane death:
   - helper job remains owned until the chosen completion point;
   - no unintended truncation or early helper kill.

7. Explicit `pipe-pane` close or toggle-off:
   - pane ownership clears immediately;
   - helper stdin closes politely when relevant;
   - later helper callbacks do not crash or resurrect pane ownership.

8. Popup and format jobs:
   - normal completion uses passive free;
   - explicit replacement or close uses explicit cancel.

## Relationship To Earlier I/O-Service Notes

[`tools/win32-io-service-plan.md`](win32-io-service-plan.md) recorded an
intermediate Win32 `pipe-pane` ownership fix that intentionally chose
kill-on-detach semantics to close a leak.

That was a valid containment step, but it is not the intended final lifecycle
model. This plan supersedes that narrow choice and defines the final ownership
split between:

- ordinary pane destruction;
- explicit pipe close; and
- explicit process termination.

## Recommended Implementation Order

1. Restore the state model first: process exit, quiescent backend, and destroy
   readiness must become distinct predicates.
2. Split pane terminate versus cleanup next, because pane destruction currently
   drives the most user-visible process loss.
3. Fix `pipe-pane` ownership on top of that state model.
4. Then split generic job cancellation from passive job free.
5. Only after the model is stable, add broader native regression coverage.

This order matters. If Win32 keeps the current overloaded meaning of close and
exit while trying to patch one call site at a time, the result will just be a
new layer of special cases around the same broken boundary.
