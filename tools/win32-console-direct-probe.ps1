param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-console-direct-probe",
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
    throw "This probe must be run from a real Windows console, not redirected output or the Codex runner."
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

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-console-direct-probe-" + [Guid]::NewGuid().ToString("N"))
$label = "$LabelPrefix-" + [Guid]::NewGuid().ToString("N")
$config = Join-Path $root "empty.conf"
New-Item -ItemType Directory -Path $root | Out-Null
New-Item -ItemType File -Path $config | Out-Null

try {
    $env:TMUX = $null
    $env:TMUX_WIN32_HANDLE_TTY = "force"
    $env:TMUX_WIN32_CONSOLE_RELAY = "0"

    Push-Location $root
    try {
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "probe", "cmd.exe") | Out-Null

        Write-Host "Launching forced direct native-console attach."
        Write-Host "If the screen works, press Ctrl-b then d to detach."
        Write-Host "If it fails or hangs, close the tmux client and inspect logs at: $root"

        $attachCode = Invoke-TmuxInteractive -Arguments @("-f", $config, "-vv", "-L", $label, "attach-session", "-t", "probe")

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
    } finally {
        Pop-Location
    }

    $logs = @(Get-ChildItem -LiteralPath $root -Filter "tmux-*.log" -File)
    $forced = Test-AnyLogMatch $logs "forcing direct Win32 terminal handles by TMUX_WIN32_HANDLE_TTY=force"
    $direct = Test-AnyLogMatch $logs "using direct Win32 terminal output and input"
    $stdout = Test-AnyLogMatch $logs "IDENTIFY_WIN32_STDOUT duplicated"
    $stdin = Test-AnyLogMatch $logs "IDENTIFY_WIN32_STDIN duplicated"
    $size = Test-AnyLogMatch $logs "IDENTIFY_WIN32_SIZE"
    $relay = Test-AnyLogMatch $logs "IDENTIFY_WIN32_TERMINAL|using Win32 console relay fallback"
    $failures = @(Get-LogMatches $logs "rejected|ReadFile failed|WriteFile failed|output error|input closed, events 0x20")

    Write-Host ""
    Write-Host "Probe summary:"
    Write-Host "  attach exit code: $attachCode"
    Write-Host "  forced direct mode: $forced"
    Write-Host "  direct input/output: $direct"
    Write-Host "  stdout duplicated: $stdout"
    Write-Host "  stdin duplicated: $stdin"
    Write-Host "  size identified: $size"
    Write-Host "  relay used: $relay"
    Write-Host "  logs: $root"

    if ($failures.Count -ne 0) {
        Write-Host ""
        Write-Host "Failure lines:"
        $failures | ForEach-Object { Write-Host "  $_" }
    }

    $passed = $attachCode -eq 0 -and $forced -and $direct -and $stdout -and
        $stdin -and $size -and -not $relay -and $failures.Count -eq 0
    if ($passed) {
        Write-Host ""
        Write-Host "Forced native-console direct handles appear usable in this run."
        if ($RemoveLogsOnSuccess) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
        exit 0
    }

    Write-Host ""
    Write-Host "Forced native-console direct handles did not pass this probe."
    exit 1
} finally {
    $env:TMUX = $savedEnv.TMUX
    $env:TMUX_WIN32_HANDLE_TTY = $savedEnv.TMUX_WIN32_HANDLE_TTY
    $env:TMUX_WIN32_CONSOLE_RELAY = $savedEnv.TMUX_WIN32_CONSOLE_RELAY
}
