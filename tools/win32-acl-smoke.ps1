param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-acl-smoke",
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

function Start-TmuxClientProcess {
    param(
        [string]$CaseDir,
        [string[]]$Arguments
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
    [pscustomobject]@{
        Process = $process
        Stdin = $process.StandardInput
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
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

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-acl-smoke-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root | Out-Null
$removeRoot = $false

try {
    $config = Join-Path $root "empty.conf"
    New-Item -ItemType File -Path $config | Out-Null
    $label = "$LabelPrefix-" + [Guid]::NewGuid().ToString("N")

    $env:TMUX = $null
    $env:TMUX_WIN32_HANDLE_TTY = $null
    $env:TMUX_WIN32_CONSOLE_RELAY = "0"
    $env:COLUMNS = "120"
    $env:LINES = "40"

    Push-Location $root
    try {
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments @("-f", $config, "-vv", "-L", $label, "new-session", "-d", "-s", "smoke") | Out-Null

        $serverAccess = Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "server-access", "-l")
        Assert-True -Condition ($serverAccess.Output.Count -eq 1) `
            -Message "server-access -l did not print exactly one ACL entry"
        Assert-True -Condition ($serverAccess.Output[0] -match '^S-1-5-21-[0-9-]+ \((R|W)\)$') `
            -Message "server-access -l did not print a SID principal"

        $client = Start-TmuxClientProcess -CaseDir $root -Arguments @("-f", $config, "-CC", "-L", $label, "attach-session", "-t", "smoke")
        Start-Sleep -Milliseconds 700

        $listClients = Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "list-clients", "-F", "#{client_user}")
        $clientUser = ($listClients.Output | Where-Object { $_.Length -ne 0 } | Select-Object -First 1)
        Assert-True -Condition ($clientUser -ne $null) -Message "list-clients did not report a client user"
        Assert-True -Condition ($clientUser -match '^S-1-5-21-[0-9-]+$') `
            -Message "list-clients did not report a SID user"
        Assert-True -Condition ($clientUser -eq ($serverAccess.Output[0] -replace ' \((R|W)\)$', '')) `
            -Message "client_user and server-access listed different principals"

        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        $client.Stdin.Close()
        if (-not $client.Process.WaitForExit(1000)) {
            $client.Process.Kill()
            $client.Process.WaitForExit()
        }

        Write-Host "Win32 ACL smoke passed. Logs: $root"
        if (-not $KeepLogs) {
            $removeRoot = $true
        }
    } finally {
        Pop-Location
    }
    if ($removeRoot) {
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
