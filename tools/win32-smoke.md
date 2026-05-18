# Native Windows tmux MVP Smoke Test

Run from Windows Terminal with the MinGW-built `tmux.exe` in PATH.

1. `tmux.exe -L winmvp new-session`
   Expected: tmux starts, status line appears, shell prompt is visible.

2. Press `Ctrl-b %`
   Expected: vertical split appears, second shell prompt is visible.

3. Press `Ctrl-b "`
   Expected: horizontal split appears, third shell prompt is visible.

4. Run `echo hello` in each pane.
   Expected: each pane accepts input and renders output.

5. Resize the Windows Terminal window.
   Expected: tmux redraws without corrupting pane borders and ConPTY panes receive the new size.

6. Press `Ctrl-b d`
   Expected: client detaches and server remains running.

7. Run `tmux.exe -L winmvp attach`
   Expected: previous session reattaches with panes intact.

8. Exit all shells.
   Expected: panes close or mark exited, session/server exits according to tmux options.

9. Run `tmux.exe -L winmvp ls`
   Expected: command reports no server or no sessions after cleanup.

For a failing step, rerun with `tmux.exe -vv -L winmvp ...` and record:

```text
command:
expected:
actual:
log file:
```

For native-console relay automation from a redirected runner, use
`tools/win32-console-relay-coverage.ps1`. It launches the real-console relay
smoke in fresh console windows and records per-case result files under a temp
artifact root. Its default suite covers the stable relay cases; use
`-IncludeBacklogCases` to add the more host-sensitive output-progress and
resize-backlog probes.
