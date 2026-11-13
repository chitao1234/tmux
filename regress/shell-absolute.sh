#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

cleanup() { $TMUX kill-server 2>/dev/null; }
trap cleanup 0 1 15

# A bare shell name must never be resolved through the current directory.
$TMUX -f/dev/null new -d -x80 -y24 || exit 1
sleep 1

shell=$($TMUX display -p '#{default-shell}') || exit 1
case "$shell" in
/*)	;;
*)	exit 1 ;;
esac

$TMUX kill-server 2>/dev/null
exit 0
