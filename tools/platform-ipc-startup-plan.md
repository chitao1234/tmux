# Platform IPC Startup Abstraction Plan

Date: 2026-05-16

Status: Redesign

Related docs:

- [`tools/win32-port-findings.md`](win32-port-findings.md)
- [`tools/win32-auth-plan.md`](win32-auth-plan.md)
- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)

## Goal

Replace the current Unix-owned startup policy plus Win32 add-ons with one
cross-platform endpoint service abstraction for:

- socket-path resolution;
- endpoint identity;
- startup coordination and synchronization;
- stale endpoint recovery;
- listener creation and cleanup.

The shared tmux call sites should use one abstract API. They must not care
whether the backend uses `flock`, `LockFileEx`, a mutex, or any other
platform-specific primitive.

This is not a request for "a better Win32 lock." It is a request to stop
letting the Unix startup model define the shared control flow and then bolting
Win32 behavior onto its side.

## Why the Current Structure Is Wrong

Today the product policy is fragmented:

- [`tmux.c`](../tmux.c) resolves the socket path and applies default-path policy
  directly.
- [`client.c`](../client.c) owns the Unix startup state machine and carries a
  separate Win32 autostart path.
- [`server.c`](../server.c) owns raw listener creation and keeps a platform
  branch for `bind()` setup.
- [`win32-ipc.c`](../win32-ipc.c) adds its own startup lock naming, path
  normalization, directory preparation, and unlink-before-bind behavior.

That means Unix currently defines the shape of tmux startup, while Win32 tries
to imitate pieces of it out of band. Even the new shared
[`ipc-startup.c`](../ipc-startup.c) is only a halfway step: it centralizes some
startup sequencing, but it still hardcodes Unix-shaped concepts such as
`path.lock` naming and path-string-based guard acquisition into the shared
layer.

The result is exactly the drift the port is showing now:

- raw path strings are used as identity in one place and filesystem objects in
  another;
- coordination object naming still leaks into shared code;
- `CLIENT_NOFORK` semantics and detached helper semantics remain awkward because
  startup ownership is not modeled explicitly;
- listener creation and stale cleanup are not part of one shared state machine;
- platform code still decides too much policy rather than only supplying
  mechanics.

This is not a "Win32 needs one more fix" problem. It is a layering problem.

## Design Principles

- Shared policy, platform backend.
- Resolve endpoint identity once, then reuse it everywhere.
- Shared code must not manipulate lock files, mutex names, guard file suffixes,
  or raw unlink rules.
- `CLIENT_NOFORK` must not bypass startup coordination.
- No unconditional endpoint delete before bind.
- The loser must not be able to delete the winner's live endpoint.
- Existing AF_UNIX transport stays in place.
- This plan does not redesign auth, ACL, or transport mode selection.
- Explicit `-S` and inherited `$TMUX` paths stay supported in this pass, but
  they must go through the same resolver and startup state machine as default
  sockets.

## Scope

This plan covers:

- default and explicit socket-path resolution;
- canonical endpoint identity;
- startup guard acquisition and release;
- connect/retry/start sequencing;
- stale endpoint validation and removal;
- listener create/listen/cleanup ownership.

This plan does not cover:

- Win32 auth redesign;
- `server-access` semantics;
- console relay versus direct-handle transport policy;
- general path-security product policy changes beyond what is needed for shared
  startup coordination;
- non-IPC locking elsewhere in tmux.

## Core Model

Introduce one shared "endpoint service" layer with three opaque concepts:

- `struct ipc_endpoint`
- `struct ipc_coordination`
- `struct ipc_listener`

The shared layer owns the state machine and the ownership rules. Platform code
only supplies backend operations.

### `struct ipc_endpoint`

This object represents one tmux control endpoint after resolution.

It should carry:

- requested origin: default label, explicit `-S`, or inherited `$TMUX`;
- display path: the path tmux logs and exposes to the user;
- canonical path: the normalized path used for connect, bind, cleanup, and
  guard identity;
- parent directory path if the backend needs it;
- policy flags such as `CLIENT_DEFAULTSOCKET`.

Important rule:

- after resolution, callers stop using raw input spelling;
- connect, startup coordination, bind, and cleanup all use the same canonical
  endpoint identity.

### `struct ipc_coordination`

This is an opaque lease over startup coordination for one endpoint.

Shared code may do only these things with it:

- acquire it for an endpoint;
- hold it across the startup sequence;
- pass it through the server-start path;
- release it.

Shared code must not know whether the backend uses:

- Unix `path.lock` plus `flock`;
- Win32 `CreateFileW` plus `LockFileEx`;
- a named mutex;
- a directory lease;
- or something else.

Important rule:

- the coordination object is a backend implementation detail;
- the shared layer talks only in terms of "acquire coordination for endpoint X"
  and "release or finish coordination for endpoint X".

### `struct ipc_listener`

This is an opaque listener ownership record.

Shared code should not assume that successful listener creation is represented
only by an `fd` plus a pathname string. The backend may need extra ownership
state for cleanup, stale probing, or lifetime rules. The shared layer should
hold that ownership token and ask the backend to destroy it cleanly.

## Shared Backend Contract

The abstraction should split into one shared policy layer and one platform
backend contract.

The shared layer needs backend operations with this shape:

1. Resolve a requested socket path into a canonical endpoint path.
2. Prepare the default socket root if the path came from `-L` / default label.
3. Acquire coordination for the canonical endpoint.
4. Release or finish coordination.
5. Attempt one client `connect`.
6. Attempt one raw listener create without deleting an existing endpoint first.
7. Probe whether an existing endpoint is live, dead, or indeterminate.
8. Remove a stale endpoint once the shared policy authorizes it.
9. Return listener ownership state for successful create.
10. Destroy listener ownership state during cleanup.

The important separation is:

- backend answers "how do I do this on this OS?"
- shared layer answers "when is this action allowed, and in what order?"

The backend contract should be explicit enough that Unix and Win32 implement
the same conceptual operations even when their mechanics differ. For example:

- Unix may implement coordination with `open(path.lock)` plus `flock`.
- Win32 may implement coordination with `CreateFileW(path.lock)` plus
  `LockFileEx`, or with a named mutex during a transition period.
- Shared code should not know or care which backend was selected.

## Shared Startup State Machine

### 1. Path resolution

Socket-path handling should stop being split between `tmux.c` and
`win32-ipc.c`.

New shape:

1. Parse `-S`, `-L`, and inherited `$TMUX` as today.
2. Call one shared resolver to create `struct ipc_endpoint`.
3. The resolver asks the backend to canonicalize the chosen path.
4. The canonical path becomes the one true endpoint path for the process.
5. `socket_path` may remain as a global string for compatibility, but it should
   be populated from the endpoint object, not from raw input text.

That means:

- `-L foo`, `-S C:\x\y`, and inherited `$TMUX` all feed the same resolver;
- slash direction, case folding, relative-path expansion, and default-root
  policy happen once;
- startup locking and bind logic do not get a second chance to reinterpret the
  path differently.

### 2. Client connect-or-start

The startup sequence should become one shared helper used by both Unix and
Win32.

Target flow:

1. Attempt `connect(endpoint)`.
2. If it succeeds, return the connected fd.
3. If server start is not allowed, fail.
4. Acquire `coordination(endpoint)`.
5. Retry `connect(endpoint)` while holding the guard.
6. If it succeeds, release the guard and return the fd.
7. Start the server:
   - foreground path: call `server_start(...)` with the active guard;
   - detached path: spawn the server through the abstraction while the guard
     remains active.
8. Retry connect until the new server is reachable or startup times out.
9. Release the guard on success or failure.

This becomes the only place where startup races are resolved.

Consequences:

- `client_get_lock()` disappears into the endpoint service backend;
- no Win32 startup primitive is called directly from `client.c`;
- `CLIENT_NOFORK` and detached helper startup both become policy cases inside
  the same shared state machine.

### 3. Server listener creation

Listener creation also needs one shared flow instead of raw platform branches in
`server.c`.

Target flow:

1. The server already has an `ipc_endpoint`.
2. The server already holds the coordination lease for that endpoint.
3. Shared code asks the backend to try one raw listener create.
4. If the backend reports success, keep the returned listener ownership token
   and continue.
