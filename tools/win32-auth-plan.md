# Win32 AF_UNIX Auth Plan

## Objective

Define the Win32 authentication model for tmux while keeping AF_UNIX as the
control transport.

This plan is about authenticated client admission. It is not a transport
redesign, not a relay/direct terminal I/O redesign, and not the full custom
socket-path hardening pass.

## Product Model

The Win32 product model for tmux should be:

- the authorization principal is the Windows user SID, not the logon session;
- later attaches from other logon sessions of the same Windows user must work,
  including later SSH logons;
- different Windows users must not be able to act as tmux clients;
- lower-integrity clients must not be able to attach to a higher-integrity
  server;
- the client must authenticate the server before it sends identify data,
  environment data, terminal handle claims, or commands.

This means the auth model must allow same-user multi-logon attach while still
rejecting cross-user and lower-integrity attach.

## Current State

Today the Win32 port has partial socket and handle security, but not real client
authentication:

- [`server-acl.c`](../server-acl.c) accepts every Win32 client in
  `server_acl_join()`.
- [`server.c`](../server.c) creates the `struct client` and calls
  `server_acl_join()` immediately after `accept()`.
- [`server-client.c`](../server-client.c) then accepts unauthenticated
  `MSG_IDENTIFY_*` traffic.
- [`client.c`](../client.c) sends `MSG_IDENTIFY_CLIENTPID` and Win32 direct
  handle claims before there is any authenticated peer identity.
- [`win32-ipc.c`](../win32-ipc.c) can validate a claimed client process token
  and duplicate handles from that process, but only after the server trusts the
  claimed PID.

This is backwards for Win32. The server currently trusts the connection first
and only later inspects claimed client process information.

## Requirements

The auth design must satisfy all of the following:

1. Keep AF_UNIX as the transport.
2. Work for same-user later attaches from different logon sessions.
3. Stop trusting socket reachability as the only auth boundary.
4. Stop trusting pre-auth `MSG_IDENTIFY_*` traffic.
5. Let the client verify that it is talking to the real tmux server before it
   discloses terminal handles or environment data.
6. Bind Win32 handle duplication to the authenticated client process, not just
   to an untrusted PID field.
7. Continue to work for both first-class console relay clients and direct-handle
   non-console clients.
8. Fail closed on missing or invalid auth state.

## Threat Model

The design should defend against:

- a different Windows user connecting to the AF_UNIX socket;
- a different Windows user placing a fake socket at a weak custom `-S` path;
- a client connecting to a rogue server and disclosing identify or handle data;
- an unauthenticated client claiming an arbitrary PID and asking the server to
  duplicate handles from it;
- a lower-integrity client attaching to a higher-integrity server.

The design does not try to isolate malicious processes running as the same
Windows user. Same-user is the intended authorization boundary for tmux on
Win32.

## Core Design

The auth model has two layers:

1. durable same-user mutual authentication;
2. authenticated process binding for Win32 handle claims and integrity checks.

Both layers are required.

Same-user mutual authentication is what allows later attach from another logon
session. Process binding is what keeps Win32 handle duplication tied to the
actual connecting client process instead of an untrusted PID string.

## Durable Server Auth State

Each server instance needs a durable auth record that any later client of the
same Windows user can read.

The auth record should live in a managed tmux metadata root under the user
profile, not beside the socket path:

- keep socket endpoints where tmux already places them;
- keep auth metadata in a managed tmux auth directory under the user profile;
- do not store the auth secret inside arbitrary custom `-S` directories.

The auth record should be keyed by the canonical socket path, not by the raw
spelling the client used. This auth plan therefore depends on the IPC/path pass
using one shared canonical socket-path function for:

- startup locking;
- server socket creation;
- client connect;
- auth record lookup.

The auth record should contain:

- a format version;
- the canonical socket path;
- the server owner user SID string;
- the server integrity level;
- a random 32-byte server secret;
- a server instance identifier or generation nonce;
- creation time for diagnostics.

The record should be written atomically and overwritten on every fresh server
startup. Clean shutdown may remove it, but correct startup must not depend on
successful cleanup of old files.

## Mutual Auth Handshake

The Win32 client and server should perform an auth handshake before any normal
identify traffic.

### Transport Rules

Before auth succeeds:

- the server must accept only version traffic and Win32 auth messages;
- the client must accept only version traffic and Win32 auth messages;
- any `MSG_IDENTIFY_*`, `MSG_COMMAND`, relay traffic, file traffic, or handle
  claims before auth completes is a protocol error and closes the connection.

