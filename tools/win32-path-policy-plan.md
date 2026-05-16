# Win32 Path Policy Plan

Date: 2026-05-16

Status: In progress

Related docs:

- [`tools/win32-port-findings.md`](win32-port-findings.md)
- [`tools/platform-ipc-startup-plan.md`](platform-ipc-startup-plan.md)
- [`tools/win32-auth-plan.md`](win32-auth-plan.md)

## Progress Snapshot

Completed in tree:

- shared Win32 path helpers now exist in the generic path layer instead of
  being split across unrelated call sites;
- generic filesystem consumers were migrated to the shared Win32 path policy
  for rooted paths, home and environment expansion, basename/dirname handling,
  and drive-relative rejection;
- Win32 path-list parsing no longer relies on naive Unix `:` splitting for
  filesystem path lists;
- IPC endpoint resolution now tracks endpoint source and endpoint class, and
  Win32 endpoint canonicalization preserves user-visible path spelling instead
  of lowercasing it unconditionally;
- Win32 IPC endpoints now derive a distinct comparison identity, and startup
  coordination now keys off that identity instead of the preserved display
  spelling;
- explicit `-S` and inherited `$TMUX` now validate the parent chain before any
  startup-side mutation for non-managed endpoints, including parent creation,
  `.lock` acquisition, stale cleanup, and listener recovery;
- the checked-in native PowerShell startup smoke now covers inherited `$TMUX`
  startup on an explicit path and unsafe explicit `-S` rejection, and it
  passes again when invoked directly from native PowerShell.

Still pending after that:

- any coordination redesign needed if sibling `.lock` files remain incompatible
  with the final explicit-endpoint trust policy.

## Goal

Stop treating Win32 path handling as scattered Unix compatibility tweaks plus
socket-specific exceptions.

tmux needs one shared path service boundary with a Win32 backend policy that is
used consistently by:

- command-line path parsing;
- cwd resolution and sanitization;
- config and file lookup;
- basename and dirname style operations;
- path-list parsing;
- IPC endpoint resolution and trust decisions.

This plan covers both:

1. general Win32 path semantics across tmux; and
2. Win32 IPC endpoint policy for default `-L`, explicit `-S`, and inherited
   `$TMUX`.

The problem is not only "socket path hardening." The problem is that tmux
currently has multiple incompatible ideas of what a Windows path means.

## Why The Current Structure Is Wrong

The current tree spreads Win32 path policy across several unrelated files:

- [`tmux.c`](../tmux.c) decides what counts as an absolute path, expands `~/`,
  splits path lists, and strips shell basenames.
- [`win32-error.c`](../win32-error.c) has a different cwd resolution policy and
  rejects some paths that [`tmux.c`](../tmux.c) accepts.
- [`cfg.c`](../cfg.c) and [`file.c`](../file.c) each carry their own
  home-relative path logic.
- [`win32-conpty.c`](../win32-conpty.c) already has a better basename helper
  than the generic path code.
- [`ipc-startup.c`](../ipc-startup.c) canonicalizes socket paths separately.
- [`win32-ipc.c`](../win32-ipc.c) normalizes and mutates socket parents using a
  socket-specific policy.

That fragmentation produces visible contradictions:

- [`tmux.c`](../tmux.c) accepts `/foo` as absolute, but
  [`win32-error.c`](../win32-error.c) rejects `/foo` as an invalid Win32 cwd.
- `\foo` behaves as a normal rooted current-drive path on Windows, but tmux
  does not recognize it as absolute.
- `~/...` expands in some places while `~\...` does not.
- path lists still use `:`, which collides with drive letters.
- socket-path canonicalization lowercases text and normalizes slashes, but that
  policy is not shared with the rest of the codebase.
- explicit `-S` paths currently inherit side effects such as sibling `.lock`
  creation and stale unlink without first passing a path-trust policy.

This is not acceptable as a final port. The core path model needs to be
deliberate.

## Design Principles

- One shared path service API, platform backend underneath.
- Product policy first, helper functions second.
- Separate user-visible display spelling from internal comparison identity.
- Do not encode filesystem trust in ad hoc string normalization.
- Do not keep adding Win32 path one-offs to unrelated call sites.
- Filesystem mutation must follow endpoint trust policy, not merely successful
  text canonicalization.
