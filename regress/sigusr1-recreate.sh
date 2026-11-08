#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

cleanup() {
	$TMUX kill-server 2>/dev/null
	kill "${server:-}" 2>/dev/null
	wait "${server:-}" 2>/dev/null
}
trap cleanup 0 1 15

# Run the server in the foreground so the test owns its pid.
$TEST_TMUX -Ltest -f/dev/null -D &
server=$!
sleep 1

$TMUX new -d -x80 -y24 -s s || exit 1
sleep 1

socket=$($TMUX display -p '#{socket_path}') || exit 1
[ -S "$socket" ] || exit 1

# Remove the socket and ask the server to recreate it.
rm -f "$socket"
kill -USR1 "$server"
sleep 1

[ -S "$socket" ] || exit 1
$TMUX ls >/dev/null 2>&1 || exit 1

$TMUX kill-server 2>/dev/null
wait "$server" 2>/dev/null
exit 0
