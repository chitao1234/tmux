# Run the HIGH -> MEDIUM and MEDIUM -> HIGH transport probes.
#
# Sequence:
#   1. MEDIUM server started from this (unelevated) shell, on an explicit -S
#      endpoint so both integrity levels address the same object
#   2. HIGH client connects to that MEDIUM server   -> expect success
#   3. HIGH server started from an elevated shell, same explicit endpoint
#   4. MEDIUM client connects to that HIGH server   -> expect denial
#
# -S is required: -L resolves to %LOCALAPPDATA%\tmux-<integrity>\<label>, so a
# HIGH client asking for a MEDIUM label searches the tmux-h directory and
# never finds the MEDIUM server at all. That is a naming issue, not a policy
# result, and it would make this test meaningless.
param(
    [string]$TmuxPath = 'C:\ddev\tmux4win\tmux\tmux.exe',
    [string]$LabelPrefix = 'ilprobe',
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'tmux-il')
)

$ErrorActionPreference = 'Stop'
$toolsDir = $PSScriptRoot
$probe = Join-Path $toolsDir 'win32-il-probe.ps1'

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Get-ChildItem $OutDir -File -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue

# One endpoint shared by both integrity levels.
$sockDir = Join-Path $OutDir 'endpoint'
New-Item -ItemType Directory -Path $sockDir -Force | Out-Null
$sockPath = Join-Path $sockDir $LabelPrefix

$failures = New-Object System.Collections.Generic.List[string]

function Wait-Result {
    param([string]$Path, [int]$TimeoutMs = 60000)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path $Path) {
            $t = Get-Content $Path -Raw -ErrorAction SilentlyContinue
            if ($t -and $t.Contains('mode=')) { return $t }
        }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

function Invoke-Elevated {
    param([string]$Mode, [string]$Label)
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', $probe,
        '-Mode', $Mode,
        '-Label', $Label,
        '-TmuxPath', $TmuxPath,
        '-OutDir', $OutDir,
        '-SocketPath', $sockPath
    ) -PassThru -Wait | Out-Null
}

function Invoke-Unelevated {
    param([string]$Mode, [string]$Label)
    & powershell -NoProfile -ExecutionPolicy Bypass -File $probe `
        -Mode $Mode -Label $Label -TmuxPath $TmuxPath -OutDir $OutDir `
        -SocketPath $sockPath
}

# ---------------------------------------------------------------- case 1+2
$medLabel = "$LabelPrefix-med"
$highClientLabel = "$LabelPrefix-highclient"

Write-Host "[1/4] MEDIUM server"
Invoke-Unelevated -Mode server -Label $medLabel
$med = Wait-Result (Join-Path $OutDir "$medLabel.txt") 30000
if ($null -eq $med) { $failures.Add('medium server never reported') }
Write-Host $med

Write-Host "[2/4] HIGH client -> MEDIUM server"
Invoke-Elevated -Mode client -Label $highClientLabel
$hc = Wait-Result (Join-Path $OutDir "$highClientLabel.txt") 90000
if ($null -eq $hc) { $failures.Add('elevated client never reported') }
Write-Host $hc
if ($hc -and $hc -notmatch 'elevated=True') {
    $failures.Add('client step did not actually run elevated')
}

& $TmuxPath -S $sockPath kill-server 2>$null | Out-Null
Start-Sleep -Milliseconds 800

# ---------------------------------------------------------------- case 3+4
$highLabel = "$LabelPrefix-high"
$medClientLabel = "$LabelPrefix-medclient"

Write-Host "[3/4] HIGH server"
Invoke-Elevated -Mode server -Label $highLabel
$high = Wait-Result (Join-Path $OutDir "$highLabel.txt") 90000
if ($null -eq $high) { $failures.Add('elevated server never reported') }
Write-Host $high

Write-Host "[4/4] MEDIUM client -> HIGH server"
Invoke-Unelevated -Mode client -Label $medClientLabel
$mc = Wait-Result (Join-Path $OutDir "$medClientLabel.txt") 30000
if ($null -eq $mc) { $failures.Add('medium client never reported') }
Write-Host $mc

# ---------------------------------------------------------------- verdict
if ($hc -and $hc -notmatch 'ls_exit=0') {
    $failures.Add('HIGH client was refused by MEDIUM server')
}
if ($mc -and $mc -match 'ls_exit=0') {
    $failures.Add('MEDIUM client reached a HIGH server (should be denied)')
}

& $TmuxPath -S $sockPath kill-server 2>$null | Out-Null

Write-Host ''
if ($failures.Count -ne 0) {
    Write-Host 'FAILURES:'
    $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'il-matrix passed'