- Win32 support should stay compatible with normal Windows path forms instead
  of forcing users to guess which Unix-shaped subset tmux happens to accept.

## Scope

This plan covers:

- path classification and normalization on Win32;
- cwd resolution on Win32;
- home and environment expansion on Win32;
- basename and dirname style operations used by tmux;
- Win32 path-list parsing for filesystem path lists;
- IPC endpoint path policy and trust classification;
- default managed socket-root policy;
- explicit `-S` and inherited `$TMUX` path handling;
- stale cleanup and startup-coordination path side effects as they relate to
  path trust.

This plan does not cover:

- auth redesign;
- `server-access` ACL semantics;
- relay versus direct terminal transport policy;
- Unicode decoding rules beyond path conversion;
- generic Unix path semantics, except where a shared abstraction boundary is
  required.

## Current Concrete Problems

### 1. General path grammar is inconsistent

Relevant code:

- [`tmux.c`](../tmux.c): `path_is_absolute()`, `expand_path()`,
  `expand_paths()`, `areshell()`, `shell_argv0()`
- [`win32-error.c`](../win32-error.c): `win32_resolve_cwd()`,
  `win32_sanitize_cwd()`, `win32_default_cwd()`
- [`cfg.c`](../cfg.c): `cfg_win32_get_path()`
- [`file.c`](../file.c): `file_get_path()`
- [`format.c`](../format.c): basename and dirname modifiers

The current tree still disagrees about:

- whether `/foo` is valid on Win32;
- whether `\foo` is valid on Win32;
- whether `~\foo` is a home-relative path;
- whether `$VAR\foo` is a valid environment-relative path;
- which separator basename logic understands;
- which separator path-list splitting uses.

### 2. Socket policy is stronger for default paths than explicit paths

Relevant code:

- [`ipc-startup.c`](../ipc-startup.c): `ipc_default_path_win32()`,
  `ipc_endpoint_canonicalize_win32()`
- [`win32-ipc.c`](../win32-ipc.c): `win32_default_socket_dir()`,
  `win32_ipc_make_managed_root()`, `win32_ipc_ensure_socket_dir()`,
  `win32_ipc_server_create()`

The managed default root under `LOCALAPPDATA` is special-cased and secured.
Explicit `-S` and inherited `$TMUX` paths are accepted after text
canonicalization, but listener creation still auto-creates parent directories
and startup still uses sibling `.lock` files and stale unlink at the endpoint
path.

That means text acceptance and filesystem trust are still coupled too loosely.

### 3. Socket identity is still text-shaped

Relevant code:

- [`ipc-startup.c`](../ipc-startup.c): `ipc_endpoint_canonicalize_win32()`

Current canonicalization uses:

- `GetFullPathNameW()`;
- slash normalization;
- unconditional lowercasing.

That is useful for alias collapse, but it is not a complete identity or trust
model. It also makes user-visible spelling drift from input spelling and may
become actively wrong in case-sensitive directory configurations.

### 4. Startup coordination still assumes sibling path mutation

Relevant code:

- [`ipc-startup.c`](../ipc-startup.c): Win32 coordination currently uses
  `path.lock`

This matters to path policy because explicit socket paths currently require
extra mutation rights in the endpoint's parent directory just to participate in
startup races. That is a product choice, not a law of AF_UNIX.

## Product Policy

The final implementation should make the following Win32 rules explicit.

### 1. Accepted filesystem path forms on Win32

Accept these as absolute or rooted path forms:

- drive-qualified absolute paths:
  `C:\foo`, `C:/foo`
- UNC paths:
  `\\server\share\foo`, `//server/share/foo`
- rooted current-drive paths:
  `\foo`, `/foo`

Reject drive-relative paths:

- `C:foo`

Reason:

- Windows defines `C:foo` relative to the current working directory on drive
  `C:`, which tmux does not model coherently and should not try to preserve.

### 2. Home expansion

Accept both:

- `~/foo`
- `~\foo`

This policy should be shared across all tmux filesystem consumers on Win32,
including config lookup and file redirection paths.

### 3. Environment-relative expansion

Accept both:

- `$VAR/foo`
- `$VAR\foo`

Keep tmux's existing `$VAR` syntax. This plan does not require `%VAR%`
expansion.

### 4. Separator-insensitive basename and dirname behavior