This is the key change that stops the current Win32 flow from trusting
unauthenticated identify data.

### Proposed Message Flow

1. Client connects over AF_UNIX.
2. Server sends `MSG_WIN32_AUTH_CHALLENGE` with:
   - auth protocol version;
   - random server nonce;
   - server instance identifier;
   - canonical socket-path digest or identifier used by the auth record.
3. Client loads the auth record for the canonical socket path.
4. Client verifies that the record socket path matches the path it intended to
   reach.
5. Client sends `MSG_WIN32_AUTH_RESPONSE` with:
   - random client nonce;
   - proof of knowledge of the server secret.
6. Server verifies the client proof using its in-memory auth record.
7. Server sends `MSG_WIN32_AUTH_SERVER_PROOF`.
8. Client verifies the server proof.
9. Only after server proof succeeds does the client continue to process any
   normal tmux handshake.

The proof should be HMAC-SHA256 over a fixed transcript that includes:

- a domain-separation label such as `tmux-win32-auth-v1`;
- the canonical socket-path identity;
- the server instance identifier;
- the server nonce;
- the client nonce.

The client and server should use different labels for client proof and server
proof so that one message cannot be replayed as the other.

This gives mutual authentication. A rogue server without the secret cannot
convince the client to continue into identify or handle-transfer state.

## Authenticated PID Binding

Same-user auth alone is not enough for Win32 direct-handle claims. The server
still needs to know which client process it is allowed to duplicate handles
from.

After mutual auth succeeds, the client must bind a live process identity to the
connection.

### Proposed PID-Bind Flow

1. Client creates a small proof object in its own process.
2. Client sends `MSG_WIN32_AUTH_BIND_PID` with:
   - its PID;
   - a handle value for the proof object.
3. Server opens the claimed process.
4. Server duplicates the proof handle from that process.
5. Server verifies the proof object contents against the authenticated
   connection transcript.
6. Server opens the process token and captures:
   - user SID;
   - integrity level;
   - optional logon SID and session id for diagnostics.
7. Server checks the token against admission policy.
8. Server replies with `MSG_WIN32_AUTH_OK` or `MSG_WIN32_AUTH_ERROR`.

The proof object should be an unnamed file mapping or another kernel object
whose contents the server can read after `DuplicateHandle()`. Its contents
should be derived from the authenticated transcript so it is per-connection and
non-replayable.

The purpose of PID binding is:

- to tie later direct-handle duplication to the authenticated client process;
- to capture the real client token and integrity level;
- to stop unauthenticated or mismatched PID claims from reaching the existing
  handle-duplication path.

This does not try to stop malicious same-user processes from impersonating each
other. Same-user remains the product boundary. It does stop the current
cross-user and pre-auth trust problem.

## Admission Policy

Once PID binding succeeds, the server should make the real Win32 admission
decision.

The policy should be:

- authenticated client token user SID must equal the server owner SID;
- authenticated client token integrity must not be lower than the server
  integrity floor;
- logon SID mismatch is allowed;
- session id mismatch is allowed.

This is the rule that preserves the desired tmux model:

- same Windows user across multiple logon sessions may attach;
- different user may not attach;
- lower-integrity client may not attach to higher-integrity server.

Higher-integrity attach to a lower-integrity server may be allowed if the client
can reach the socket path and authenticate. That is consistent with same-user as
the principal plus integrity as a floor, not an exact-match requirement.

## Server ACL Integration

The Win32 auth pass should not keep the current unconditional
`server_acl_join()` behavior.

Instead:

- `accept()` should create the client in an auth-pending state;
- the server should not call final Win32 admission at raw `accept()` time;
- after PID binding succeeds, the server should apply Win32 admission policy and
  only then allow the normal identify flow to continue.

`server-access` does not need to be fully redesigned in this pass.

For the first complete Win32 auth model:

- keep the Windows policy same-user only;
- keep `server-access` limited on Win32 rather than forcing the Unix UID ACL
  model onto SID-based identities;
- once authenticated SID metadata exists, a later policy pass may decide whether
  Windows should support a readonly same-user mode or richer per-principal
  policy.

## Direct Handle Transfer After Auth

The current Win32 direct-handle path in
[`server-client.c`](../server-client.c) should only run after PID binding
completes.

That implies the handle duplication helper in [`win32-ipc.c`](../win32-ipc.c)
should be refactored away from the current:

- `pid + handle value + same-user token check`

toward:

- `authenticated peer/process + handle value + access check`.

The server should store the authenticated process handle or authenticated PID in
peer state and refuse to duplicate from any other process for that connection.

