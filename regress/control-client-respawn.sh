#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

cleanup() { $TMUX kill-server 2>/dev/null; }
trap cleanup 0 1 15

$TMUX -f/dev/null new -d -s c -x80 -y24 \
	'sh -c "while :; do echo tick; sleep 0.2; done"' || exit 1
sleep 1

# Respawn the pane while a control client is attached. The pane output after
# the respawn must not underflow the control client's offsets.
( sleep 1; echo 'respawn-pane -k -t c:0.0'; sleep 2; echo 'detach-client' ) |
	$TMUX -C attach -t c >/dev/null 2>&1

# If the control client killed the server, this fails.
$TMUX ls >/dev/null 2>&1 || exit 1

$TMUX kill-server 2>/dev/null
exit 0