Where tmux needs basename or dirname style behavior for filesystem paths,
helpers must understand both `/` and `\`.

That includes:

- shell name extraction;
- editor name heuristics;
- format basename and dirname modifiers;
- any Win32 shell or process-launch helpers that classify paths by filename.

### 5. Path-list separators

Filesystem path lists on Win32 must not use raw `:` splitting.

Use a Win32-aware parser that:

- uses `;` as the list separator on Win32; or
- at minimum parses drive letters without splitting `C:\...`.

This applies to tmux path-list consumers such as default config search paths
and any future path-list handling that carries filesystem paths rather than
tmux syntax.

### 6. Display spelling versus comparison identity

Do not treat lowercased normalized text as the only representation of a path.

The path service should distinguish between:

- display path:
  user-visible spelling preserved for messages, options, and logs where useful;
- comparison path:
  normalized spelling used for string-level equality and alias collapse;
- trusted object identity:
  filesystem-validated identity used when mutation or security decisions depend
  on more than string normalization.

This separation is especially important for IPC endpoints.

## IPC Endpoint Policy

The Win32 IPC path model should classify endpoints instead of treating all
accepted text paths as equally trusted.

### Endpoint classes

1. Managed endpoint

- source: default `-L` or default socket name resolution
- root: managed tmux-owned directory under `LOCALAPPDATA`
- tmux may create parents, apply security, create coordination state, and
  remove proven stale endpoints here

2. Explicit endpoint

- source: explicit `-S`
- path is user-selected and may live outside the managed root
- text canonicalization alone does not make it trusted for mutation

3. Inherited endpoint

- source: `$TMUX`
- path policy must be identical to explicit `-S` once the path has been parsed

There should not be a fourth hidden class where inherited or explicit paths are
accepted for connect but silently treated as managed for mutation.

### Supported behavior by endpoint class

Managed endpoint:

- full connect, startup, bind, coordination, and stale cleanup support

Explicit or inherited endpoint:

- connect is allowed after path resolution
- startup and listener creation are allowed only after explicit parent-path
  validation succeeds
- stale cleanup is allowed only after the same validation succeeds and the
  endpoint is proven dead by the shared startup state machine

If validation fails, tmux should fail with a clear error rather than silently
downgrading or mutating an untrusted directory.

### Parent-path validation requirements

For explicit or inherited endpoints used for startup or cleanup, validation
should check at minimum:

- every parent component used by tmux as a directory is actually a directory;
- no forbidden reparse-point traversal occurs in the validated parent chain;
- the endpoint parent is not a shared or ambiguous location that tmux has not
  chosen to trust;
- the final mutation scope is the intended directory, not a text alias that
  resolves elsewhere.

The exact Win32 API sequence may evolve, but the policy must be explicit before
implementation details are chosen.

## Shared Path Service Boundary

This work should not remain a collection of helper functions in random files.

Introduce one shared path service interface with backend-specific
implementation. Exact names may change, but the abstraction should look like:

```c
enum tmux_path_kind {
	TMUX_PATH_INVALID,
	TMUX_PATH_RELATIVE,
	TMUX_PATH_ROOTED,
	TMUX_PATH_ABSOLUTE,
	TMUX_PATH_UNC,
	TMUX_PATH_DRIVE_RELATIVE
};

struct tmux_path;

