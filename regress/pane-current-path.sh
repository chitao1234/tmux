#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

cleanup() { $TMUX kill-server 2>/dev/null; }
trap cleanup 0 1 15

$TMUX -f/dev/null new -d -x80 -y24 || exit 1
sleep 1

cwd=$($TMUX display -p '#{pane_current_path}') || exit 1
[ -n "$cwd" ] || exit 1

# The server must survive the format expansion (it used to free a static
# buffer returned by osdep_get_cwd()).
$TMUX ls >/dev/null 2>&1 || exit 1

$TMUX kill-server 2>/dev/null
exit 0
