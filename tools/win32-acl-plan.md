# Win32 ACL Integration Plan

## Objective

Integrate the authenticated Win32 peer identity into tmux ACL semantics while
keeping the tmux authorization model small and Unix-shaped.

This plan follows the Win32 auth work in `win32-auth-plan.md`. It does not
redesign transport, socket-path policy, or the broader Windows auth model.

## Why This Is Needed

The current Win32 auth path already establishes a real authenticated peer:

- the server knows which client process was authenticated;
- the peer token yields a Windows user SID;
- the peer token yields an integrity level.

But the generic ACL layer still only understands Unix `uid_t`:

- `struct tmuxpeer` stores `uid` only;
- `proc_get_peer_uid()` is not a real Windows identity model;
- `server_acl_join()` returns success unconditionally on Win32;
- `server-access` has no useful Win32 admission target yet.

That split is the main remaining gap.

## Design Goal

The ACL layer should operate on an abstract peer principal, not on a fake
Windows `uid_t`.

The principal model should stay narrow:

- Unix peers continue to use `uid_t`;
- Win32 peers use the authenticated Windows user SID;
- same-user later attaches across different Windows logon sessions remain
  allowed;
- logon session is not the ACL principal;
- integrity still remains part of admission, but not the ACL key.

## Non-Goals

This plan does not:

- redesign the Win32 auth handshake;
- introduce persistent auth databases or shared secrets;
- redesign relay versus direct-handle transport;
- invent a new Windows-specific access-control system unrelated to tmux ACLs;
- require logon-session equality for attach.

## Current Gap

Today the Win32 auth result is trapped in `client->win32_peer`, while the ACL
code still asks the generic peer layer for a `uid_t`.

That means:

- authenticated Win32 peers are not first-class ACL principals;
- `server_acl_join()` cannot enforce a meaningful Win32 readonly/deny policy;
- `server-access` on Windows is still effectively disconnected from the real
  authenticated identity;
- helper display paths still render via Unix passwd lookups that do not model
  Windows identity correctly.

## Proposed Model

Introduce a small generic peer principal abstraction and make ACL consume it.

The abstraction should capture only the stable authorization principal:

- Unix: numeric `uid_t`;
- Win32: authenticated user SID string.

The identity object should be available on `struct tmuxpeer`, and Win32 should
populate it from the authenticated peer process after auth finishes.

The client-side Win32 auth state should remain in `client->win32_peer` because
the process handle and integrity checks are still needed for direct handle
duplication and admission.

## ACL Semantics

The ACL tree should become principal-keyed instead of Win32-special-cased.

Expected behavior:

- `server_acl_join()` compares the authenticated principal against the ACL
  entries;
- `server_acl_user_allow()` / `deny()` / `allow_write()` / `deny_write()`
  operate on the principal key;
- readonly state continues to propagate to live clients that share that
  principal;
- `server-access -l` displays the current principal set on Win32 rather than
  pretending that Unix user names are the only valid representation.

The important rule is that the ACL core should not depend on `getpwuid()` or
`getuid()` on Win32 as the source of truth.

## `server-access` Policy

`server-access` should stay tmux-shaped, but it needs a Win32-aware backend.

Recommended direction:

- keep Unix user-name behavior unchanged;
- make Windows list output show the current authenticated principal;
- let Windows ACL mutation target the authenticated principal, not an invented
  Unix user mapping;
- keep the UI conservative until there is a stable, user-friendly way to name
  principals on Windows.

This avoids pretending that Windows has a meaningful Unix passwd database for
ACL control.

## Implementation Order

1. Add a generic principal/identity structure and helper comparison routines.
2. Populate the generic peer identity from Unix peer credentials and from the
   Win32 authenticated SID.
3. Convert `server-acl.c` to key off the abstract principal instead of a raw
   `uid_t`.
4. Remove the Win32 unconditional-success ACL path and seed the Win32 ACL tree
   from the authenticated server principal.
5. Update `server-access` and identity display paths to use the new principal
   model.
6. Add Windows-native smoke coverage for same-user attach, different-user
   rejection, and readonly propagation.

## Testing Plan

Native Windows coverage should include:

- same Windows user across different logon sessions attaches successfully;
- different Windows user is rejected before identify traffic is accepted;
- authenticated same-user clients inherit the expected readonly state from ACL;
- `server-access -l` shows the active Windows principal;
- changing readonly state updates all clients that share that principal.

The existing auth smoke should continue to pass unchanged after the ACL
integration.

## Open Questions

1. Should the generic peer principal live directly on `struct tmuxpeer`, or as
   a separate pointer owned by the peer?
2. Should `server-access` on Windows accept only the current authenticated
   principal initially, or also accept a broader principal naming scheme?
3. Should format/display helpers expose SID text directly on Windows, or a
   normalized principal string?

## Summary

The right ACL design for Win32 is:

- keep the Win32 auth model;
- stop pretending `uid_t` is the Windows authorization source of truth;
- make ACL consume an abstract authenticated principal;
- keep same-user multi-logon attaches working;
- keep the Windows `server-access` surface conservative until the principal
  model is stable.
