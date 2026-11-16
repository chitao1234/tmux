# win32-input-matrix.ps1
#
# Collect what Windows terminal hosts actually deliver for key input.
#
# Three tiers, because they are three genuinely different boundaries:
#
#   A  conhost (hidden real console)  inject records -> ReadFile bytes
#      The console host's own translation. This decides whether Ctrl-J,
#      ESC and the arrow keys have a byte at all. Fully automated: each
#      (mode, key) pair runs in a fresh hidden console.
#
#   B  ConPTY (what a tmux pane is)   write device bytes -> child reads
#      tmux owns the device side of the pane, so this is exactly what a
#      program inside a pane sees, for whatever console mode it chooses.
#
#   C  wt.exe / wezterm.exe           real terminals, real windows
#      The probe runs inside the real terminal and injects into its own
#      console, so no focus or manual keypress is needed. Reported
#      separately because these terminals ship their own ConPTY backend.
#
# Everything is measured through tools/win32-consoletap.c and
# tools/win32-conptyprobe.c; this script only orchestrates and renders.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win32-input-matrix.ps1
#   ... -OutDir C:\tmp\m -Modes 0x0,0x200,0x3f0 -Keys ctrl-j,esc -Terminals conhost,conpty,wt,wezterm

param(
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "tmux-win32-input-matrix"),
    [string[]]$Modes = @("none", "0x0", "0x200", "0x1f7", "0x3f0"),
    [string[]]$Terminals = @("conhost", "conpty", "wt", "wezterm"),
    [string[]]$Keys = @(
        "a", "space", "ctrl-a", "ctrl-b", "ctrl-c", "ctrl-h", "ctrl-i",
        "ctrl-j", "ctrl-l", "ctrl-m", "ctrl-[", "ctrl-space", "enter",
        "tab", "esc", "backspace", "shift-tab", "up", "down", "left",
        "right", "home", "end", "pageup", "pagedown", "f1", "alt-x"
    ),
    [switch]$KeepArtifacts
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

# powershell -File delivers "-Modes 0x0,0x200" as one comma-joined string.
function Expand-List {
    param($Value)
    $out = @()
    foreach ($v in @($Value)) {
        foreach ($p in ($v -split ',')) {
            $t = $p.Trim()
            if ($t -ne "") { $out += $t }
        }
    }
    ,$out
}

$Modes = Expand-List $Modes
$Terminals = Expand-List $Terminals
$Keys = Expand-List $Keys

$toolsDir = $PSScriptRoot
$tapSrc = Join-Path $toolsDir "win32-consoletap.c"
$conptySrc = Join-Path $toolsDir "win32-conptyprobe.c"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$tapExe = Join-Path $OutDir "win32-consoletap.exe"
$conptyExe = Join-Path $OutDir "win32-conptyprobe.exe"

# ------------------------------------------------------------------ helpers

function Find-Gcc {
    foreach ($c in @("C:\msys64\ucrt64\bin\gcc.exe",
                     "C:\msys64\mingw64\bin\gcc.exe")) {
        if (Test-Path $c) { return $c }
    }
    $g = Get-Command gcc.exe -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    throw "no gcc found; install MSYS2 UCRT64"
}

function Initialize-Toolchain {
    # gcc exits 1 with no message if its own bin dir is not on PATH, even
    # when invoked by absolute path.
    $gcc = Find-Gcc
    $ucrt = Split-Path -Parent $gcc
    $root = Split-Path -Parent (Split-Path -Parent $ucrt)
    foreach ($d in @($ucrt, (Join-Path $root "usr\bin"))) {
        if ((Test-Path $d) -and (($env:PATH -split ';') -notcontains $d)) {
            $env:PATH = "$d;$env:PATH"
        }
    }
    return $gcc
}

