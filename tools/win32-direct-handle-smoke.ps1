param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-direct-smoke",
    [switch]$KeepLogs
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

    $code = $process.ExitCode
    $output = @()
    if ($stdout.Length -ne 0) {
        $output += @($stdout -split "`r?`n" | Where-Object { $_.Length -ne 0 })
    }
    if ($stderr.Length -ne 0) {
        $output += @($stderr -split "`r?`n" | Where-Object { $_.Length -ne 0 })
    }
    if (-not $AllowFailure -and $code -ne 0) {
        throw "tmux $($Arguments -join ' ') failed with exit code ${code}: $($output -join "`n")"
    }

    [pscustomobject]@{
        ExitCode = $code
        Output = $output
    }
}

function Start-TmuxClientProcess {
    param(
        [string]$CaseDir,
        [string[]]$Arguments,
        [switch]$NoDrainOutput
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $CaseDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments

    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $null
    if (-not $NoDrainOutput) {
        $stdout = $process.StandardOutput.ReadToEndAsync()
    }
    [pscustomobject]@{
        Process = $process
        Stdout = $stdout
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

    $stdout = ""
    if ($null -ne $Run.Stdout) {
        $stdout = $Run.Stdout.Result
    }

    [pscustomobject]@{
        ExitCode = $Run.Process.ExitCode
        Stdout = $stdout
        Stderr = $Run.Stderr.Result
    }
}

function Get-CaseLogs {
    param([string]$CaseDir)

    Get-ChildItem -LiteralPath $CaseDir -Filter "tmux-*.log" -File
}

function Assert-AnyLogMatch {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern,
        [string]$Message
    )

    foreach ($log in $Logs) {
        if (Select-String -LiteralPath $log.FullName -Pattern $Pattern -Quiet) {
            return
        }
    }
    throw $Message
}

function Assert-NoLogMatch {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern,
        [string]$Message
    )

    foreach ($log in $Logs) {
        $match = Select-String -LiteralPath $log.FullName -Pattern $Pattern |
            Select-Object -First 1
        if ($null -ne $match) {
            throw "$Message ($($log.Name):$($match.LineNumber): $($match.Line))"
        }
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

function Invoke-DirectCommandSmoke {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "direct-command"
    $config = Join-Path $caseDir "empty.conf"
    $label = "$LabelPrefix-direct-" + [Guid]::NewGuid().ToString("N")
    New-Item -ItemType File -Path $config | Out-Null

    Push-Location $caseDir
    try {
        $env:TMUX = $null
        $env:TMUX_WIN32_HANDLE_TTY = $null
        $env:TMUX_WIN32_CONSOLE_RELAY = "0"
        $env:COLUMNS = "132"
        $env:LINES = "43"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "smoke") | Out-Null
        $list = Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "list-sessions")
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") | Out-Null

        $listText = $list.Output -join "`n"
        Assert-True -Condition ($listText.Contains("smoke:")) `
            -Message "direct command smoke did not list the smoke session"

        $logs = @(Get-CaseLogs $caseDir)
        Assert-AnyLogMatch $logs "using direct Win32 terminal output and input" "direct command smoke did not use direct Win32 terminal I/O"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDOUT duplicated" "direct command smoke did not duplicate stdout"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDIN duplicated" "direct command smoke did not duplicate stdin"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_SIZE 132x43" "direct command smoke did not send the expected size"
        Assert-NoLogMatch $logs "IDENTIFY_WIN32_TERMINAL" "direct command smoke unexpectedly used the relay identify path"
        Assert-NoLogMatch $logs "using Win32 console relay fallback" "direct command smoke unexpectedly enabled relay fallback"
        Assert-NoLogMatch $logs "rejected|ReadFile failed|WriteFile failed" "direct command smoke logged an I/O failure"
    } finally {
        Pop-Location
    }
}

function Invoke-DirectAttachDetachSmoke {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "direct-attach"
    $config = Join-Path $caseDir "empty.conf"
    $label = "$LabelPrefix-attach-" + [Guid]::NewGuid().ToString("N")
    $run = $null
    New-Item -ItemType File -Path $config | Out-Null

    Push-Location $caseDir
    try {
        $env:TMUX = $null
        $env:TMUX_WIN32_HANDLE_TTY = $null
        $env:TMUX_WIN32_CONSOLE_RELAY = "0"
        $env:COLUMNS = "100"
        $env:LINES = "31"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "attach") | Out-Null

        $run = Start-TmuxClientProcess -CaseDir $caseDir -Arguments @(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "attach-session",
            "-t",
            "attach"
        )

        Start-Sleep -Milliseconds 750
        $run.Process.StandardInput.Write([char]2)
        $run.Process.StandardInput.Write("d")
        $run.Process.StandardInput.Flush()

        $result = Wait-TmuxClientProcess $run 5000 "direct attach smoke"
        Assert-True -Condition ($result.ExitCode -eq 0) `
            -Message "direct attach smoke exited with code $($result.ExitCode)"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") | Out-Null

        $logs = @(Get-CaseLogs $caseDir)
        Assert-AnyLogMatch $logs "using direct Win32 terminal output and input" "direct attach smoke did not use direct Win32 terminal I/O"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDOUT duplicated" "direct attach smoke did not duplicate stdout"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDIN duplicated" "direct attach smoke did not duplicate stdin"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_SIZE 100x31" "direct attach smoke did not send the expected size"
        Assert-NoLogMatch $logs "IDENTIFY_WIN32_TERMINAL" "direct attach smoke unexpectedly used the relay identify path"
        Assert-NoLogMatch $logs "using Win32 console relay fallback" "direct attach smoke unexpectedly enabled relay fallback"
        Assert-NoLogMatch $logs "rejected|ReadFile failed|WriteFile failed" "direct attach smoke logged an I/O failure"
    } finally {
        if ($null -ne $run -and -not $run.Process.HasExited) {
            $run.Process.Kill()
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-DirectOutputStressSmoke {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "direct-output-stress"
    $config = Join-Path $caseDir "empty.conf"
    $done = Join-Path $caseDir "stress.done"
    $label = "$LabelPrefix-stress-" + [Guid]::NewGuid().ToString("N")
    $run = $null
    New-Item -ItemType File -Path $config | Out-Null

    Push-Location $caseDir
    try {
        $env:TMUX = $null
        $env:TMUX_WIN32_HANDLE_TTY = $null
        $env:TMUX_WIN32_CONSOLE_RELAY = "0"
        $env:COLUMNS = "120"
        $env:LINES = "40"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "stress", "cmd.exe") | Out-Null

        $run = Start-TmuxClientProcess -CaseDir $caseDir -Arguments @(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "attach-session",
            "-t",
            "stress"
        )

        Start-Sleep -Milliseconds 750
        $command = "for /l %i in (1,1,1200) do @echo TMUX_DIRECT_STRESS_%i_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 & echo done > `"$done`""
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "send-keys", "-t", "stress", "-l", $command) | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "send-keys", "-t", "stress", "Enter") | Out-Null

        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        while (-not (Test-Path -LiteralPath $done)) {
            if ([DateTime]::UtcNow -gt $deadline) {
                throw "direct output stress command did not finish"
            }
            Start-Sleep -Milliseconds 100
        }

        Start-Sleep -Milliseconds 250
        $run.Process.StandardInput.Write([char]2)
        $run.Process.StandardInput.Write("d")
        $run.Process.StandardInput.Flush()

        $result = Wait-TmuxClientProcess $run 10000 "direct output stress"
        Assert-True -Condition ($result.ExitCode -eq 0) `
            -Message "direct output stress exited with code $($result.ExitCode)"
        Assert-True -Condition ($result.Stdout.Length -gt 8192) `
            -Message "direct output stress did not produce enough terminal output"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") | Out-Null

        $logs = @(Get-CaseLogs $caseDir)
        Assert-AnyLogMatch $logs "using direct Win32 terminal output and input" "direct output stress did not use direct Win32 terminal I/O"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDOUT duplicated" "direct output stress did not duplicate stdout"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDIN duplicated" "direct output stress did not duplicate stdin"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_SIZE 120x40" "direct output stress did not send the expected size"
        Assert-NoLogMatch $logs "IDENTIFY_WIN32_TERMINAL" "direct output stress unexpectedly used the relay identify path"
        Assert-NoLogMatch $logs "using Win32 console relay fallback" "direct output stress unexpectedly enabled relay fallback"
        Assert-NoLogMatch $logs "rejected|ReadFile failed|WriteFile failed" "direct output stress logged an I/O failure"
    } finally {
        if ($null -ne $run -and -not $run.Process.HasExited) {
            $run.Process.Kill()
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-DirectInputEofSmoke {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "direct-input-eof"
    $config = Join-Path $caseDir "empty.conf"
    $done = Join-Path $caseDir "eof.done"
    $label = "$LabelPrefix-eof-" + [Guid]::NewGuid().ToString("N")
    $run = $null
    New-Item -ItemType File -Path $config | Out-Null

    Push-Location $caseDir
    try {
        $env:TMUX = $null
        $env:TMUX_WIN32_HANDLE_TTY = $null
        $env:TMUX_WIN32_CONSOLE_RELAY = "0"
        $env:COLUMNS = "110"
        $env:LINES = "35"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "eof", "cmd.exe") | Out-Null

        $run = Start-TmuxClientProcess -CaseDir $caseDir -Arguments @(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "attach-session",
            "-t",
            "eof"
        )

        Start-Sleep -Milliseconds 750
        $run.Process.StandardInput.Close()

        $command = "echo TMUX_DIRECT_EOF_OUTPUT & for /l %i in (1,1,80) do @echo EOF_AFTER_CLOSE_%i & echo done > `"$done`""
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "send-keys", "-t", "eof", "-l", $command) | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "send-keys", "-t", "eof", "Enter") | Out-Null

        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Test-Path -LiteralPath $done)) {
            if ([DateTime]::UtcNow -gt $deadline) {
                throw "direct input EOF output command did not finish"
            }
            Start-Sleep -Milliseconds 100
        }

        Assert-True -Condition (-not $run.Process.HasExited) `
            -Message "direct input EOF closed the attach client before output completed"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "detach-client", "-s", "eof") | Out-Null
        $result = Wait-TmuxClientProcess $run 5000 "direct input EOF smoke"
        Assert-True -Condition ($result.ExitCode -eq 0) `
            -Message "direct input EOF smoke exited with code $($result.ExitCode)"
        Assert-True -Condition ($result.Stdout.Contains("TMUX_DIRECT_EOF_OUTPUT")) `
            -Message "direct input EOF smoke did not preserve output after stdin closed"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") | Out-Null

        $logs = @(Get-CaseLogs $caseDir)
        Assert-AnyLogMatch $logs "using direct Win32 terminal output and input" "direct input EOF smoke did not use direct Win32 terminal I/O"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDOUT duplicated" "direct input EOF smoke did not duplicate stdout"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDIN duplicated" "direct input EOF smoke did not duplicate stdin"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_SIZE 110x35" "direct input EOF smoke did not send the expected size"
        Assert-AnyLogMatch $logs "tty_win32_in_close" "direct input EOF smoke did not close direct input as a half-close"
        Assert-NoLogMatch $logs "IDENTIFY_WIN32_TERMINAL" "direct input EOF smoke unexpectedly used the relay identify path"
        Assert-NoLogMatch $logs "using Win32 console relay fallback" "direct input EOF smoke unexpectedly enabled relay fallback"
        Assert-NoLogMatch $logs "rejected|ReadFile failed|WriteFile failed" "direct input EOF smoke logged an I/O failure"
    } finally {
        if ($null -ne $run -and -not $run.Process.HasExited) {
            $run.Process.Kill()
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-DirectOutputLossSmoke {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "direct-output-loss"
    $config = Join-Path $caseDir "empty.conf"
    $done = Join-Path $caseDir "output-loss.done"
    $label = "$LabelPrefix-output-loss-" + [Guid]::NewGuid().ToString("N")
    $run = $null
    New-Item -ItemType File -Path $config | Out-Null

    Push-Location $caseDir
    try {
        $env:TMUX = $null
        $env:TMUX_WIN32_HANDLE_TTY = $null
        $env:TMUX_WIN32_CONSOLE_RELAY = "0"
        $env:COLUMNS = "120"
        $env:LINES = "35"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "loss", "cmd.exe") | Out-Null

        $run = Start-TmuxClientProcess -CaseDir $caseDir -NoDrainOutput -Arguments @(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "attach-session",
            "-t",
            "loss"
        )

        Start-Sleep -Milliseconds 750
        $run.Process.StandardOutput.Close()

        $command = "for /l %i in (1,1,400) do @echo OUTPUT_HANDLE_CLOSED_%i & echo done > `"$done`""
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "send-keys", "-t", "loss", "-l", $command) | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "send-keys", "-t", "loss", "Enter") | Out-Null

        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Test-Path -LiteralPath $done)) {
            if ([DateTime]::UtcNow -gt $deadline) {
                throw "direct output loss command did not finish"
            }
            Start-Sleep -Milliseconds 100
        }

        $list = Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "list-sessions")
        $listText = $list.Output -join "`n"
        Assert-True -Condition ($listText.Contains("loss:")) `
            -Message "server did not remain usable after direct output loss"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") | Out-Null
        if (-not $run.Process.WaitForExit(5000)) {
            $run.Process.Kill()
            throw "direct output loss client did not exit after server shutdown"
        }

        $logs = @(Get-CaseLogs $caseDir)
        Assert-AnyLogMatch $logs "using direct Win32 terminal output and input" "direct output loss did not use direct Win32 terminal I/O"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDOUT duplicated" "direct output loss did not duplicate stdout"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_STDIN duplicated" "direct output loss did not duplicate stdin"
        Assert-AnyLogMatch $logs "IDENTIFY_WIN32_SIZE 120x35" "direct output loss did not send the expected size"
        Assert-AnyLogMatch $logs "WriteFile failed|output error" "direct output loss did not exercise output failure handling"
        Assert-NoLogMatch $logs "IDENTIFY_WIN32_TERMINAL" "direct output loss unexpectedly used the relay identify path"
        Assert-NoLogMatch $logs "using Win32 console relay fallback" "direct output loss unexpectedly enabled relay fallback"
        Assert-NoLogMatch $logs "rejected|ReadFile failed" "direct output loss logged an unexpected I/O failure"
    } finally {
        if ($null -ne $run -and -not $run.Process.HasExited) {
            $run.Process.Kill()
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

if (-not (Test-Path -LiteralPath $TmuxPath -PathType Leaf)) {
    throw "tmux executable not found: $TmuxPath"
}
$script:TmuxPath = (Resolve-Path -LiteralPath $TmuxPath).Path

$savedEnv = @{
    TMUX = $env:TMUX
    TMUX_WIN32_HANDLE_TTY = $env:TMUX_WIN32_HANDLE_TTY
    TMUX_WIN32_CONSOLE_RELAY = $env:TMUX_WIN32_CONSOLE_RELAY
    COLUMNS = $env:COLUMNS
    LINES = $env:LINES
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-direct-smoke-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root | Out-Null

try {
    Invoke-DirectCommandSmoke $root
    Invoke-DirectAttachDetachSmoke $root
    Invoke-DirectOutputStressSmoke $root
    Invoke-DirectInputEofSmoke $root
    Invoke-DirectOutputLossSmoke $root
    Write-Host "Win32 direct-handle smoke passed. Logs: $root"
    if (-not $KeepLogs) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
} catch {
    Write-Error "$($_.Exception.Message). Logs kept at $root"
    exit 1
} finally {
    $env:TMUX = $savedEnv.TMUX
    $env:TMUX_WIN32_HANDLE_TTY = $savedEnv.TMUX_WIN32_HANDLE_TTY
    $env:TMUX_WIN32_CONSOLE_RELAY = $savedEnv.TMUX_WIN32_CONSOLE_RELAY
    $env:COLUMNS = $savedEnv.COLUMNS
    $env:LINES = $savedEnv.LINES
}
