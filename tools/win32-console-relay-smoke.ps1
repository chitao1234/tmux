param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-console-relay-smoke",
    [switch]$RemoveLogsOnSuccess
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

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
        [switch]$AllowFailure
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments

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

function Invoke-TmuxInteractive {
    param([string[]]$Arguments)

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.Arguments = Join-Win32Arguments $Arguments

    $process = [System.Diagnostics.Process]::Start($psi)
    $process.WaitForExit()
    $process.ExitCode
}

function Test-AnyLogMatch {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    foreach ($log in $Logs) {
        if (Select-String -LiteralPath $log.FullName -Pattern $Pattern -Quiet) {
            return $true
        }
    }
    $false
}

function Get-LogMatches {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    foreach ($log in $Logs) {
        Select-String -LiteralPath $log.FullName -Pattern $Pattern |
            ForEach-Object {
                "{0}:{1}: {2}" -f $log.Name, $_.LineNumber, $_.Line
            }
    }
}

if ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected) {
    throw "This smoke must be run from a real Windows console, not redirected output or the Codex runner."
}
if (-not (Test-Path -LiteralPath $TmuxPath -PathType Leaf)) {
    throw "tmux executable not found: $TmuxPath"
}
$script:TmuxPath = (Resolve-Path -LiteralPath $TmuxPath).Path

$savedEnv = @{
    TMUX = $env:TMUX
    TMUX_WIN32_HANDLE_TTY = $env:TMUX_WIN32_HANDLE_TTY
    TMUX_WIN32_CONSOLE_RELAY = $env:TMUX_WIN32_CONSOLE_RELAY
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-console-relay-smoke-" + [Guid]::NewGuid().ToString("N"))
$label = "$LabelPrefix-" + [Guid]::NewGuid().ToString("N")
$config = Join-Path $root "empty.conf"
New-Item -ItemType Directory -Path $root | Out-Null
New-Item -ItemType File -Path $config | Out-Null

try {
    $env:TMUX = $null
    $env:TMUX_WIN32_HANDLE_TTY = "0"
    $env:TMUX_WIN32_CONSOLE_RELAY = $null

    Push-Location $root
    try {
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "relay", "cmd.exe") | Out-Null

        Write-Host "Launching native-console relay attach."
        Write-Host "Press Ctrl-b then d to detach. Logs will be checked afterward: $root"

        $attachCode = Invoke-TmuxInteractive -Arguments @("-f", $config, "-vv", "-L", $label, "attach-session", "-t", "relay")

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
    } finally {
        Pop-Location
    }

    $logs = @(Get-ChildItem -LiteralPath $root -Filter "tmux-*.log" -File)
    $relayFallback = Test-AnyLogMatch $logs "using Win32 console relay fallback"
    $relayIdentify = Test-AnyLogMatch $logs "IDENTIFY_WIN32_TERMINAL"
    $directOutput = Test-AnyLogMatch $logs "using direct Win32 terminal output|IDENTIFY_WIN32_STDOUT duplicated|IDENTIFY_WIN32_STDIN duplicated"
    $failures = @(Get-LogMatches $logs "rejected|ReadFile failed|WriteFile failed|output error")

    Write-Host ""
    Write-Host "Smoke summary:"
    Write-Host "  attach exit code: $attachCode"
    Write-Host "  relay fallback selected: $relayFallback"
    Write-Host "  relay terminal identify: $relayIdentify"
    Write-Host "  direct handle path used: $directOutput"
    Write-Host "  logs: $root"

    if ($failures.Count -ne 0) {
        Write-Host ""
        Write-Host "Failure lines:"
        $failures | ForEach-Object { Write-Host "  $_" }
    }

    $passed = $attachCode -eq 0 -and $relayFallback -and $relayIdentify -and
        -not $directOutput -and $failures.Count -eq 0
    if ($passed) {
        Write-Host ""
        Write-Host "Native-console relay fallback smoke passed."
        if ($RemoveLogsOnSuccess) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
        exit 0
    }

    Write-Host ""
    Write-Host "Native-console relay fallback smoke failed."
    exit 1
} finally {
    $env:TMUX = $savedEnv.TMUX
    $env:TMUX_WIN32_HANDLE_TTY = $savedEnv.TMUX_WIN32_HANDLE_TTY
    $env:TMUX_WIN32_CONSOLE_RELAY = $savedEnv.TMUX_WIN32_CONSOLE_RELAY
}