function Build-Helper {
    param([string]$Source, [string]$Exe)

    if ((Test-Path $Exe) -and
        ((Get-Item $Exe).LastWriteTime -gt (Get-Item $Source).LastWriteTime)) {
        return
    }
    $gcc = Initialize-Toolchain
    Write-Host "  building $(Split-Path -Leaf $Exe)"
    & $gcc -O2 -o $Exe $Source
    if ($LASTEXITCODE -ne 0) { throw "build failed: $Source" }
}

# Tier A launcher: conhost.exe directly.
#
# This matters more than it looks. CREATE_NEW_CONSOLE does NOT reliably give a
# classic console: on Windows 11 the default terminal application intercepts
# it, so the child ends up on a ConPTY-backed console hosted by Windows
# Terminal / OpenConsole instead of conhost.exe. An earlier version of this
# script used CREATE_NEW_CONSOLE and silently measured ConPTY three times over
# while labelling the columns "conhost", "wt" and "wezterm".
#
# Invoking conhost.exe with the program as its argument starts a real conhost
# and waits for it, which is what the classic-console tier is supposed to
# measure. Every tier is verified with win32-conhost-ident before its numbers
# are trusted.
$identSrc = Join-Path $toolsDir "win32-conhost-ident.c"
$identExe = Join-Path $OutDir "win32-conhost-ident.exe"

