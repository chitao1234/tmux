param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-console-relay-smoke",
    [switch]$ExerciseInputCredit,
    [switch]$SimulateOutputLoss,
    [switch]$SimulateTransportLost,
    [switch]$ExerciseDetachBacklog,
    [switch]$ExerciseOutputProgress,
    [switch]$ExerciseResizeBacklog,
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
    param(
        [string[]]$Arguments,
        [int]$TimeoutMs = 0,
        [scriptblock]$AfterStart
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.Arguments = Join-Win32Arguments $Arguments

    $process = [System.Diagnostics.Process]::Start($psi)
    $processId = $process.Id
    if ($AfterStart -ne $null) {
        & $AfterStart $processId
    }
    if ($TimeoutMs -gt 0) {
        if (-not $process.WaitForExit($TimeoutMs)) {
            $process.Kill()
            throw "tmux $($Arguments -join ' ') did not exit within ${TimeoutMs}ms"
        }
    } else {
        $process.WaitForExit()
    }
    [pscustomobject]@{
        ExitCode = $process.ExitCode
        ProcessId = $processId
    }
}

function Initialize-ConsoleInputInterop {
    if ("Win32ConsoleInput" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class Win32ConsoleInput
{
    [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
    public struct INPUT_RECORD
    {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public KEY_EVENT_RECORD KeyEvent;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct KEY_EVENT_RECORD
    {
        [MarshalAs(UnmanagedType.Bool)] public bool bKeyDown;
        public ushort wRepeatCount;
        public ushort wVirtualKeyCode;
        public ushort wVirtualScanCode;
        public char UnicodeChar;
        public uint dwControlKeyState;
    }

    const ushort KEY_EVENT = 0x0001;
    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 0x00000001;
    const uint FILE_SHARE_WRITE = 0x00000002;
    const uint OPEN_EXISTING = 3;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint desiredAccess,
        uint shareMode, IntPtr securityAttributes, uint creationDisposition,
        uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteConsoleInputW(IntPtr consoleInput,
        INPUT_RECORD[] buffer, uint length, out uint eventsWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);

    public static void WriteText(string text)
    {
        if (string.IsNullOrEmpty(text))
            return;

        IntPtr handle = CreateFileW("CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0,
            IntPtr.Zero);
        if (handle == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "CreateFileW(CONIN$) failed");

        try
        {
            INPUT_RECORD[] records = new INPUT_RECORD[text.Length];
            for (int i = 0; i < text.Length; i++)
            {
                records[i].EventType = KEY_EVENT;
                records[i].KeyEvent.bKeyDown = true;
                records[i].KeyEvent.wRepeatCount = 1;
                records[i].KeyEvent.wVirtualKeyCode = 0;
                records[i].KeyEvent.wVirtualScanCode = 0;
                records[i].KeyEvent.UnicodeChar = text[i];
                records[i].KeyEvent.dwControlKeyState = 0;
            }

            uint written;
            if (!WriteConsoleInputW(handle, records, (uint)records.Length,
                out written))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "WriteConsoleInputW failed");
            if (written != (uint)records.Length)
                throw new InvalidOperationException(
                    "WriteConsoleInputW wrote " + written + " of " +
                    records.Length + " events");
        }
        finally
        {
            CloseHandle(handle);
        }
    }
}
"@
}

function Write-ConsoleInputText {
    param(
        [string]$Text,
        [int]$ChunkSize = 512,
        [int]$ChunkDelayMs = 1
    )

    Initialize-ConsoleInputInterop

    $offset = 0
    while ($offset -lt $Text.Length) {
        $length = [Math]::Min($ChunkSize, $Text.Length - $offset)
        [Win32ConsoleInput]::WriteText($Text.Substring($offset, $length))
        $offset += $length
        if ($ChunkDelayMs -gt 0 -and $offset -lt $Text.Length) {
            Start-Sleep -Milliseconds $ChunkDelayMs
        }
    }
}

function Get-ConsoleWindowSize {
    $size = $Host.UI.RawUI.WindowSize
    [pscustomobject]@{
        Width = $size.Width
        Height = $size.Height
    }
}

function Get-AlternateConsoleWindowSize {
    param(
        [int]$Width,
        [int]$Height
    )

    $targetWidth = $Width
    $targetHeight = $Height

    if ($Width -gt 100) {
        $targetWidth = $Width - 8
    } elseif ($Height -gt 32) {
        $targetHeight = $Height - 4
    } elseif ($Width -gt 52) {
        $targetWidth = $Width - 4
    } elseif ($Height -gt 20) {
        $targetHeight = $Height - 2
    } else {
        $targetWidth = $Width + 4
    }
    if ($targetWidth -eq $Width -and $targetHeight -eq $Height) {
        throw "Could not choose an alternate console size from ${Width}x${Height}."
    }

    [pscustomobject]@{
        Width = $targetWidth
        Height = $targetHeight
    }
}

function Set-ConsoleWindowSize {
    param(
        [int]$Width,
        [int]$Height
    )

    $raw = $Host.UI.RawUI
    $buffer = $raw.BufferSize
    $bufferWidth = [Math]::Max($buffer.Width, $Width)
    $bufferHeight = [Math]::Max($buffer.Height, $Height)

    if ($bufferWidth -ne $buffer.Width -or $bufferHeight -ne $buffer.Height) {
        $raw.BufferSize = [System.Management.Automation.Host.Size]::new(
            $bufferWidth,
            $bufferHeight
        )
    }
    $raw.WindowSize = [System.Management.Automation.Host.Size]::new(
        $Width,
        $Height
    )
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

function Get-LogMaximumCapture {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    $maximum = $null
    foreach ($log in $Logs) {
        foreach ($match in Select-String -LiteralPath $log.FullName -Pattern $Pattern) {
            $value = [int64]$match.Matches[0].Groups[1].Value
            if ($maximum -eq $null -or $value -gt $maximum) {
                $maximum = $value
            }
        }
    }
    if ($maximum -eq $null) {
        return -1
    }
    $maximum
}

function Get-TmuxClientSize {
    param(
        [string]$Config,
        [string]$Label,
        [int]$ClientPid
    )

    $result = Invoke-Tmux -Arguments @(
        "-f",
        $Config,
        "-L",
        $Label,
        "list-clients",
        "-F",
        "#{client_name} #{client_width} #{client_height}"
    ) -AllowFailure
    if ($result.ExitCode -ne 0) {
        return $null
    }
    foreach ($line in $result.Output) {
        if ($line -match "^client-$ClientPid\s+([0-9]+)\s+([0-9]+)$") {
            return [pscustomobject]@{
                Width = [int]$Matches[1]
                Height = [int]$Matches[2]
            }
        }
    }
    $null
}

function Wait-TmuxClientSize {
    param(
        [string]$Config,
        [string]$Label,
        [int]$ClientPid,
        [int]$Width,
        [int]$Height,
        [int]$TimeoutMs = 3000
    )

    $deadline = [Environment]::TickCount64 + $TimeoutMs
    do {
        $size = Get-TmuxClientSize -Config $Config -Label $Label -ClientPid $ClientPid
        if ($size -ne $null -and $size.Width -eq $Width -and $size.Height -eq $Height) {
            return $size
        }
        Start-Sleep -Milliseconds 100
    } while ([Environment]::TickCount64 -lt $deadline)

    $null
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
    TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS
    TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-console-relay-smoke-" + [Guid]::NewGuid().ToString("N"))
$label = "$LabelPrefix-" + [Guid]::NewGuid().ToString("N")
$config = Join-Path $root "empty.conf"
New-Item -ItemType Directory -Path $root | Out-Null
New-Item -ItemType File -Path $config | Out-Null

try {
    $restoreConsoleSize = $null
    $resizeTarget = $null
    $resizeObserved = $null

    $env:TMUX = $null
    $env:TMUX_WIN32_HANDLE_TTY = "0"
    $env:TMUX_WIN32_CONSOLE_RELAY = $null
    if ($SimulateOutputLoss) {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = "1"
    } else {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = $null
    }
    if ($SimulateTransportLost) {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = "1"
    } else {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = $null
    }

    Push-Location $root
    try {
        $sessionCommand = "cmd.exe"
        if ($ExerciseInputCredit) {
            $sessionCommand = 'cmd.exe /Q /K "findstr .* >nul"'
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "relay", $sessionCommand) | Out-Null

        Write-Host "Launching native-console relay attach."
        if ($ExerciseInputCredit) {
            Write-Host "Relay input-credit exercise is enabled. Large native console input will be injected and the attach client will be detached automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 25000 -AfterStart {
                param([int]$AttachPid)

                $builder = [System.Text.StringBuilder]::new()
                $line = "relay-input-credit-" + [string]::new([char]'x', 48)

                Start-Sleep -Milliseconds 750
                for ($i = 0; $i -lt 1100; $i++) {
                    [void]$builder.Append($line).Append("`r")
                }
                Write-ConsoleInputText -Text $builder.ToString() -ChunkSize 4096 -ChunkDelayMs 0
                Start-Sleep -Milliseconds 1500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($SimulateOutputLoss) {
            Write-Host "Relay output loss is being simulated. Attach should fail automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 10000
        } elseif ($SimulateTransportLost) {
            Write-Host "Relay transport loss is being simulated while output is in flight. Attach should fail automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 15000 -AfterStart {
                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,6000) do @echo relay-transport-loss-0123456789abcdefghijklmnopqrstuvwxyz"
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
            }
        } elseif ($ExerciseDetachBacklog) {
            Write-Host "Relay detach-under-backlog exercise is enabled. Output will be generated and detach will happen while relay output is still pending."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 20000 -AfterStart {
                param([int]$AttachPid)

                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,20000) do @echo relay-detach-backlog-0123456789abcdefghijklmnopqrstuvwxyz"
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
                Start-Sleep -Milliseconds 150
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseOutputProgress) {
            Write-Host "Relay output progress exercise is enabled. Output will be generated and the attach client will be detached automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 20000 -AfterStart {
                param([int]$AttachPid)

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
                    "for /L %i in (1,1,6000) do @echo relay-progress-0123456789abcdefghijklmnopqrstuvwxyz"
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
                Start-Sleep -Milliseconds 250
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "set-option",
                    "-g",
                    "status-left",
                    "relay-output-progress-backlog"
                ) | Out-Null
                Start-Sleep -Milliseconds 1500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseResizeBacklog) {
            Write-Host "Relay resize-under-backlog exercise is enabled. Output will be generated, the real console will be resized, and the attach client will be detached automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 20000 -AfterStart {
                param([int]$AttachPid)

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
                    "for /L %i in (1,1,9000) do @echo relay-resize-backlog-0123456789abcdefghijklmnopqrstuvwxyz"
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
                Start-Sleep -Milliseconds 250
                $script:restoreConsoleSize = Get-ConsoleWindowSize
                $script:resizeTarget = Get-AlternateConsoleWindowSize `
                    -Width $script:restoreConsoleSize.Width `
                    -Height $script:restoreConsoleSize.Height
                Set-ConsoleWindowSize -Width $script:resizeTarget.Width `
                    -Height $script:resizeTarget.Height
                $script:resizeObserved = Wait-TmuxClientSize `
                    -Config $config `
                    -Label $label `
                    -ClientPid $AttachPid `
                    -Width $script:resizeTarget.Width `
                    -Height $script:resizeTarget.Height `
                    -TimeoutMs 3000
                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } else {
            Write-Host "Press Ctrl-b then d to detach. Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            )
        }

        $attachCode = $attach.ExitCode
        $attachPid = $attach.ProcessId
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
    } finally {
        Pop-Location
    }

    $logs = @(Get-ChildItem -LiteralPath $root -Filter "tmux-*.log" -File)
    $attachLogs = @()
    $attachLogPath = Join-Path $root ("tmux-client-{0}.log" -f $attachPid)
    if (Test-Path -LiteralPath $attachLogPath -PathType Leaf) {
        $attachLogs = @([System.IO.FileInfo](Get-Item -LiteralPath $attachLogPath))
    }
    if ($attachLogs.Count -eq 0) {
        $attachLogs = $logs
    }

    $relayMode = Test-AnyLogMatch $attachLogs "using Win32 console relay (terminal transport|fallback)"
    $relayIdentify = Test-AnyLogMatch $logs "IDENTIFY_WIN32_TERMINAL"
    $inputCredit = Test-AnyLogMatch $attachLogs "Win32 input credit"
    $directOutput = Test-AnyLogMatch $attachLogs "using direct Win32 terminal output|IDENTIFY_WIN32_STDOUT duplicated|IDENTIFY_WIN32_STDIN duplicated"
    $outputAbort = Test-AnyLogMatch $logs "Win32 output abort|dropping [0-9]+ pending bytes|simulating Win32 console relay output loss"
    $transportLost = Test-AnyLogMatch $logs "relay transport lost|simulating Win32 console relay transport loss|transport-lost"
    $closePending = Test-AnyLogMatch $logs "Win32 relay close pending"
    $inputPauseCount = Get-LogMatchCount $attachLogs "console input paused \((credit exhausted|[0-9]+ reserved bytes pending send)"
    $inputResumeCount = Get-LogMatchCount $attachLogs "console input resumed"
    $inputReturnedCount = Get-LogMatchCount $logs "Win32 input credit returned"
    $inputPeakReserved = Get-LogMaximumCapture $attachLogs "console input peak reserved ([0-9]+) bytes, peak reader buffered [0-9]+ bytes"
    $inputPeakBuffered = Get-LogMaximumCapture $attachLogs "console input peak reserved [0-9]+ bytes, peak reader buffered ([0-9]+) bytes"
    $outputProgressCount = Get-LogMatchCount $attachLogs "client_win32_output_progress: progressed"
    $redrawDeferredCount = Get-LogMatchCount $logs "redraw deferred"
    $waitingForRedrawCount = Get-LogMatchCount $logs "waiting for redraw, [0-9]+ bytes left"
    $statusRedraw = Test-AnyLogMatch $logs "redraw status"
    $resizeClientLog = $false
    $resizeServerLog = $false
    if ($ExerciseResizeBacklog -and $resizeTarget -ne $null) {
        $resizeClientLog = Test-AnyLogMatch $attachLogs (
            "console size is now $($resizeTarget.Width)x$($resizeTarget.Height)"
        )
        $resizeServerLog = Test-AnyLogMatch $logs (
            "server_client_win32_resize: client-$attachPid now " +
            "$($resizeTarget.Width)x$($resizeTarget.Height)"
        )
    }
    $failures = @(Get-LogMatches $logs "rejected|ReadFile failed|WriteFile failed|output error")

    Write-Host ""
    Write-Host "Smoke summary:"
    Write-Host "  attach exit code: $attachCode"
    Write-Host "  relay terminal mode selected: $relayMode"
    Write-Host "  relay terminal identify: $relayIdentify"
    Write-Host "  input credit observed: $inputCredit"
    Write-Host "  direct handle path used: $directOutput"
    if ($ExerciseInputCredit) {
        Write-Host "  input pause events: $inputPauseCount"
        Write-Host "  input resume events: $inputResumeCount"
        Write-Host "  returned credit events: $inputReturnedCount"
        Write-Host "  peak reserved bytes: $inputPeakReserved"
        Write-Host "  peak reader buffered bytes: $inputPeakBuffered"
    }
    if ($SimulateTransportLost) {
        Write-Host "  transport lost observed: $transportLost"
    }
    if ($ExerciseDetachBacklog) {
        Write-Host "  close pending observed: $closePending"
    }
    if ($ExerciseOutputProgress) {
        Write-Host "  output progress events: $outputProgressCount"
        Write-Host "  redraw deferred events: $redrawDeferredCount"
        Write-Host "  waiting-for-redraw events: $waitingForRedrawCount"
        Write-Host "  status redraw observed: $statusRedraw"
    }
    if ($ExerciseResizeBacklog) {
        if ($resizeTarget -ne $null) {
            Write-Host "  resize target: $($resizeTarget.Width)x$($resizeTarget.Height)"
        }
        if ($resizeObserved -ne $null) {
            Write-Host "  resize observed by server query: $($resizeObserved.Width)x$($resizeObserved.Height)"
        } else {
            Write-Host "  resize observed by server query: <none>"
        }
        Write-Host "  client resize log observed: $resizeClientLog"
        Write-Host "  server resize log observed: $resizeServerLog"
        Write-Host "  output progress events: $outputProgressCount"
        Write-Host "  redraw deferred events: $redrawDeferredCount"
    }
    if ($SimulateOutputLoss) {
        Write-Host "  output abort observed: $outputAbort"
    }
    Write-Host "  logs: $root"

    if ($failures.Count -ne 0) {
        Write-Host ""
        Write-Host "Failure lines:"
        $failures | ForEach-Object { Write-Host "  $_" }
    }

    if ($ExerciseInputCredit) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $inputPauseCount -ge 1 -and
            $inputResumeCount -ge 2 -and $inputReturnedCount -ge 2 -and
            $inputPeakReserved -ge 0 -and $inputPeakReserved -le 65536 -and
            $inputPeakBuffered -ge 0 -and $inputPeakBuffered -le 65536 -and
            $failures.Count -eq 0
    } elseif ($SimulateOutputLoss) {
        $passed = $attachCode -ne 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $outputAbort -and
            $failures.Count -eq 0
    } elseif ($SimulateTransportLost) {
        $passed = $attachCode -ne 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $transportLost -and
            $failures.Count -eq 0
    } elseif ($ExerciseDetachBacklog) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $closePending -and
            $outputProgressCount -ge 1 -and $failures.Count -eq 0
    } elseif ($ExerciseOutputProgress) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
        $inputCredit -and -not $directOutput -and
            $outputProgressCount -ge 2 -and $redrawDeferredCount -ge 1 -and
            $waitingForRedrawCount -ge 1 -and $statusRedraw -and
            $failures.Count -eq 0
    } elseif ($ExerciseResizeBacklog) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $resizeTarget -ne $null -and
            $resizeObserved -ne $null -and
            $resizeObserved.Width -eq $resizeTarget.Width -and
            $resizeObserved.Height -eq $resizeTarget.Height -and
            $resizeClientLog -and $resizeServerLog -and
            $outputProgressCount -ge 1 -and $redrawDeferredCount -ge 1 -and
            $failures.Count -eq 0
    } else {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $failures.Count -eq 0
    }
    if ($passed) {
        Write-Host ""
        if ($ExerciseInputCredit) {
            Write-Host "Native-console relay input-credit smoke passed."
        } elseif ($SimulateOutputLoss) {
            Write-Host "Native-console relay output-loss smoke passed."
        } elseif ($SimulateTransportLost) {
            Write-Host "Native-console relay transport-loss smoke passed."
        } elseif ($ExerciseDetachBacklog) {
            Write-Host "Native-console relay detach-backlog smoke passed."
        } elseif ($ExerciseOutputProgress) {
            Write-Host "Native-console relay output-progress smoke passed."
        } elseif ($ExerciseResizeBacklog) {
            Write-Host "Native-console relay resize-backlog smoke passed."
        } else {
            Write-Host "Native-console relay smoke passed."
        }
        if ($RemoveLogsOnSuccess) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
        exit 0
    }

    Write-Host ""
    if ($ExerciseInputCredit) {
        Write-Host "Native-console relay input-credit smoke failed."
    } elseif ($SimulateOutputLoss) {
        Write-Host "Native-console relay output-loss smoke failed."
    } elseif ($SimulateTransportLost) {
        Write-Host "Native-console relay transport-loss smoke failed."
    } elseif ($ExerciseDetachBacklog) {
        Write-Host "Native-console relay detach-backlog smoke failed."
    } elseif ($ExerciseOutputProgress) {
        Write-Host "Native-console relay output-progress smoke failed."
    } elseif ($ExerciseResizeBacklog) {
        Write-Host "Native-console relay resize-backlog smoke failed."
    } else {
        Write-Host "Native-console relay smoke failed."
    }
    exit 1
} finally {
    if ($restoreConsoleSize -ne $null) {
        try {
            Set-ConsoleWindowSize -Width $restoreConsoleSize.Width `
                -Height $restoreConsoleSize.Height
        } catch {
            Write-Warning "Failed to restore console size to $($restoreConsoleSize.Width)x$($restoreConsoleSize.Height): $_"
        }
    }
    $env:TMUX = $savedEnv.TMUX
    $env:TMUX_WIN32_HANDLE_TTY = $savedEnv.TMUX_WIN32_HANDLE_TTY
    $env:TMUX_WIN32_CONSOLE_RELAY = $savedEnv.TMUX_WIN32_CONSOLE_RELAY
    $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = $savedEnv.TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS
    $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = $savedEnv.TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST
}
