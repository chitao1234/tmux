param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-console-relay-reader-loss-smoke",
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

function Get-LogMatchCount {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    $count = 0
    foreach ($log in $Logs) {
        $count += @(Select-String -LiteralPath $log.FullName -Pattern $Pattern).Count
    }
    $count
}

function Get-CurrentTmuxLogs {
    param(
        [string]$Path
    )

    @(Get-ChildItem -LiteralPath $Path -Filter "tmux-*.log" -File)
}

function Wait-LogMatch {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$TimeoutMs = 3000
    )

    $deadline = [Environment]::TickCount64 + $TimeoutMs
    do {
        $logs = Get-CurrentTmuxLogs -Path $Path
        if (Test-AnyLogMatch $logs $Pattern) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    } while ([Environment]::TickCount64 -lt $deadline)

    $false
}

function Initialize-WindowCloseInterop {
    if ("Win32RelayWindow" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class Win32RelayWindow
{
    public const uint WM_CLOSE = 0x0010;

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessageW(IntPtr window, uint message,
        IntPtr wParam, IntPtr lParam);
}
"@
}

function New-AttachBootstrapScript {
    param(
        [string]$Path
    )

    @'
param(
    [string]$TmuxPath,
    [string]$ConfigPath,
    [string]$Label,
    [string]$StatePath
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class Win32ChildConsole
{
    [DllImport("kernel32.dll")]
    public static extern IntPtr GetConsoleWindow();
}
"@

"{0} {1}" -f $PID, [Win32ChildConsole]::GetConsoleWindow().ToInt64() |
    Set-Content -LiteralPath $StatePath -NoNewline

& $TmuxPath -f $ConfigPath -vv -L $Label attach-session -t relay
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $Path -Encoding ASCII
}

function Wait-AttachConsoleState {
    param(
        [string]$Path,
        [int]$TimeoutMs = 5000
    )

    $deadline = [Environment]::TickCount64 + $TimeoutMs
    do {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $text = Get-Content -LiteralPath $Path -Raw
            if ($text -match '^\s*([0-9]+)\s+(-?[0-9]+)\s*$') {
                return [pscustomobject]@{
                    ProcessId = [int]$Matches[1]
                    WindowHandle = [int64]$Matches[2]
                }
            }
        }
        Start-Sleep -Milliseconds 100
    } while ([Environment]::TickCount64 -lt $deadline)

    throw "Timed out waiting for attach console state at $Path"
}

if (-not (Test-Path -LiteralPath $TmuxPath -PathType Leaf)) {
    throw "tmux executable not found: $TmuxPath"
}

$script:TmuxPath = (Resolve-Path -LiteralPath $TmuxPath).Path
Initialize-WindowCloseInterop

$savedEnv = @{
    TMUX = $env:TMUX
    TMUX_WIN32_HANDLE_TTY = $env:TMUX_WIN32_HANDLE_TTY
    TMUX_WIN32_CONSOLE_RELAY = $env:TMUX_WIN32_CONSOLE_RELAY
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-console-relay-reader-loss-smoke-" + [Guid]::NewGuid().ToString("N"))
$label = "$LabelPrefix-" + [Guid]::NewGuid().ToString("N")
$config = Join-Path $root "empty.conf"
$bootstrap = Join-Path $root "attach-bootstrap.ps1"
$statePath = Join-Path $root "attach-state.txt"
New-Item -ItemType Directory -Path $root | Out-Null
New-Item -ItemType File -Path $config | Out-Null
New-AttachBootstrapScript -Path $bootstrap

try {
    $env:TMUX = $null
    $env:TMUX_WIN32_HANDLE_TTY = "0"
    $env:TMUX_WIN32_CONSOLE_RELAY = $null

    Push-Location $root
    try {
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "new-session",
            "-d",
            "-s",
            "relay",
            'cmd.exe /Q /K'
        ) | Out-Null

        Write-Host "Launching relay attach in a child console window."
        Write-Host "That child console will be closed automatically to exercise native reader loss."
        Write-Host "Logs will be checked afterward: $root"

        $attachShell = Start-Process -FilePath "powershell.exe" -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            $bootstrap,
            $script:TmuxPath,
            $config,
            $label,
            $statePath
        ) -WorkingDirectory $root -WindowStyle Normal -PassThru

        $attachState = Wait-AttachConsoleState -Path $statePath
        Start-Sleep -Milliseconds 750

        Invoke-Tmux -Arguments @(
            "-f",
            $config,
            "-L",
            $label,
            "send-keys",
            "-t",
            "relay",
            "-l",
            "for /L %i in (1,1,6000) do @echo relay-reader-loss-0123456789abcdefghijklmnopqrstuvwxyz"
        ) | Out-Null
        Invoke-Tmux -Arguments @(
            "-f",
            $config,
            "-L",
            $label,
            "send-keys",
            "-t",
            "relay",
            "Enter"
        ) | Out-Null

        $outputProgressReady = Wait-LogMatch -Path $root `
            -Pattern "client_win32_output_progress: progressed" `
            -TimeoutMs 3000

        $windowClosePosted = [Win32RelayWindow]::PostMessageW(
            [IntPtr]::new($attachState.WindowHandle),
            [Win32RelayWindow]::WM_CLOSE,
            [IntPtr]::Zero,
            [IntPtr]::Zero
        )
        if (-not $windowClosePosted) {
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw "PostMessageW(WM_CLOSE) failed with Win32 error $errorCode"
        }

        if (-not $attachShell.WaitForExit(15000)) {
            $attachShell.Kill()
            throw "Attach console process did not exit after closing the child console window."
        }

        $attachShellExitCode = $attachShell.ExitCode
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
    } finally {
        Pop-Location
    }

    $logs = Get-CurrentTmuxLogs -Path $root
    $clientLogs = @(Get-ChildItem -LiteralPath $root -Filter "tmux-client-*.log" -File)
    $attachLogs = @(
        $clientLogs | Where-Object {
            Select-String -LiteralPath $_.FullName `
                -Pattern "using Win32 console relay terminal transport" `
                -Quiet
        }
    )
    if ($attachLogs.Count -eq 0) {
        $attachLogs = $logs
    }

    $relayMode = Test-AnyLogMatch $attachLogs "using Win32 console relay terminal transport"
    $relayIdentify = Test-AnyLogMatch $logs "IDENTIFY_WIN32_TERMINAL"
    $inputCredit = Test-AnyLogMatch $attachLogs "Win32 input credit"
    $directOutput = Test-AnyLogMatch $attachLogs "using direct Win32 terminal output|IDENTIFY_WIN32_STDOUT duplicated|IDENTIFY_WIN32_STDIN duplicated"
    $clientInputClosed = Test-AnyLogMatch $attachLogs "console input closed"
    $clientTransportLost = Test-AnyLogMatch $attachLogs "Win32 console relay transport lost \(console input closed\)"
    $serverTransportLost = Test-AnyLogMatch $logs "relay transport lost \(input closed\)"
    $outputProgressCount = Get-LogMatchCount $attachLogs "client_win32_output_progress: progressed"
    $interestingFailures = @(Get-LogMatches $logs "rejected|output error")

    Write-Host ""
    Write-Host "Smoke summary:"
    Write-Host "  attach shell exit code: $attachShellExitCode"
    Write-Host "  relay terminal mode selected: $relayMode"
    Write-Host "  relay terminal identify: $relayIdentify"
    Write-Host "  input credit observed: $inputCredit"
    Write-Host "  direct handle path used: $directOutput"
    Write-Host "  output progress observed before close: $outputProgressReady"
    Write-Host "  client input-closed observed: $clientInputClosed"
    Write-Host "  client transport-lost observed: $clientTransportLost"
    Write-Host "  server transport-lost observed: $serverTransportLost"
    Write-Host "  output progress events: $outputProgressCount"
    Write-Host "  logs: $root"

    if ($interestingFailures.Count -ne 0) {
        Write-Host ""
        Write-Host "Failure lines:"
        $interestingFailures | ForEach-Object { Write-Host "  $_" }
    }

    $passed = $relayMode -and $relayIdentify -and $inputCredit -and
        -not $directOutput -and $outputProgressReady -and
        $clientInputClosed -and $clientTransportLost -and
        $serverTransportLost -and $outputProgressCount -ge 1 -and
        $interestingFailures.Count -eq 0
    if ($passed) {
        Write-Host ""
        Write-Host "Native-console relay reader-loss smoke passed."
        if ($RemoveLogsOnSuccess) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
        exit 0
    }

    Write-Host ""
    Write-Host "Native-console relay reader-loss smoke failed."
    exit 1
} finally {
    $env:TMUX = $savedEnv.TMUX
    $env:TMUX_WIN32_HANDLE_TTY = $savedEnv.TMUX_WIN32_HANDLE_TTY
    $env:TMUX_WIN32_CONSOLE_RELAY = $savedEnv.TMUX_WIN32_CONSOLE_RELAY
}