struct tmux_path *tmux_path_resolve(const char *, const char *, int, char **);
const char *tmux_path_display(const struct tmux_path *);
const char *tmux_path_compare(const struct tmux_path *);
enum tmux_path_kind tmux_path_kind(const struct tmux_path *);
int tmux_path_is_absolute_for_open(const struct tmux_path *);
char *tmux_path_join_display(const char *, const char *);
char *tmux_path_basename(const char *);
char *tmux_path_dirname(const char *);
void tmux_path_free(struct tmux_path *);
```

The important point is not the exact signature. The important point is that:

- generic tmux code should stop open-coding Win32 path grammar;
- Win32 IPC code should stop being the only place with a stronger
  canonicalization path;
- callers should choose from a small shared API instead of embedding new path
  policy locally.

## Implementation Strategy

### Stage 1: Introduce the shared path layer

Status: completed

Create a path service boundary used by both Unix and Win32, with the Win32
backend carrying the real policy and the Unix backend staying minimal.

Initial responsibilities:

- classify path kinds;
- resolve relative versus rooted versus absolute forms;
- expand home and `$VAR` prefixes;
- join paths safely for the current platform;
- provide separator-insensitive basename helpers on Win32.

Success condition:

- new Win32 path policy is no longer introduced by editing unrelated files
  directly.

### Stage 2: Migrate generic Win32 filesystem consumers

Status: completed

Replace local Win32 path special cases in:

- [`tmux.c`](../tmux.c)
- [`win32-error.c`](../win32-error.c)
- [`cfg.c`](../cfg.c)
- [`file.c`](../file.c)
- [`format.c`](../format.c)
- [`win32-conpty.c`](../win32-conpty.c)

Specific outcomes:

- `/foo` and `\foo` are handled consistently;
- `C:foo` is rejected consistently where filesystem paths are expected;
- `~/` and `~\` work consistently;
- `$VAR/` and `$VAR\` work consistently;
- basename and dirname behavior is consistent across the tree.

Success condition:

- there is one Win32 filesystem grammar, not multiple partially conflicting
  ones.

### Stage 3: Replace Win32 path-list splitting

Status: completed

Move `expand_paths()` and similar path-list consumers to a Win32-aware parser.

This stage must cover:

- config search path parsing;
- any default path-list constants or environment-driven path lists that
  represent filesystem paths.

Success condition:

- drive letters are not broken by Unix `:` splitting.

### Stage 4: Split IPC display, comparison, and trust identity

Status: completed

Refactor IPC endpoint resolution so it carries:

- display spelling;
- comparison spelling;
- endpoint class;
- trust-validation result.

Do not rely on unconditional lowercasing as the only stored form.

Current state:

- endpoint resolution now knows whether the path came from default `-L`,
  explicit `-S`, or inherited `$TMUX`;
- endpoint resolution now distinguishes managed versus explicit endpoint class;
- Win32 canonicalization now uses the shared path layer and rejects
  drive-relative socket paths;
- display spelling is preserved, and Win32 comparison identity is now a
  distinct alias-collapsing representation used by startup coordination;
- startup-time trust validation is still separate pending work.

Success condition:

- IPC endpoint equality and IPC endpoint trust are no longer the same thing.

### Stage 4 execution slices

The remaining Stage 4 work should be done in narrow commits with separate
verification, not as one blended "path hardening" patch.

#### Slice 4A: give endpoints a real comparison identity

Status: completed

Current gap:

- `endpoint->compare_path` still duplicates `endpoint->path`;
- preserved display spelling exists, but alias collapse does not.

Required work:

- derive a Win32 comparison identity that is distinct from display spelling and
  stable across slash aliases and ordinary case aliases;
- keep the actual user-visible endpoint path unchanged for logs, errors, and
  `#{socket_path}`;
- do not reopen the generic filesystem path layer unless a missing primitive is
  required for IPC identity.

Completed outcome:

- one endpoint object now carries both preserved display spelling and a
  distinct compare identity.

#### Slice 4B: route alias-sensitive startup logic through compare identity

Status: completed for startup coordination

Current gap:

- startup coordination now uses the comparison identity;
- later trust policy still does not, because that belongs to Stage 5.

Completed outcome:

- alias-sensitive startup coordination now uses the comparison identity rather
  than the display spelling;
- bind, connect, and `#{socket_path}` still use the accepted display spelling
  that the user selected.

Remaining boundary:

- trust validation and any later mutation policy still belong to Stage 5.

#### Slice 4C: stabilize the native startup smoke as a real gate

Status: completed for direct native PowerShell invocation

Current gap:

- the checked-in `tools/win32-ipc-startup-smoke.ps1` now passes when invoked
  directly from native PowerShell;
- invoking it through a nested `powershell.exe -File` wrapper under the current
  automation host can still reproduce `server exited unexpectedly` during
  `Test-ConcurrentAutostart`.

Completed outcome:

- direct invocation from native PowerShell is once again a reliable regression
  gate for the path-policy startup work;
- focused reproductions and exact helper-style replays pass in the same native
  shell context, which isolates the remaining false-negative to the nested
  wrapper environment rather than the checked-in smoke logic itself.

Remaining caveat:

- if this smoke is later automated through a nested PowerShell wrapper, that
  wrapper path still needs its own separate investigation.

### Stage 5: Harden explicit `-S` and inherited `$TMUX`

Status: completed

Add explicit-path validation before tmux performs startup-side mutation for
non-managed endpoints.

