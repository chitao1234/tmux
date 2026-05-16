#!/bin/sh

# shared IPC startup path should handle default labels, explicit -S aliases,
# and concurrent startup races.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -f/dev/null -Ltest"
TMUX_RACE="$TEST_TMUX -f/dev/null -Ltest-race-$$"
TMUX_START="$TEST_TMUX -f/dev/null -Ltest-start-$$"

TMP=$(mktemp -d)
SOCKET_REL=alias.sock
SOCKET_ABS=$TMP/$SOCKET_REL

cleanup() {
	$TMUX kill-server 2>/dev/null
	$TMUX_RACE kill-server 2>/dev/null
	$TMUX_START kill-server 2>/dev/null
	$TEST_TMUX -f/dev/null -S "$SOCKET_ABS" kill-server 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup 0 1 15

$TMUX kill-server 2>/dev/null
$TMUX start-server || exit 1
SOCKET=$($TMUX display-message -p '#{socket_path}') || exit 1
[ -n "$SOCKET" ] || exit 1
[ ! -e "$SOCKET.lock" ] || exit 1
$TMUX kill-server 2>/dev/null || exit 1
[ ! -e "$SOCKET.lock" ] || exit 1

(
	cd "$TMP" || exit 1
	$TEST_TMUX -f/dev/null -S "$SOCKET_REL" new -ds aliascheck || exit 1
	SOCKET=$($TEST_TMUX -f/dev/null -S "$SOCKET_ABS" display-message -p '#{socket_path}') || exit 1
	[ "$SOCKET" = "$SOCKET_ABS" ] || exit 1
	$TEST_TMUX -f/dev/null -S "$SOCKET_ABS" list-sessions |
	    grep '^aliascheck:' >/dev/null || exit 1
) || exit 1
$TEST_TMUX -f/dev/null -S "$SOCKET_ABS" kill-server 2>/dev/null || exit 1

i=0
pids=
while [ $i -lt 8 ]; do
	$TMUX_RACE list-commands >/dev/null 2>&1 &
	pids="$pids $!"
	i=$((i + 1))
done
for pid in $pids; do
	wait $pid || exit 1
done
$TMUX_RACE kill-server 2>/dev/null || exit 1

i=0
pids=
while [ $i -lt 8 ]; do
	$TMUX_START start-server >/dev/null 2>&1 &
	pids="$pids $!"
	i=$((i + 1))
done
for pid in $pids; do
	wait $pid || exit 1
done
$TMUX_START kill-server 2>/dev/null || exit 1

exit 0