Relay clients use the same auth handshake even though they do not need direct
terminal handle duplication. Auth is a client-admission property, not a
direct-handle-only feature.

## Custom Socket Paths

This auth model intentionally reduces the damage from weak custom `-S` paths,
but it does not replace socket-path hardening.

What auth fixes:

- a rogue socket path endpoint without the auth secret cannot impersonate the
  tmux server to the client;
- a different user that can connect to a weak path still cannot authenticate as
  a client.

What auth does not fix:

- denial of service on weak custom paths;
- stale-path aliasing;
- reparse-point/junction trust decisions;
- unlinking or replacing a live endpoint.

Those remain part of the IPC/path hardening track.

## Failure Behavior

Fail closed on:

- missing auth record;
- malformed auth record;
- socket-path mismatch between the connection target and the auth record;
- client auth proof mismatch;
- server proof mismatch;
- PID bind proof mismatch;
- token SID mismatch;
- client integrity below server floor.

The client should surface explicit user-facing failures instead of a generic
`access not allowed` whenever possible. Auth failures should be diagnosable from
tmux logs without needing a debugger.

## Implementation Slices

### Slice 1: Shared Auth Metadata

- Add a managed Win32 auth-metadata directory.
- Add one canonical socket-path function shared by startup lock, connect, bind,
  and auth lookup.
- Add auth-record read/write helpers.
- Generate a fresh random server secret on startup and publish the record
  atomically.

### Slice 2: Protocol Messages And State Machine

- Add Win32 auth protocol messages to [`tmux-protocol.h`](../tmux-protocol.h).
- Add client and server auth-pending state.
- Reject all normal identify and command traffic before auth completion.
- Bump protocol version; mixed old/new Win32 auth behavior should not be
  supported silently.

### Slice 3: Mutual Same-User Auth

- Add server challenge generation.
- Add client HMAC proof generation.
- Add server proof generation.
- Teach the client to stop and report an auth failure before sending identify
  traffic.

### Slice 4: PID Binding And Token Capture

- Add the proof object helper on the client.
- Add server-side proof-handle duplication and verification.
- Capture authenticated token metadata from the bound process.
- Store SID, integrity, PID, and optional logon/session diagnostics on the
  peer/client.

### Slice 5: Admission And Handle Gating

- Move Win32 admission decision to post-auth state.
- Replace unconditional Win32 `server_acl_join()` success with authenticated
  same-user admission.
- Gate `win32_ipc_duplicate_client_handle()` on authenticated peer process
  identity.

### Slice 6: Cleanup And Recovery

- Rewrite the auth record when the server detects it is missing or stale while
  still running.
- Remove or invalidate the record on clean shutdown where practical.
- Ensure stale record replacement on next startup is always safe.

## Test Plan

Native Windows tests should cover at least:

1. Same user, same logon session:
   - create server;
   - attach successfully;
   - verify normal identify flow still works.

2. Same user, later logon session:
   - create session in one logon;
   - attach in a later same-user logon;
   - verify auth succeeds without sharing a logon SID.

3. Different user:
   - connect to the same socket path;
   - verify the client cannot complete auth;
   - verify the server never accepts identify data.

4. Rogue server at weak custom `-S` path:
   - client reaches the endpoint;
   - client rejects the server before identify or handle transfer.

5. Lower-integrity client to higher-integrity server:
   - same user SID;
   - auth secret may be readable;
   - server rejects at PID/token validation.

6. High-integrity client to lower-integrity server:
   - verify the chosen floor policy is enforced as designed.

7. Direct-handle path:
   - authenticated direct stdout/stdin duplication succeeds only for the bound
     client process.

8. Relay path:
   - authenticated relay client still completes attach and interactive use.

9. Pre-auth protocol abuse:
   - send `MSG_IDENTIFY_*` or `MSG_COMMAND` before auth;
   - verify disconnect and log evidence.

10. Stale metadata:
    - remove or corrupt the auth record while the server is stopped and while it
      is running;
    - verify startup rewrite and failure behavior are deterministic.

## Explicit Non-Goals

- Do not redesign tmux away from AF_UNIX here.
- Do not redesign relay versus direct-handle terminal transport here.
- Do not solve all custom socket-path security issues here.
- Do not force the Unix `server-access` UX onto Windows in this pass.

## Recommended Order Relative To Other Win32 Work

Do this before broader custom path policy and before authenticated direct-handle
frontend work. The relay and direct-handle paths both need a trustworthy Win32
client identity model underneath them.