Mutation covered here includes:

- parent creation;
- startup coordination artifacts;
- stale unlink;
- bind-time recovery behavior.

Success condition:

- explicit paths remain supported, but tmux no longer treats arbitrary accepted
  text paths as implicitly safe to mutate.

Completed outcome:

- explicit `-S` and inherited `$TMUX` paths now validate the nearest existing
  parent chain before any startup-side mutation;
- validation rejects non-directory ancestors, reparse points in the validated
  chain, and parents whose nearest existing directory is not owned by the
  current user;
- startup-side failures now surface the trust-policy cause back through the
  client instead of collapsing to generic `Permission denied`;
- native PowerShell startup smoke now verifies safe inherited startup and
  unsafe explicit rejection without parent creation.

### Stage 6: Decouple coordination from sibling `.lock` when needed

Status: pending

If explicit endpoint policy shows that sibling `.lock` files force unnecessary
directory mutation outside trusted roots, move coordination into a backend-owned
namespace that does not require adjacent `.lock` creation.

Examples of acceptable directions:

- managed coordination directory keyed by canonical endpoint identity;
- backend-private coordination primitive not materialized beside the endpoint.

This must stay behind the shared IPC startup abstraction.

Success condition:

- startup coordination no longer weakens explicit endpoint policy merely because
  the current lock representation lives next to the socket path.

## Testing Plan

Build:

- use MSYS2 `bash`
- prepend `/ucrt64/bin:/usr/bin` to `PATH`
- run `make`

Runtime validation:

- use native PowerShell or `cmd`
- do not run tmux itself under MSYS2

### General path matrix

Verify the same result across path consumers for:

- `C:\tmp\foo`
- `C:/tmp/foo`
- `\\server\share\foo`
- `//server/share/foo`
- `\tmp\foo`
- `/tmp/foo`
- `C:foo`
- `~/foo`
- `~\foo`
- `$HOME/foo`
- `$HOME\foo`

Consumers to exercise:

- cwd resolution;
- config lookup;
- `source-file`;
- file read and write paths;
- shell and editor basename heuristics;
- format basename and dirname modifiers.

### IPC endpoint matrix

Verify:

- default `-L` managed root startup and reconnect;
- explicit `-S` startup in a validated user-owned directory;
- inherited `$TMUX` reconnect and stale-endpoint behavior;
- slash and case aliases resolve to one comparison identity;
- unsafe explicit parents are rejected before startup-side mutation;
- live endpoints are never stale-unlinked by a loser in a race.

### Immediate next slice

The next implementation step should stay narrowly scoped:

1. decide whether Stage 6 should keep sibling `.lock` files for validated
   explicit endpoints or move coordination into a backend-private namespace.

Stage 5 now proves that explicit startup mutation can be gated correctly, so
the next remaining path-policy question is whether adjacent `.lock` files are
still an acceptable product constraint for validated explicit endpoints.

### Case behavior

Verify both ordinary Win32 case-insensitive directories and any available
case-sensitive directory configurations that matter to local development.

The goal is not to promise fully case-sensitive filesystem semantics
immediately. The goal is to ensure tmux does not hardcode a lowercasing policy
that corrupts its own path model.

## Recommended Implementation Order

1. Introduce the shared path abstraction and migrate generic Win32 filesystem
   consumers first.
2. Replace path-list splitting and separator-only basename logic.
3. Refactor IPC endpoint objects to carry endpoint class and path identity
   explicitly.
4. Harden explicit `-S` and inherited `$TMUX` mutation policy.
5. Only then revisit whether sibling `.lock` files remain compatible with the
   chosen explicit endpoint policy.

This order matters. If tmux keeps the current fragmented grammar while trying to
harden IPC trust rules, the result will be another layer of special cases.

## Expected Outcome

After this plan is implemented:

- tmux has one coherent Win32 path grammar instead of several conflicting
  ones;
- rooted Win32 paths, home expansion, environment expansion, and basename
  handling behave consistently;
- drive letters no longer collide with filesystem path-list parsing;
- IPC endpoint identity is explicit instead of being a side effect of socket
  helper normalization;
- default managed socket paths remain easy to use;
- explicit `-S` and inherited `$TMUX` remain supported without implicitly
  granting tmux broad mutation rights on arbitrary directories.

That is the bar required before the remaining Win32 port issues in this area
can be fixed cleanly.
