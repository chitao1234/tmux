#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

TMP=$(mktemp -d)
cleanup() {
	$TMUX kill-server 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup 0 1 15
cd "$TMP" || exit 1

printf hello >data
$TMUX -f/dev/null new -d -x80 -y24 || exit 1
$TMUX load-buffer data || exit 1

# A leading $ in a buffer path is a literal filename on Unix.
$TMUX save-buffer '$FOO' || exit 1
[ -f '$FOO' ] || exit 1

$TMUX kill-server 2>/dev/null
exit 0
