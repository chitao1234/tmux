# Windows highlight artifact trace

This collects enough data to tell whether a highlight trail is in tmux's
pane model, tmux's outer-terminal byte stream, or only the final Windows
console rendering.

If the same application also leaves trails when run inside an upstream tmux
server on Linux and displayed through the same terminal, the issue is outside
the tmux4win console transport. In that case, prefer collecting pane captures
and upstream traces before adding Win32-specific redraw delays or repaint
workarounds.

Run these commands from the repository root in PowerShell. They use an
isolated socket name and do not touch normal tmux sessions.

## Build the deterministic repro

```powershell
C:\msys64\usr\bin\bash.exe -lc 'source /c/ddev/tmux4win/sysroot-env.sh; export PATH="$SYSROOT/bin:/ucrt64/bin:/usr/bin:$PATH"; cd /c/ddev/tmux4win/tmux; x86_64-w64-mingw32-gcc -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 -Wall -Wextra -O2 -o tools/win32-highlight-repro.exe tools/win32-highlight-repro.c'
```

## Manual repro trace

Use this when checking what you can see in Windows Terminal or conhost:

```powershell
$env:PATH = 'C:\ddev\tmux4win\sysroot\bin;C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:TERM = 'xterm-256color'
Remove-Item .\tmux-*.log,.\tmux-out-*.log -ErrorAction SilentlyContinue
.\tmux.exe -vv -L trace-highlight new-session -x 120 -y 36 -- "$PWD\tools\win32-highlight-repro.exe"
```

Inside tmux, press Down repeatedly until the artifact appears, then press
`q` to exit. Save:

```powershell
Get-ChildItem .\tmux-*.log,.\tmux-out-*.log | Sort-Object LastWriteTime |
    Select-Object Name,Length,LastWriteTime
```

If the artifact appears, keep the `tmux-server-*.log`, `tmux-client-*.log`,
and `tmux-out-*.log` from that run.

To mimic applications such as `ntop.exe` that briefly draw the new
highlighted row before clearing the old one, use the phased mode:

```powershell
.\tmux.exe -vv -L trace-highlight-phase new-session -x 120 -y 36 -- "$PWD\tools\win32-highlight-repro.exe --phase --phase-delay 16"
```

## Automatic model trace

This checks tmux's pane model without relying on what the outer terminal
draws:

```powershell
$env:PATH = 'C:\ddev\tmux4win\sysroot\bin;C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:TERM = 'xterm-256color'
$server = 'trace-highlight-model'
.\tmux.exe -L $server kill-server 2>$null
.\tmux.exe -L $server new-session -d -x 120 -y 36 -- "$PWD\tools\win32-highlight-repro.exe --auto 12 --delay 80 --hold"
Start-Sleep -Seconds 2
.\tmux.exe -L $server capture-pane -e -p -S 0 -E 35 > $env:TEMP\trace-highlight-capture.txt
.\tmux.exe -L $server kill-server 2>$null
notepad $env:TEMP\trace-highlight-capture.txt
```

Expected: only the title/header and one data row have the cyan background
sequence. If multiple data rows are highlighted in this capture, the problem
is before final console rendering.

For the phased update path:

```powershell
$server = 'trace-highlight-phase-model'
.\tmux.exe -L $server kill-server 2>$null
.\tmux.exe -L $server new-session -d -x 120 -y 36 -- "$PWD\tools\win32-highlight-repro.exe --phase --phase-delay 16 --auto 12 --delay 80 --hold"
Start-Sleep -Seconds 2
.\tmux.exe -L $server capture-pane -e -p -S 0 -E 35 > $env:TEMP\trace-highlight-phase-capture.txt
.\tmux.exe -L $server kill-server 2>$null
notepad $env:TEMP\trace-highlight-phase-capture.txt
```

## ntop trace

Use the same process for `ntop.exe`:

```powershell
$env:PATH = 'C:\ddev\tmux4win\sysroot\bin;C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:TERM = 'xterm-256color'
Remove-Item .\tmux-*.log,.\tmux-out-*.log -ErrorAction SilentlyContinue
.\tmux.exe -vv -L trace-ntop new-session -x 140 -y 40 -- 'C:\Users\chi\AppData\Local\Microsoft\WinGet\Links\ntop.exe'
```

Navigate until the trail appears, then quit `ntop`. Keep the log files and
also run this detached model check:

```powershell
$server = 'trace-ntop-model'
.\tmux.exe -L $server kill-server 2>$null
.\tmux.exe -L $server new-session -d -x 140 -y 40 -- 'C:\Users\chi\AppData\Local\Microsoft\WinGet\Links\ntop.exe'
Start-Sleep -Seconds 2
1..8 | ForEach-Object { .\tmux.exe -L $server send-keys Down; Start-Sleep -Milliseconds 150 }
Start-Sleep -Milliseconds 1
.\tmux.exe -L $server capture-pane -e -p -S 0 -E 39 > $env:TEMP\trace-ntop-capture.txt
.\tmux.exe -L $server kill-server 2>$null
notepad $env:TEMP\trace-ntop-capture.txt
```

Send back:

- Whether the deterministic repro trails.
- Whether `trace-highlight-capture.txt` or `trace-ntop-capture.txt` has
  multiple highlighted data rows.
- The newest `tmux-server-*.log`, `tmux-client-*.log`, and `tmux-out-*.log`
  from the visible artifact run.
