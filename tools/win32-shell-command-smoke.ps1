param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-shell-command",
    [switch]$KeepArtifacts
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function ConvertTo-Win32Argument {
    param([string]$Argument)

    if ($Argument.Length -ne 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $result = '"'
    $backslashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
            continue
        }
        if ($character -eq '"') {
            $result += '\' * (($backslashes * 2) + 1)
            $result += '"'
            $backslashes = 0
            continue
        }
        if ($backslashes -ne 0) {
            $result += '\' * $backslashes
            $backslashes = 0
        }
        $result += $character
    }
    if ($backslashes -ne 0) {
        $result += '\' * ($backslashes * 2)
    }
    $result += '"'
    $result
}

function Join-Win32Arguments {
    param([string[]]$Arguments)

    (@($Arguments) | ForEach-Object { ConvertTo-Win32Argument $_ }) -join " "
}

function Invoke-Tmux {
    param(
        [string[]]$Arguments,
        [string]$WorkingDirectory = (Get-Location).Path,
        [hashtable]$Environment = @{},
        [string[]]$RemoveEnvironment = @(),
        [switch]$AllowFailure
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments

    foreach ($name in $RemoveEnvironment) {
        $null = $psi.Environment.Remove($name)
    }
    foreach ($entry in $Environment.GetEnumerator()) {
        $psi.Environment[$entry.Key] = $entry.Value
    }

    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    $output = @()
    if ($stdout.Length -ne 0) {
        $output += @($stdout -split "`r?`n" | Where-Object { $_.Length -ne 0 })
    }
    if ($stderr.Length -ne 0) {
        $output += @($stderr -split "`r?`n" | Where-Object { $_.Length -ne 0 })
    }

    if (-not $AllowFailure -and $process.ExitCode -ne 0) {
        throw "tmux $($Arguments -join ' ') failed with exit code $($process.ExitCode): $($output -join "`n")"
    }

    [pscustomobject]@{
        ExitCode = $process.ExitCode
        Output = $output
    }
}

function New-CaseDirectory {
    param(
        [string]$Root,
        [string]$Name
    )

    $path = Join-Path $Root $Name
    New-Item -ItemType Directory -Path $path | Out-Null
    $path
}

function Wait-Until {
    param(
        [scriptblock]$Condition,
        [int]$TimeoutMs = 5000,
        [int]$IntervalMs = 100,
        [string]$Description = "condition"
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while (-not (& $Condition)) {
        if ([DateTime]::UtcNow -gt $deadline) {
            throw "timed out waiting for $Description"
        }
        Start-Sleep -Milliseconds $IntervalMs
    }
}

function Normalize-ComparablePath {
    param([string]$Path)

    if ($null -eq $Path) {
        return $null
    }
    return $Path.TrimEnd('\', '/')
}

function Test-ComparablePathEquals {
    param(
        [string]$Left,
        [string]$Right
    )

    return [string]::Equals(
        (Normalize-ComparablePath $Left),
        (Normalize-ComparablePath $Right),
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function New-CaptureHelper {
    param([string]$Path)

    @'
@echo off
if /I "%~1"=="--write" (
    > "%~2" (
        echo ARGS=[%~3]^|[%~4]
    )
    exit /b 0
)
if /I "%~1"=="--edit" (
    > "%~2" (
        echo popup editor ok
    )
    exit /b 0
)
echo ARGS=[%~1]^|[%~2]
'@ | Set-Content -LiteralPath $Path -Encoding ASCII
}

function New-Label {
    param([string]$Suffix)

    $token = [guid]::NewGuid().ToString("N").Substring(0, 8)
    "$LabelPrefix-$Suffix-$token"
}

function Start-TmuxClientProcess {
    param(
        [string[]]$Arguments,
        [string]$WorkingDirectory = (Get-Location).Path,
        [hashtable]$Environment = @{},
        [string[]]$RemoveEnvironment = @()
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments

    foreach ($name in $RemoveEnvironment) {
        $null = $psi.Environment.Remove($name)
    }
    foreach ($entry in $Environment.GetEnumerator()) {
        $psi.Environment[$entry.Key] = $entry.Value
    }

    $process = [System.Diagnostics.Process]::Start($psi)
    [pscustomobject]@{
        Process = $process
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
    }
}

function Wait-TmuxClientProcess {
    param(
        $Run,
        [int]$TimeoutMs,
        [string]$Description
    )

    if (-not $Run.Process.WaitForExit($TimeoutMs)) {
        $Run.Process.Kill()
        throw "$Description did not exit within ${TimeoutMs}ms"
    }

    [pscustomobject]@{
        ExitCode = $Run.Process.ExitCode
        Stdout = $Run.Stdout.Result
        Stderr = $Run.Stderr.Result
    }
}

function Get-TmuxClientName {
    param(
        [string]$Label,
        [string]$WorkingDirectory,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds(5000)
    while ([DateTime]::UtcNow -le $deadline) {
        $result = Invoke-Tmux -Arguments @("-L", $Label, "list-clients",
            "-F", "#{client_name}") -WorkingDirectory $WorkingDirectory `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment `
            -AllowFailure
        if ($result.ExitCode -eq 0 -and $result.Output.Count -ne 0) {
            return $result.Output[0]
        }
        Start-Sleep -Milliseconds 100
    }
    throw "timed out waiting for attached client"
}

function Invoke-DefaultShellCase {
    param(
        [string]$Root,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment
    )

    $caseDir = New-CaseDirectory $Root "default-shell"
    $label = New-Label "default"

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session", "-d") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null
        $show = Invoke-Tmux -Arguments @("-L", $label, "show-options", "-g",
            "default-shell") -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment
        $showText = $show.Output -join "`n"
        $showValue = ($showText -replace '^default-shell ', '').Replace('\\', '\')
        Assert-True -Condition (Test-ComparablePathEquals $showValue $env:ComSpec) `
            -Message ("default-shell did not fall back to ComSpec: " + $showText)
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-PaneShellCommandCase {
    param(
        [string]$Root,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment
    )

    $caseDir = New-CaseDirectory $Root "pane-shell-command"
    $label = New-Label "pane"
    $helperDir = Join-Path $caseDir "helper dir"
    $helper = Join-Path $helperDir "capture.cmd"
    $command = $null

    New-Item -ItemType Directory -Path $helperDir | Out-Null
    New-CaptureHelper -Path $helper
    $command = ('"{0}" "pane alpha" "pane beta" & echo CHAIN=[ok]' -f $helper)

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "-s", "probe") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "set-option", "-pt",
            "probe:0.0", "remain-on-exit", "on") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-k", "-t",
            "probe:0.0", $command) -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment | Out-Null
        Start-Sleep -Milliseconds 500
        $capture = Invoke-Tmux -Arguments @("-L", $label, "capture-pane",
            "-pt", "probe:0.0", "-S", "-", "-p") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment
        $text = $capture.Output -join "`n"
        Assert-True -Condition ($text.Contains("ARGS=[pane alpha]|[pane beta]")) `
            -Message ("pane shell-command lost argument structure: " + $text)
        Assert-True -Condition ($text.Contains("CHAIN=[ok]")) `
            -Message ("pane shell-command did not execute chained cmd.exe text: " + $text)
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-JobShellCommandCase {
    param(
        [string]$Root,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment
    )

    $caseDir = New-CaseDirectory $Root "job-shell-command"
    $label = New-Label "job"
    $helperDir = Join-Path $caseDir "helper dir"
    $helper = Join-Path $helperDir "capture.cmd"
    $marker = Join-Path $caseDir "job marker.txt"
    $command = $null

    New-Item -ItemType Directory -Path $helperDir | Out-Null
    New-CaptureHelper -Path $helper
    $command = ('"{0}" --write "{1}" "job alpha" "job beta"' -f $helper, $marker)

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d") -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "run-shell", "-b", $command) `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null
        Wait-Until -TimeoutMs 5000 -IntervalMs 100 -Description "job marker" `
            -Condition { Test-Path -LiteralPath $marker }
        $text = Get-Content -LiteralPath $marker -Raw
        Assert-True -Condition ($text.Contains("ARGS=[job alpha]|[job beta]")) `
            -Message ("run-shell lost argument structure: " + $text)
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-PopupArgvCase {
    param(
        [string]$Root,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment
    )

    $caseDir = New-CaseDirectory $Root "popup-argv"
    $label = New-Label "popup"
    $helperDir = Join-Path $caseDir "helper dir"
    $helper = Join-Path $helperDir "capture.cmd"
    $marker = Join-Path $caseDir "popup marker.txt"
    $run = $null
    $clientName = $null

    New-Item -ItemType Directory -Path $helperDir | Out-Null
    New-CaptureHelper -Path $helper

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "-s", "probe") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment | Out-Null
        $run = Start-TmuxClientProcess -Arguments @("-L", $label, "-f", "NUL",
            "attach-session", "-t", "probe") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment
        $clientName = Get-TmuxClientName -Label $label `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment

        Invoke-Tmux -Arguments @("-L", $label, "display-popup", "-c",
            $clientName, "-t", "probe:0.0", "-E", "--", "cmd.exe", "/d",
            "/c", "call", $helper, "--write", $marker, "popup alpha",
            "popup beta") -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 100 -Description "popup marker" `
            -Condition { Test-Path -LiteralPath $marker }
        $text = Get-Content -LiteralPath $marker -Raw
        Assert-True -Condition ($text.Contains("ARGS=[popup alpha]|[popup beta]")) `
            -Message ("display-popup argv path lost argument structure: " + $text)
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment -AllowFailure | Out-Null
        if ($run -ne $null) {
            Wait-TmuxClientProcess -Run $run -TimeoutMs 5000 `
                -Description "popup argv client" | Out-Null
        }
        Pop-Location
    }
}

function Invoke-PopupEditorCase {
    param(
        [string]$Root,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment
    )

    $caseDir = New-CaseDirectory $Root "popup-editor"
    $label = New-Label "editor"
    $helperDir = Join-Path $caseDir "helper dir"
    $helper = Join-Path $helperDir "capture.cmd"
    $editor = $null
    $run = $null
    $clientName = $null

    New-Item -ItemType Directory -Path $helperDir | Out-Null
    New-CaptureHelper -Path $helper
    $editor = ('cmd.exe /d /c call "{0}" --edit' -f $helper)

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "-s", "probe") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "set-option", "-g", "editor",
            $editor) -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "set-buffer", "-b", "sample",
            "original popup text") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment | Out-Null

        $run = Start-TmuxClientProcess -Arguments @("-L", $label, "-f", "NUL",
            "attach-session", "-t", "probe") -WorkingDirectory $caseDir `
            -Environment $Environment -RemoveEnvironment $RemoveEnvironment
        $clientName = Get-TmuxClientName -Label $label `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment

        Invoke-Tmux -Arguments @("-L", $label, "choose-buffer", "-t",
            "probe:0.0") -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null
        Wait-Until -TimeoutMs 5000 -IntervalMs 100 `
            -Description "choose-buffer mode" -Condition {
                $info = Invoke-Tmux -Arguments @("-L", $label, "display-message",
                    "-p", "-t", "probe:0.0", "mode=[#{pane_in_mode}] pane_mode=[#{pane_mode}]") `
                    -WorkingDirectory $caseDir -Environment $Environment `
                    -RemoveEnvironment $RemoveEnvironment
                return (($info.Output -join "") -match 'pane_mode=\[buffer-mode\]')
            }

        Invoke-Tmux -Arguments @("-L", $label, "send-keys", "-c",
            $clientName, "-t", "probe:0.0", "e") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 100 `
            -Description "edited popup buffer" -Condition {
                $buffer = Invoke-Tmux -Arguments @("-L", $label, "show-buffer",
                    "-b", "sample") -WorkingDirectory $caseDir `
                    -Environment $Environment -RemoveEnvironment $RemoveEnvironment
                return (($buffer.Output -join "`n").Contains("popup editor ok"))
            }
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") `
            -WorkingDirectory $caseDir -Environment $Environment `
            -RemoveEnvironment $RemoveEnvironment -AllowFailure | Out-Null
        if ($run -ne $null) {
            Wait-TmuxClientProcess -Run $run -TimeoutMs 5000 `
                -Description "popup editor client" | Out-Null
        }
        Pop-Location
    }
}