5. If the backend reports "address already in use", shared code probes for a
   live server.
6. Only if the endpoint is proven stale does shared code authorize backend
   removal and one retry.
7. If the retry still fails, startup fails without deleting anything else.

Important changes:

- client code never unlinks the socket path;
- server code never unlinks first and asks questions later;
- stale removal becomes conditional shared policy, not backend habit.

### 4. Cleanup ownership

Listener cleanup should be treated as endpoint ownership, not just "remember a
string and unlink it later."

The backend may still store cleanup data internally, but the ownership rule
should be:

- only the listener instance that successfully created the endpoint owns cleanup;
- a failed or losing startup attempt must not remove the winner's endpoint;
- cleanup must use the same canonical identity that startup used.

## Synchronization Design

The important redesign is not "choose file lock or mutex first." The important
redesign is:

- define one coordination API;
- make shared code use only that API;
- make the backend responsible for its own coordination naming and lifetime.

### Why the current shape is weak

The current tree is weak even after the recent cleanup because the shared layer
still bakes in backend-specific concepts:

- `ipc-startup.c` still manufactures `.lock` paths itself;
- coordination is still keyed by raw endpoint text passed down from callers
  rather than by a fully resolved endpoint object;
- listener lifetime is still represented mostly as "an fd plus a pathname";
- backend policy and shared policy are still interleaved.

That means we have improved the primitive, but not yet the abstraction.

### What the final abstraction should allow

The backend-neutral coordination contract should permit any of these without
shared-call-site changes:

- Unix file lock backend;
- Win32 file lock backend;
- Win32 named mutex backend, if temporarily needed for transition or testing;
- any future endpoint-scoped lease primitive.

The shared layer should call the same API in every case.

### Recommended Win32 backend preference

The abstraction should not force the decision, but the current Win32 preference
should remain endpoint-scoped file locking rather than a global named mutex,
because it lines up better with endpoint identity, stale handling, and cleanup
ownership.

That is a backend choice under the abstraction, not something the rest of tmux
should know about.

## Canonicalization Rules

The abstraction needs one place to define endpoint identity.

Minimum requirement:

- relative paths become absolute before any connect, lock, or bind decision;
- slash spelling differences stop mattering;
- Win32 case differences stop mattering for endpoint identity;
- default-root paths and explicit `-S` paths both go through the same
  canonicalizer.

For Win32, `GetFullPathNameW()` is the minimum acceptable base.

Open question for later hardening:

- whether reparse-point and final-object aliasing must be collapsed at this
  layer or left to a stricter explicit-path policy pass.

This plan does not need to solve the entire explicit-path security question
first. It does need one canonical path identity so startup coordination is not
built on raw spelling.

## Call-Site Rewrite

The design goal is to make the top-level call sites boring again.

### `tmux.c`

Current role:

- parse `-S`, `-L`, and `$TMUX`;
- build `socket_path` directly;
- special-case Win32 default-root preparation.

Target role:

- parse arguments;
- ask the endpoint abstraction to resolve the socket path;
- store the canonical path result.

### `client.c`

Current role:

- owns Unix retry and lock flow directly;
- branches into separate Win32 lock/spawn logic;
- knows about `CLIENT_NOFORK` bypass behavior.

Target role:

- call one shared `connect-or-start` helper;
- stop knowing which startup primitive the platform uses.

### `server.c`

Current role:

- branches between Unix raw `socket`/`bind`/`listen` and Win32
  `win32_ipc_server_create()`;
- Unix path still unlinks before bind.

Target role:

- call one shared listener-create helper;
- stop hardcoding unlink-before-bind behavior.

### `proc.c` and `win32-proc.c`

Current role:

- detached spawn takes `socket_path` text directly;
- startup coordination is external to spawn semantics.

Target role:

- spawn helpers receive endpoint/guard context through the abstraction;
- detached spawn no longer requires callers to know how startup coordination is
  maintained.

## Suggested API Shape

Exact names may change, but the layering should look like this:

