# Spawn tmux at a chosen elevation and report what it can see.
#
# Two modes, because the interesting case is asymmetric:
#
#   server  start a server in this (possibly elevated) process context
#   client  connect to an existing server from this context
#
# The point is to prove that a HIGH client can reach a MEDIUM server, while a
# MEDIUM client cannot reach a HIGH server.
param(
    [Parameter(Mandatory = $true)][ValidateSet('server', 'client')]
    [string]$Mode,
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$TmuxPath = 'C:\ddev\tmux4win\tmux\tmux.exe',
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'tmux-il'),
    # Explicit socket path. Required for cross-integrity testing: -L resolves
    # to %LOCALAPPDATA%\tmux-<integrity>\<label>, so a HIGH client looking up
    # a MEDIUM server's label would search tmux-h and never see tmux-m.
    [string]$SocketPath = ''
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$out = Join-Path $OutDir "$Label.txt"

$lines = New-Object System.Collections.Generic.List[string]
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
$lines.Add("label=$Label")
$lines.Add("mode=$Mode")
$lines.Add("user=" + $id.Name)
$lines.Add("elevated=" + $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
$lines.Add("socket=" + $SocketPath)

# -S picks the endpoint explicitly; -L would pick a per-integrity directory.
$target = if ($SocketPath -ne '') { @('-S', $SocketPath) } else { @('-L', $Label) }
$lines.Add("target=" + ($target -join ' '))

$env:TERM = 'xterm-256color'
$env:TERMINFO = 'C:/msys64/ucrt64/share/terminfo'
# tmux.exe links against UCRT64 libs (libevent, ncursesw, utf8proc, ...), so the
# runtime needs them on PATH. Without this the process dies with 0xC0000135
# (STATUS_DLL_NOT_FOUND) before main() runs.
foreach ($d in @('C:\msys64\ucrt64\bin', 'C:\msys64\usr\bin')) {
    if ((Test-Path $d) -and (($env:PATH -split ';') -notcontains $d)) {
        $env:PATH = "$d;$env:PATH"
    }
}

function Invoke-Tmux {
    param([string[]]$Arguments)

    # Must NOT use Start-Process: a detached tmux server created that way is
    # killed when the launcher exits, so every subsequent step would see "no
    # server running". A direct invocation keeps the server alive.
    #
    # stderr is expected to carry messages like "no server running" (from the
    # initial kill-server, for instance), which is not an error here, so
    # ErrorActionPreference is relaxed for the duration of the call.
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $o = & $TmuxPath @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    [pscustomobject]@{
        ExitCode = $exit
        Stdout   = ($o -replace '\s+$', '')
        Stderr   = ''
    }
}

switch ($Mode) {
    'server' {
        $null = Invoke-Tmux ($target + @('kill-server'))
        Start-Sleep -Milliseconds 400
        $r = Invoke-Tmux ($target + @('new-session', '-d', '-s', 's',
            'C:\Windows\System32\ping.exe', '-n', '90', '127.0.0.1'))
        $lines.Add("new_exit=" + $r.ExitCode)
        if ($r.Stderr) { $lines.Add("new_stderr=" + $r.Stderr) }
    }
    'client' {
        $r = Invoke-Tmux ($target + @('list-sessions'))
        $lines.Add("ls_exit=" + $r.ExitCode)
        if ($r.Stdout) { $lines.Add("ls_stdout=" + $r.Stdout) }
        if ($r.Stderr) { $lines.Add("ls_stderr=" + $r.Stderr) }
    }
}

[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
