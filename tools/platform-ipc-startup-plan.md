# Platform IPC Startup Abstraction Plan

Date: 2026-05-16

Status: Design

Related docs:

- [`tools/win32-port-findings.md`](win32-port-findings.md)
- [`tools/win32-auth-plan.md`](win32-auth-plan.md)
- [`tools/win32-io-service-plan.md`](win32-io-service-plan.md)

## Goal

Replace the current Unix-owned startup policy plus Win32 add-ons with one
cross-platform abstraction for:

- socket-path resolution;
- endpoint identity;
- startup coordination;
- stale endpoint recovery;
- listener creation and cleanup.

The shared tmux call sites should use one abstract API. They must not care
whether the backend uses `flock`, `LockFileEx`, a mutex, or any other
platform-specific primitive.

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
to imitate pieces of it out of band. The result is exactly the drift the port is
showing now:

- raw path strings are used as identity in one place and filesystem objects in
  another;
- `CLIENT_NOFORK` is coordinated on Unix but bypasses coordination on Win32;
- listener creation and stale cleanup are not part of one shared state machine;
- the Win32 startup lock is not attached to the endpoint namespace it protects;
- shared code still knows too much about lock implementation details.

This is not a "Win32 needs one more fix" problem. It is a layering problem.

## Design Principles

- Shared policy, platform backend.
- Resolve endpoint identity once, then reuse it everywhere.
- Shared code must not manipulate lock files, mutex names, or raw unlink rules.
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

Introduce a shared "endpoint startup" layer with two opaque concepts:

- `struct ipc_endpoint`
- `struct ipc_startup_guard`

The shared layer owns the state machine. Platform code only supplies backend
operations.

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

### `struct ipc_startup_guard`

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
- or something else.

## Shared Backend Contract

The abstraction should split into one shared policy layer and one platform
backend contract.

The shared layer needs backend operations with this shape:

1. Resolve a requested socket path into a canonical endpoint path.
2. Prepare the default socket root if the path came from `-L` / default label.
3. Acquire startup coordination for the canonical endpoint.
4. Release startup coordination.
5. Attempt one client `connect`.
6. Attempt one raw listener create without deleting an existing endpoint first.
7. Detect whether an existing endpoint is stale enough to remove.
8. Remove a stale endpoint once the shared policy authorizes it.
9. Register cleanup ownership for a successfully created listener.

The important separation is:

- backend answers "how do I do this on this OS?"
- shared layer answers "when is this action allowed, and in what order?"

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
4. Acquire `startup_guard(endpoint)`.
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

- `client_get_lock()` stops being a Unix-only special case in `client.c`;
- `win32_ipc_startup_lock()` stops being called directly from `client.c`;
- `CLIENT_NOFORK` no longer bypasses coordination on Win32.

### 3. Server listener creation

Listener creation also needs one shared flow instead of raw platform branches in
`server.c`.

Target flow:

1. The server already has an `ipc_endpoint`.
2. The server already holds the startup guard for that endpoint.
3. Shared code asks the backend to try one raw listener create.
4. If the backend reports success, register cleanup ownership and continue.
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

## Recommended Win32 Backend Change

The abstraction may support either a mutex backend or a file-lock backend, but
the final Win32 implementation should not keep the current hashed global mutex
as the long-term startup primitive.

### Why the current mutex is weak

The current Win32 mutex is weak for structural reasons:

- it is named from a path string hash, not from a filesystem object;
- equivalent path aliases can map to different mutexes;
- it is not naturally tied to endpoint cleanup or stale detection;
- detached-server handoff is awkward because the mutex lease lives in the
  spawning client, not in the endpoint namespace.

That is the opposite of why the Unix side is strong.

### Recommended direction

For the final redesign, Win32 should move to a filesystem-backed startup lease
derived from the endpoint path, for example:

- canonical endpoint path `X`
- startup lease path `X.lock`
- backend primitive `CreateFileW` plus `LockFileEx`

This gives Win32 the same structural strengths Unix already benefits from:

- the coordination object lives in the same namespace as the endpoint;
- aliasing pressure is reduced once canonical path resolution is shared;
- startup ownership and cleanup become easier to reason about;
- server-start handoff becomes possible without exposing raw lock details at the
  call site.

If a compatibility stage temporarily wraps the current mutex under the new
guard API, that is acceptable as a transition only. It should not be the final
model.

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
struct ipc_startup_guard;

struct ipc_endpoint *ipc_endpoint_resolve(const char *, const char *, uint64_t *,
    char **);
const char *ipc_endpoint_path(const struct ipc_endpoint *);
void ipc_endpoint_free(struct ipc_endpoint *);

int ipc_client_connect_or_start(struct event_base *, struct tmuxproc *,
    struct ipc_endpoint *, uint64_t);

int ipc_startup_guard_acquire(struct ipc_endpoint *,
    struct ipc_startup_guard **, char **);
void ipc_startup_guard_release(struct ipc_startup_guard *);

int ipc_server_listener_create(struct ipc_endpoint *,
    struct ipc_startup_guard *, uint64_t, int *, char **);
```

The exact exported surface should stay small. The important rule is that the
shared callers do not work with:

- `.lock` paths;
- `HANDLE` mutexes;
- manual `unlink`;
- path hashes;
- or backend-specific retry rules.

## Implementation Stages

### Stage 1: Introduce the abstraction

- Add a new shared module for endpoint resolution and startup policy.
- Keep behavior as close to current as possible while moving call sites behind
  the abstraction.
- Preserve current auth and path-security behavior.

Success condition:

- `tmux.c`, `client.c`, and `server.c` call the new layer instead of open-coding
  platform-specific startup policy.

### Stage 2: Unify path identity

- Make all socket-path sources create one `ipc_endpoint`.
- Remove raw-path hashing and raw-path retry decisions from shared callers.
- Populate `socket_path` from the endpoint object only.

Success condition:

- connect, startup guard, bind, and cleanup all use one canonical path.

### Stage 3: Remove startup bypasses

- Route `CLIENT_NOFORK` through the same guard flow.
- Make detached and foreground startup use the same connect/retry state machine.
- Add bounded wait or at least explicit timeout diagnostics for startup guard
  waits.

Success condition:

- there is no special startup path that skips coordination on Win32.

### Stage 4: Replace Win32 mutex backend

- Move Win32 startup coordination behind a filesystem-backed lease derived from
  the endpoint path.
- Keep the guard opaque at the shared call sites.

Success condition:

- Win32 endpoint ownership is anchored to the endpoint namespace rather than a
  global string hash.

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
- backend choice stays hidden behind the abstraction.

That is the bar this port needs before smaller Win32 fixes in this area are
worth continuing.