```c
struct ipc_endpoint;
struct ipc_coordination;
struct ipc_listener;

struct ipc_endpoint *ipc_endpoint_resolve(const char *, const char *, uint64_t *,
    char **);
const char *ipc_endpoint_path(const struct ipc_endpoint *);
void ipc_endpoint_free(struct ipc_endpoint *);

int ipc_client_connect_or_start(struct event_base *, struct tmuxproc *,
    struct ipc_endpoint *, uint64_t);

int ipc_coordination_acquire(struct ipc_endpoint *,
    struct ipc_coordination **, char **);
void ipc_coordination_release(struct ipc_coordination *);
void ipc_coordination_finish(struct ipc_coordination *);

int ipc_listener_create(struct ipc_endpoint *, struct ipc_coordination *,
    uint64_t, struct ipc_listener **, int *, char **);
void ipc_listener_destroy(struct ipc_listener *);
```

The exact exported surface should stay small. The important rule is that the
shared callers do not work with:

- `.lock` paths or any guard-path suffix policy;
- `HANDLE` mutexes or file-lock handles;
- manual `unlink`;
- path hashes;
- or backend-specific retry rules.

## Implementation Stages

### Stage 1: Introduce the endpoint service boundary

- Replace raw path-based startup helpers with endpoint objects and opaque
  coordination/listener objects.
- Keep behavior as close to current as possible while moving call sites behind
  the abstraction.
- Preserve current auth and path-security behavior.

Success condition:

- `tmux.c`, `client.c`, and `server.c` call the endpoint service instead of
  open-coding platform-specific startup policy.

### Stage 2: Unify path identity and endpoint ownership

- Make all socket-path sources create one `ipc_endpoint`.
- Remove raw-path coordination naming and raw-path retry decisions from shared
  callers.
- Populate `socket_path` from the endpoint object only.
- Represent successful listener creation with an ownership token, not just a
  pathname string.

Success condition:

- connect, startup guard, bind, and cleanup all use one canonical path.

### Stage 3: Unify startup control flow

- Route `CLIENT_NOFORK` through the same coordination flow.
- Make detached and foreground startup use the same connect/retry state machine.
- Add bounded wait or at least explicit timeout diagnostics for coordination
  waits.

Success condition:

- there is no special startup path that skips coordination on Win32.

### Stage 4: Finalize backend implementations

- Implement Unix and Win32 coordination only behind the backend contract.
- Remove any remaining backend naming policy from shared code.
- Keep coordination opaque at the shared call sites.

Success condition:

- shared code no longer knows whether the backend used a file lock, mutex, or
  anything else.

### Stage 5: Shared stale-endpoint policy

- Remove unconditional unlink-before-bind.
- Probe live endpoint first, authorize stale removal only after the shared state
  machine says it is safe, then retry once.

Success condition:

- a losing or stale-cleanup path cannot delete a winner's live endpoint.

## Testing Plan

Build:

- use local MSYS2 bash with UCRT64 toolchain;
- set `PATH=/ucrt64/bin:/usr/bin:$PATH`;
- run plain `make`.

Windows runtime validation:

- run natively from PowerShell, not under MSYS2;
- smoke `start-server`, `list-commands`, `kill-server`;
- race `tmux -D` against ordinary client autostart on the same socket;
- run many concurrent `start-server` or `list-commands` processes against one
  `-L` label;
- repeat with explicit `-S` paths and slash/case variants that should map to the
  same endpoint identity;
- inject a stale socket path and verify only the stale endpoint is removed;
- verify a losing startup path cannot remove a listener created by the winner.

Unix validation:

- run the normal startup path and explicit `-S` path flow;
- verify lock-and-retry behavior still works under concurrent autostart.

## Expected Outcome

After this redesign:

- Unix no longer owns the startup policy by accident;
- Win32 no longer imitates Unix startup with ad hoc side logic;
- socket-path resolution, startup coordination, stale cleanup, and listener
  creation are one cross-platform subsystem;
- call sites use one abstract API;
- backend choice stays hidden behind the abstraction;
- the same shared code path drives startup whether the backend used `flock`,
  `LockFileEx`, a mutex, or another endpoint-scoped lease primitive.

That is the bar this port needs before smaller Win32 fixes in this area are
worth continuing.
