#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

TMP=$(mktemp)
cleanup() {
	$TMUX kill-server 2>/dev/null
	rm -f "$TMP"
}
trap cleanup 0 1 15

cat >"$TMP" <<EOF
run-shell "sleep 1"
set -g status-left CONFIGURED
EOF

# The startup command must run after the configuration has finished, even
# when the configuration contains a command that waits (run-shell).
out=$($TMUX -f "$TMP" new -d -P -F '#{status-left}' -s s) || exit 1
[ "$out" = "CONFIGURED" ] || exit 1

$TMUX kill-server 2>/dev/null
exit 0