$script:TmuxPath = [System.IO.Path]::GetFullPath($TmuxPath)
$root = Join-Path $env:TEMP ($LabelPrefix + "-" + [guid]::NewGuid().ToString("N"))
$envMap = @{
    SHELL = ""
    TMUX_WIN32_CONSOLE_RELAY = "0"
    COLUMNS = "132"
    LINES = "43"
}
$removeEnv = @("TMUX", "TMUX_WIN32_HANDLE_TTY")

New-Item -ItemType Directory -Path $root | Out-Null

try {
    Invoke-DefaultShellCase -Root $root -Environment $envMap `
        -RemoveEnvironment $removeEnv
    Invoke-PaneShellCommandCase -Root $root -Environment $envMap `
        -RemoveEnvironment $removeEnv
    Invoke-JobShellCommandCase -Root $root -Environment $envMap `
        -RemoveEnvironment $removeEnv
    Invoke-PopupArgvCase -Root $root -Environment $envMap `
        -RemoveEnvironment $removeEnv
    Invoke-PopupEditorCase -Root $root -Environment $envMap `
        -RemoveEnvironment $removeEnv
    "Win32 shell-command smoke passed"
} finally {
    if ($KeepArtifacts) {
        "Artifacts kept at $root"
    } else {
        Remove-Item -Recurse -Force $root
    }
}
