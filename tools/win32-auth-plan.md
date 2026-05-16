# Win32 AF_UNIX Auth Plan

## Objective

Define the Win32 authentication model for tmux while keeping AF_UNIX as the
control transport and staying close to the Unix tmux model.

This plan is about client admission on Win32. It is not a transport redesign,
not a terminal I/O redesign, and not a general-purpose Windows security system.

## Design Goal

The Win32 auth model should match Unix tmux in spirit:

- socket/path security is the first barrier;
- peer identity is the second barrier;
- tmux should not grow durable secrets, persistent auth databases, or mutual
  server-authentication policy just to compensate for Windows API differences.

Unix tmux relies on filesystem/socket ownership plus peer credentials. Win32
should do the same, with one small platform-specific addition: Windows AF_UNIX
does not give tmux a usable `getpeereid()` equivalent, so tmux needs a minimal
peer-credential replacement.

## Product Model

The Win32 product behavior should be:

- the authorization principal is the Windows user SID, not a specific logon
  session;
- later attaches from a different logon session of the same Windows user are
  allowed, including later SSH logons;
- different Windows users are not allowed;
- lower-integrity clients must not attach to a higher-integrity server;
- tmux should not try to solve "did the user intentionally connect to this
  socket path?" beyond normal socket-path trust.

This keeps the policy small and matches the way tmux already behaves on Unix.

## Current Problem

Today the Win32 port has only the first barrier and not the second:

- socket reachability and directory security are doing most of the work;
- [`server-acl.c`](../server-acl.c) accepts every Win32 client in
  `server_acl_join()`;
- [`server-client.c`](../server-client.c) accepts unauthenticated
  `MSG_IDENTIFY_*` traffic;
- [`client.c`](../client.c) sends `MSG_IDENTIFY_CLIENTPID` and direct-handle
  claims before the server has a trustworthy peer identity;
- [`win32-ipc.c`](../win32-ipc.c) can inspect a claimed client process token,
  but only after trusting the client's claimed PID.

So Win32 currently trusts a connection too early and trusts identify messages
that should only be accepted after peer identity is established.

## Requirements

The Win32 auth model must:

1. Keep AF_UNIX as the control transport.
2. Stay conceptually close to Unix tmux auth.
3. Add only the smallest Win32-specific mechanism needed to replace missing peer
   credentials.
4. Allow same-user attach across different logon sessions.
5. Reject different-user attach.
6. Reject lower-integrity attach to a higher-integrity server.
7. Reject normal identify and handle-claim traffic until peer identity is
   established.
8. Reuse the authenticated peer/process identity for later direct-handle
   duplication.

## Non-Goals

This plan does not do any of the following:

- no persistent server secret;
- no auth metadata database;
- no mutual client/server authentication;
- no attempt to protect users from intentionally or accidentally connecting to a
  rogue socket path beyond normal tmux path trust;
- no redesign of relay versus direct terminal-handle transport;
- no full `server-access` redesign on Windows.

Those would move too far away from what tmux normally owns.

## Threat Model

This plan is meant to stop:

- a different Windows user connecting to the tmux socket and being accepted as a
  client;
- an unauthenticated client sending `MSG_IDENTIFY_*` traffic and being trusted;
- an unauthenticated client claiming an arbitrary PID and asking the server to
  duplicate handles from it;
- a lower-integrity client attaching to a higher-integrity same-user server.

This plan does not try to isolate malicious processes running as the same
Windows user. Same-user remains the tmux trust boundary, just as Unix tmux uses
same-uid style trust.

## Core Design

Add one Win32-specific peer-bind step immediately after AF_UNIX connect and
before normal identify traffic.

That step gives tmux the Win32 equivalent of peer credentials:

- it proves that the client really owns the claimed PID;
- it lets the server read the token from that exact process;
- it lets the server apply the same-user plus integrity-floor policy;
- it gives the server a trusted process binding that later direct-handle
  duplication can reuse.

This is the only missing primitive. Everything else should stay tmux-shaped.

## Peer-Bind Handshake

### Overview

1. Client connects to the AF_UNIX socket.
2. Server puts the connection in `auth-pending` state.
3. Server sends a fresh random nonce to the client.
4. Client creates a small unnamed kernel object in its own process and writes
   the nonce into it.
5. Client sends:
   - its PID;
   - the handle value for that proof object.
6. Server opens the claimed process.
7. Server duplicates the proof handle from that process.
8. Server reads back the nonce from the duplicated object.
9. If the nonce matches, the server treats that process as the authenticated
   peer process for the connection.
10. Server opens the token from that same process and applies admission policy.
11. Only then does normal tmux identify traffic continue.

This is not "extra auth policy." It is only a peer-credential substitute for
Win32 AF_UNIX.

### Why A Nonce

The nonce prevents a client from only naming some PID and relying on the server
to trust it. The server must prove to itself that the connecting client can send
both:

- a PID;
- a handle that really lives inside that process and contains the server's
  challenge value.

That is the minimum shape of a trustworthy process bind.

### Proof Object

The proof object should be simple and local:

- unnamed file mapping is a reasonable default;
- another small readable kernel object would also work.

The object only needs to carry the server nonce for this one connection. It does
not need to be durable or reusable.

## Admission Policy

Once the server has an authenticated peer process, it should open that process
token and capture:

- user SID;
- integrity level.

The admission rule should be:

- client user SID must equal server owner SID;
- client integrity must not be lower than the server integrity level.

What is intentionally not part of admission:

- logon SID equality;
- session id equality.

Those must not be required, because later attach from another same-user logon
session is a core tmux use case.

## Relation To Unix tmux

This design is deliberately small:

- Unix tmux asks the kernel who the peer is.
- Win32 tmux asks the peer to prove which process it is, then asks the kernel
  for that process token.

That is the whole model. No durable secret, no separate auth store, no
general-purpose trust system.

## Pre-Auth Protocol Rules

Before peer bind succeeds, the server must accept only:

- version/protocol negotiation traffic;
- Win32 auth messages for the peer-bind step.

Before peer bind succeeds, the server must reject:

- all `MSG_IDENTIFY_*` traffic;
- `MSG_COMMAND`;
- relay traffic;
- file traffic;
- direct-handle claims.

This is the structural fix for the current Win32 bug. Right now tmux trusts the
client too early.

## Direct Handle Transfer

The current Win32 direct-handle path should be gated on the authenticated peer
process, not on a raw claimed PID from identify traffic.

That means:

- the peer-bind step establishes the trusted client process for the connection;
- later direct stdin/stdout handle claims must come from that same process;
- [`win32-ipc.c`](../win32-ipc.c) should stop treating a claimed PID as
  sufficient authority on its own.

This keeps direct-handle transfer small and consistent with the peer-credential
model.

Relay clients still go through the same admission step. Authentication is about
who may become a tmux client, not just who may transfer direct terminal handles.

## Custom Socket Paths

This auth model does not replace socket-path hardening.

It helps with one thing:

- reaching a socket is no longer enough to become a trusted tmux client.

It does not help with:

- a weak custom `-S` path being a denial-of-service target;
- reparse-point or aliasing issues;
- a user intentionally connecting to the wrong socket path.

That is acceptable because tmux on Unix also does not grow a separate
server-authentication system to solve those cases. The correct place for those
issues is the IPC/path hardening track.

## Server ACL Integration

Win32 should stop doing unconditional success in
[`server-acl.c`](../server-acl.c).

The Win32 admission flow should become:

1. `accept()` creates the client object.
2. The client starts in auth-pending state.
3. Win32 peer-bind succeeds or fails.
4. Same-user plus integrity-floor policy is applied.
5. Only then is the client treated as admitted and allowed to continue with
   normal identify traffic.

This pass does not need to redesign the user-visible `server-access` command.
For now, Windows policy is same-user SID plus integrity floor. That still
allows later attaches from other logon sessions of the same user, but blocks
different users and lower-integrity clients. The difference from today is that
this policy is enforced from an authenticated peer identity instead of
unconditional trust.

## Code Changes Required

### 1. Protocol

Add a small Win32 auth message set to [`tmux-protocol.h`](../tmux-protocol.h),
for example:

- `MSG_WIN32_AUTH_CHALLENGE`
- `MSG_WIN32_AUTH_BIND`
- `MSG_WIN32_AUTH_RESULT`

Exact naming can change, but the protocol should stay minimal.

### 2. Client State

[`client.c`](../client.c) should:

- wait for the server challenge after connect;
- create the proof object;
- send the bind message before normal identify traffic;
- only send normal `MSG_IDENTIFY_*` messages after bind success.

### 3. Server State

[`server-client.c`](../server-client.c) should:

- keep Win32 clients in auth-pending state initially;
- accept only auth messages pre-bind;
- verify the proof object against the server nonce;
- capture token identity and integrity from the authenticated process;
- reject everything else before auth completion.

### 4. ACL / Admission

[`server-acl.c`](../server-acl.c) and [`server.c`](../server.c) should stop
treating Win32 `accept()` as admission-complete. Win32 admission must happen
after the peer-bind step.

### 5. Handle Duplication

[`win32-ipc.c`](../win32-ipc.c) should be reshaped from:

- "duplicate from whatever PID the client claimed"

toward:

- "duplicate only from the process already authenticated for this peer."

That is a small but important tightening.

## Test Plan

Native Windows tests should cover at least:

1. Same user, same logon session:
   - connect and attach successfully;
   - verify identify and terminal startup still work.

2. Same user, different logon session:
   - create a session from one logon;
   - attach from a later same-user logon;
   - verify admission succeeds.

3. Different user:
   - connect to the socket;
   - verify bind/admission fails before identify completes.

4. Lower-integrity same-user client to higher-integrity server:
   - verify admission fails.

5. Higher-integrity same-user client to lower-integrity server:
   - verify it follows the chosen integrity-floor policy.

6. Pre-auth identify abuse:
   - send `MSG_IDENTIFY_*` before bind success;
   - verify disconnect and log evidence.

7. Direct-handle path:
   - verify direct stdin/stdout duplication succeeds only when the claiming
     process is the authenticated peer process.

8. Relay path:
   - verify relay clients still attach after the new auth step.

## Recommended Implementation Order

1. Add protocol messages and auth-pending connection state.
2. Implement nonce challenge and process bind.
3. Capture token SID and integrity from the authenticated process.
4. Move Win32 admission to post-bind state.
5. Gate direct-handle duplication on authenticated peer process identity.
6. Add native Windows regression coverage.

## Summary

The right Win32 auth model is:

- keep AF_UNIX;
- keep socket/path security as the first barrier;
- add one small process-bind step to replace missing peer credentials;
- authorize by same-user SID plus integrity floor;
- then continue with normal tmux identify and terminal setup.

That stays close to Unix tmux instead of turning tmux into a separate Windows
authentication system.
