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
    $process = $null
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

        $psi = [System.Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $script:TmuxPath
        $psi.WorkingDirectory = $caseDir
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.Arguments = Join-Win32Arguments @(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "attach-session",
            "-t",
            "attach"
        )

        $process = [System.Diagnostics.Process]::Start($psi)
        Start-Sleep -Milliseconds 750
        $process.StandardInput.Write([char]2)
        $process.StandardInput.Write("d")
        $process.StandardInput.Flush()

        if (-not $process.WaitForExit(5000)) {
            $process.Kill()
            throw "direct attach smoke did not exit after C-b d"
        }
        Assert-True -Condition ($process.ExitCode -eq 0) `
            -Message "direct attach smoke exited with code $($process.ExitCode)"

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
        if ($null -ne $process -and -not $process.HasExited) {
            $process.Kill()
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