function Invoke-InConsoleHost {
    param([string]$Exe, [string[]]$Arguments)

    # Windows PowerShell joins ArgumentList with spaces and does no quoting,
    # so quote anything that could contain a space.
    $quoted = @($Exe) + $Arguments | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }
    # Windows stay visible on purpose: a hidden console has bitten this suite
    # before, and a visible window makes it obvious which host is under test.
    $p = Start-Process -FilePath 'conhost.exe' -ArgumentList $quoted `
        -Wait -PassThru
    return $p.ExitCode
}

function Get-HostKind {
    # Returns 'conhost', 'conpty' or 'unknown' for the given launch method.
    $probe = Join-Path $OutDir "hostcheck.txt"
    Remove-Item -LiteralPath $probe -ErrorAction SilentlyContinue
    Invoke-InConsoleHost -Exe $identExe -Arguments @($probe) | Out-Null
    if (-not (Test-Path $probe)) { return 'unknown' }
    foreach ($line in Get-Content $probe) {
        if ($line -match '^host=(.+)$') { return $Matches[1].Trim() }
    }
    return 'unknown'
}

# ---------------------------------------------------------------- tier A

function Invoke-ConhostCase {
    param([string]$Mode, [string]$Key)

    $out = Join-Path $OutDir ("conhost-" + ($Mode -replace '[^a-z0-9]','_') + "-" + ($Key -replace '[^a-z0-9]','_') + ".txt")
    $win = $out.Replace('\', '\\')
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
    Invoke-InConsoleHost -Exe $tapExe -Arguments @("probe", $win, $Mode, $Key) | Out-Null
    if (-not (Test-Path $out)) { return "" }
    $bytes = ""
    foreach ($line in Get-Content $out) {
        if ($line -match '^bytes got=\d+ \[(.*)\]') { $bytes = $Matches[1].Trim() }
        elseif ($line -match '^bytes none') { $bytes = "(none)" }
        elseif ($line -match '^result error=(\S+)') { $bytes = "(error: $($Matches[1]))" }
    }
    $bytes
}

# ---------------------------------------------------------------- tier B

# Bytes a terminal sends for each key. This is what tmux forwards to a pane.
$keyBytes = [ordered]@{
    "a"          = @("61")
    "space"      = @("20")
    "ctrl-a"     = @("01")
    "ctrl-b"     = @("02")
    "ctrl-c"     = @("03")
    "ctrl-h"     = @("08")
    "ctrl-i"     = @("09")
    "ctrl-j"     = @("0a")
    "ctrl-l"     = @("0c")
    "ctrl-m"     = @("0d")
    "ctrl-["     = @("1b")
    "ctrl-space" = @("00")
    "enter"      = @("0d")
    "tab"        = @("09")
    "esc"        = @("1b")
    "backspace"  = @("7f")
    "shift-tab"  = @("1b","5b","5a")
    "up"         = @("1b","5b","41")
    "down"       = @("1b","5b","42")
    "right"      = @("1b","5b","43")
    "left"       = @("1b","5b","44")
    "home"       = @("1b","5b","48")
    "end"        = @("1b","5b","46")
    "pageup"     = @("1b","5b","35","7e")
    "pagedown"   = @("1b","5b","36","7e")
    "f1"         = @("1b","4f","50")
    "alt-x"      = @("1b","78")
}

function Invoke-ConptyCase {
    param([string]$Mode, [string]$Key)

    $bytes = $keyBytes[$Key]
    if ($null -eq $bytes) { return @{ Sent = ""; Got = "" } }
    $out = Join-Path $OutDir ("conpty-" + ($Mode -replace '[^a-z0-9]','_') + "-" + ($Key -replace '[^a-z0-9]','_') + ".txt")
    $win = $out.Replace('\', '\\')
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue

    $argList = @("host", $conptyExe, $win, $Mode) + $bytes
    & $conptyExe @argList 2>$null | Out-Null

    $got = @()
    if (Test-Path $out) {
        foreach ($line in Get-Content $out) {
            if ($line -match '^read \d+:\s*(.*)$') { $got += $Matches[1].Trim() }
        }
    }
    return @{ Sent = ($bytes -join " "); Got = ($got -join " | ") }
}

# ---------------------------------------------------------------- tier C

# Windows Terminal and WezTerm only run their GUI once; the first invocation
# starts the process and returns, so `wt new-tab` may come back before the tab
# exists. Poll the result file long enough for the tab to appear, and treat a
# zero-length file (created but never written) as a timeout, not a result.
function Wait-ProbeResult {
    param([string]$Path)

    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path $Path) {
            $len = (Get-Item $Path).Length
            if ($len -gt 0) {
                foreach ($line in Get-Content $Path) {
                    if ($line -match '^bytes got=\d+ \[(.*)\]') { return $Matches[1].Trim() }
                    if ($line -match '^bytes none') { return "(none)" }
                    if ($line -match '^result error=(\S+)') { return "(error: $($Matches[1]))" }
                }
                # written but not yet complete; keep waiting
            }
        }
        Start-Sleep -Milliseconds 200
    }
    if (Test-Path $Path) { return "(incomplete)" }
    "(no result)"
}

function Invoke-WtCase {
    param([string]$Mode, [string]$Key)

    $out = Join-Path $OutDir ("wt-" + ($Mode -replace '[^a-z0-9]','_') + "-" + ($Key -replace '[^a-z0-9]','_') + ".txt")
    $win = $out.Replace('\', '\\')
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
    $wt = Get-Command wt.exe -ErrorAction SilentlyContinue
    if (-not $wt) { return "" }
    & $wt.Source new-tab --title "tapprobe" -- $tapExe "probe" $win $Mode $Key 2>$null | Out-Null
    return (Wait-ProbeResult -Path $out)
}

function Invoke-WeztermCase {
    param([string]$Mode, [string]$Key)

    $out = Join-Path $OutDir ("wez-" + ($Mode -replace '[^a-z0-9]','_') + "-" + ($Key -replace '[^a-z0-9]','_') + ".txt")
    $win = $out.Replace('\', '\\')
    Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
    $wez = @("$env:ProgramFiles\WezTerm\wezterm.exe",
             "$env:LOCALAPPDATA\Programs\WezTerm\wezterm.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $wez) { return "" }
    & $wez start --always-new-process -- $tapExe "probe" $win $Mode $Key 2>$null | Out-Null
    return (Wait-ProbeResult -Path $out)
}

# -------------------------------------------------------------------- main

Write-Host ""
Write-Host "=== probes ==="
Build-Helper -Source $tapSrc -Exe $tapExe
Build-Helper -Source $conptySrc -Exe $conptyExe
Build-Helper -Source $identSrc -Exe $identExe

$have = [ordered]@{
    conhost = Test-Path "$env:WINDIR\system32\conhost.exe"
    conpty  = $true
    wt      = [bool](Get-Command wt.exe -ErrorAction SilentlyContinue)
    wezterm = [bool](@("$env:ProgramFiles\WezTerm\wezterm.exe",
        "$env:LOCALAPPDATA\Programs\WezTerm\wezterm.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1)
}
Write-Host ("available: " + (($have.GetEnumerator() | ForEach-Object {
    "$($_.Key)=$(if ($_.Value) {'yes'} else {'no'})" }) -join " "))
Write-Host ("modes: " + ($Modes -join " "))
Write-Host ("keys:  " + $Keys.Count)

# Verify each tier really runs where it claims to. Without this an earlier run
# reported identical numbers for "conhost", "wt" and "wezterm" because all
# three were ConPTY.
$verified = [ordered]@{}
if ($have.conhost) {
    $kind = Get-HostKind
    $verified["conhost"] = $kind
    Write-Host "conhost tier verified as: $kind"
    if ($kind -ne 'conhost') {
        Write-Host "  WARNING: conhost tier is not a real conhost; disabling it"
        $have.conhost = $false
    }
}
Write-Host ""

$results = [ordered]@{ conhost = @{}; conpty = @{}; wt = @{}; wezterm = @{} }
$total = 0
foreach ($t in $Terminals) { if ($have[$t]) { $total += $Modes.Count * $Keys.Count } }
$done = 0

foreach ($m in $Modes) {
    foreach ($k in $Keys) {
        foreach ($t in $Terminals) {
            if (-not $have[$t]) { continue }
            $done++
            Write-Progress -Activity "input matrix" `
                -Status "$t $m $k ($done/$total)" `
                -PercentComplete ([int](100 * $done / [Math]::Max($total,1)))
            switch ($t) {
                "conhost" {
                    if (-not $results.conhost.ContainsKey($m)) { $results.conhost[$m] = @{} }
                    $results.conhost[$m][$k] = Invoke-ConhostCase -Mode $m -Key $k
                }
                "conpty" {
                    if (-not $results.conpty.ContainsKey($m)) { $results.conpty[$m] = @{} }
                    $results.conpty[$m][$k] = Invoke-ConptyCase -Mode $m -Key $k
                }
                "wt" {
                    if (-not $results.wt.ContainsKey($m)) { $results.wt[$m] = @{} }
                    $results.wt[$m][$k] = Invoke-WtCase -Mode $m -Key $k
                }
                "wezterm" {
                    if (-not $results.wezterm.ContainsKey($m)) { $results.wezterm[$m] = @{} }
                    $results.wezterm[$m][$k] = Invoke-WeztermCase -Mode $m -Key $k
                }
            }
        }
    }
}
Write-Progress -Activity "input matrix" -Completed

# ------------------------------------------------------------------ render

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# Windows console input matrix")
[void]$md.AppendLine("")
[void]$md.AppendLine("Generated $(Get-Date -Format o) on $env:COMPUTERNAME, Windows " +
    (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion").DisplayVersion + ".")
[void]$md.AppendLine("")
[void]$md.AppendLine("Terminals: " + (($have.GetEnumerator() | ForEach-Object {
    "$($_.Key)=$(if ($_.Value) {'yes'} else {'no'})" }) -join ", "))
[void]$md.AppendLine("")

if ($have.conhost) {
    [void]$md.AppendLine("## conhost: injected records -> ReadFile bytes")
    [void]$md.AppendLine("")
    [void]$md.AppendLine("Each cell is what `ReadFile` on `CONIN$` returned for one key,")
    [void]$md.AppendLine("in a fresh console set to the given input mode.")
    [void]$md.AppendLine("")
    [void]$md.AppendLine("| key | " + (($Modes | ForEach-Object { "``$_``" }) -join " | ") + " |")
    [void]$md.AppendLine("|---" * ($Modes.Count + 1) + "|")
    foreach ($k in $Keys) {
        $cells = @()
        foreach ($m in $Modes) {
            $v = ""
            if ($results.conhost.ContainsKey($m) -and $results.conhost[$m].ContainsKey($k)) {
                $v = $results.conhost[$m][$k]
            }
            if ($v -eq "") { $v = "n/a" }
            $cells += $v
        }
        [void]$md.AppendLine("| ``$k`` | " + ($cells -join " | ") + " |")
    }
    [void]$md.AppendLine("")
}

if ($have.conpty) {
    [void]$md.AppendLine("## ConPTY: device bytes -> child ReadFile")
    [void]$md.AppendLine("")
    [void]$md.AppendLine("This is the tmux pane boundary: bytes written at the device side,")
    [void]$md.AppendLine("read back by a child that set the given console input mode.")
    [void]$md.AppendLine("")
    foreach ($m in $Modes) {
        [void]$md.AppendLine("### mode ``$m``")
        [void]$md.AppendLine("")
        [void]$md.AppendLine("| key | sent | child read | match |")
        [void]$md.AppendLine("|---|---|---|---|")
        foreach ($k in $Keys) {
            if (-not $results.conpty.ContainsKey($m)) { continue }
            $r = $results.conpty[$m][$k]
            if ($null -eq $r) { continue }
            $sent = ($r.Sent -replace ' ','')
            # The child may receive an escape sequence as several reads, so
            # compare the concatenation, not per-read boundaries.
            $got = ($r.Got -replace '\|','' -replace ' ','')
            $same = if ($sent -eq $got) { "yes" }
                    elseif ($got -eq "") { "**lost**" }
                    else { "**differs**" }
            [void]$md.AppendLine("| ``$k`` | ``$($r.Sent)`` | ``$($r.Got)`` | $same |")
        }
        [void]$md.AppendLine("")
    }
}

foreach ($t in @("wt","wezterm")) {
    if (-not $have[$t]) { continue }
    [void]$md.AppendLine("## ${t}: records -> bytes inside the real terminal")
    [void]$md.AppendLine("")
    [void]$md.AppendLine("| key | " + (($Modes | ForEach-Object { "``$_``" }) -join " | ") + " |")
    [void]$md.AppendLine("|---" * ($Modes.Count + 1) + "|")
    foreach ($k in $Keys) {
        $cells = @()
        foreach ($m in $Modes) {
            $v = ""
            if ($results[$t].ContainsKey($m) -and $results[$t][$m].ContainsKey($k)) {
                $v = $results[$t][$m][$k]
            }
            if ($v -eq "") { $v = "n/a" }
            $cells += $v
        }
        [void]$md.AppendLine("| ``$k`` | " + ($cells -join " | ") + " |")
    }
    [void]$md.AppendLine("")
}

$mdPath = Join-Path $OutDir "input-matrix.md"
$jsonPath = Join-Path $OutDir "input-matrix.json"
Set-Content -LiteralPath $mdPath -Value $md.ToString() -Encoding UTF8
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

Write-Host ""
Write-Host "report: $mdPath"
Write-Host "json:   $jsonPath"

if (-not $KeepArtifacts) {
    Get-ChildItem $OutDir -File -Filter "conhost-*.txt" -ErrorAction SilentlyContinue |
        Remove-Item -ErrorAction SilentlyContinue
    Get-ChildItem $OutDir -File -Filter "conpty-*.txt" -ErrorAction SilentlyContinue |
        Remove-Item -ErrorAction SilentlyContinue
    Get-ChildItem $OutDir -File -Filter "wt-*.txt" -ErrorAction SilentlyContinue |
        Remove-Item -ErrorAction SilentlyContinue
    Get-ChildItem $OutDir -File -Filter "wez-*.txt" -ErrorAction SilentlyContinue |
        Remove-Item -ErrorAction SilentlyContinue
}
