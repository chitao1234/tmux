param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-ipc-startup",
    [switch]$KeepArtifacts,
    [switch]$SkipForeground
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
        [switch]$AllowFailure
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments

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

function Start-TmuxProcess {
    param(
        [string]$Name,
        [string[]]$Arguments,
        [string]$WorkingDirectory = (Get-Location).Path
    )

    $stdout = Join-Path $script:ArtifactRoot ($Name + ".out.txt")
    $stderr = Join-Path $script:ArtifactRoot ($Name + ".err.txt")
    $outStream = [System.IO.File]::Open(
        $stdout, [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    $errStream = [System.IO.File]::Open(
        $stderr, [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments
    $null = $psi.Environment.Remove("TMUX")

    $process = [System.Diagnostics.Process]::Start($psi)
    $outTask = $process.StandardOutput.BaseStream.CopyToAsync($outStream)
    $errTask = $process.StandardError.BaseStream.CopyToAsync($errStream)

    [pscustomobject]@{
        Process = $process
        Stdout = $stdout
        Stderr = $stderr
        StdoutStream = $outStream
        StderrStream = $errStream
        StdoutTask = $outTask
        StderrTask = $errTask
        Name = $Name
    }
}

function Wait-TmuxProcess {
    param(
        $Run,
        [int]$TimeoutMs = 15000
    )

    if (-not $Run.Process.WaitForExit($TimeoutMs)) {
        try {
            $Run.Process.Kill()
        } catch {
        }
        [System.Threading.Tasks.Task]::WaitAll(@($Run.StdoutTask,
            $Run.StderrTask), 1000) | Out-Null
        $Run.StdoutStream.Dispose()
        $Run.StderrStream.Dispose()
        throw "process $($Run.Name) did not exit within ${TimeoutMs}ms"
    }
    [System.Threading.Tasks.Task]::WaitAll(@($Run.StdoutTask,
        $Run.StderrTask)) | Out-Null
    $Run.StdoutStream.Dispose()
    $Run.StderrStream.Dispose()

    $stdout = ""
    $stderr = ""
    if (Test-Path -LiteralPath $Run.Stdout) {
        $stdout = Get-Content -Raw -LiteralPath $Run.Stdout
    }
    if (Test-Path -LiteralPath $Run.Stderr) {
        $stderr = Get-Content -Raw -LiteralPath $Run.Stderr
    }

    if ($Run.Process.ExitCode -ne 0) {
        throw "process $($Run.Name) failed with exit code $($Run.Process.ExitCode): stdout=$stdout stderr=$stderr"
    }

    [pscustomobject]@{
        Stdout = $stdout
        Stderr = $stderr
    }
}

function Wait-TmuxConnect {
    param(
        [string[]]$Arguments,
        [string]$WorkingDirectory = (Get-Location).Path,
        [int]$TimeoutMs = 15000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $result = Invoke-Tmux -Arguments $Arguments -WorkingDirectory $WorkingDirectory -AllowFailure
        if ($result.ExitCode -eq 0) {
            return $result
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "timed out waiting for tmux command: $($Arguments -join ' ')"
}

function Normalize-ComparablePath {
    param([string]$Path)

    if ($null -eq $Path) {
        return $null
    }
    return ($Path -replace '\\', '/').TrimEnd('/')
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

function Test-InteractiveConsole {
    return (-not [Console]::IsInputRedirected) -and
        (-not [Console]::IsOutputRedirected) -and
        (-not [Console]::IsErrorRedirected)
}

function Remove-ArtifactRoot {
    param([string]$Path)

    for ($i = 0; $i -lt 60; $i++) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        } catch {
            if (-not (Test-Path -LiteralPath $Path)) {
                return
            }
            Start-Sleep -Milliseconds 200
        }
    }

    Write-Warning "could not remove smoke-test artifacts: $Path"
}

function Start-PowerShellHelper {
    param(
        [string]$Name,
        [string[]]$Arguments
    )

    $stdout = Join-Path $script:ArtifactRoot ($Name + ".helper.out.txt")
    $stderr = Join-Path $script:ArtifactRoot ($Name + ".helper.err.txt")
    $process = Start-Process -FilePath $script:HostShellPath `
        -ArgumentList $Arguments `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru

    [pscustomobject]@{
        Process = $process
        Stdout = $stdout
        Stderr = $stderr
        Name = $Name
    }
}

function Wait-PowerShellHelper {
    param(
        $Run,
        [int]$TimeoutMs = 30000
    )

    if (-not $Run.Process.WaitForExit($TimeoutMs)) {
        try {
            $Run.Process.Kill()
        } catch {
        }
        throw "helper $($Run.Name) did not exit within ${TimeoutMs}ms"
    }

    $stdout = ""
    $stderr = ""
    if (Test-Path -LiteralPath $Run.Stdout) {
        $stdout = Get-Content -Raw -LiteralPath $Run.Stdout
    }
    if (Test-Path -LiteralPath $Run.Stderr) {
        $stderr = Get-Content -Raw -LiteralPath $Run.Stderr
    }

    if ($Run.Process.ExitCode -ne 0) {
        throw "helper $($Run.Name) failed with exit code $($Run.Process.ExitCode): stdout=$stdout stderr=$stderr"
    }
}

function New-HelperScript {
    $scriptPath = Join-Path $script:ArtifactRoot "foreground-helper.ps1"
    $content = @'
param(
    [string]$Mode,
    [string]$TmuxPath,
    [string]$Target,
    [int]$ClientCount = 4
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
    $psi.FileName = $TmuxPath
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

function Wait-TmuxConnect {
    param(
        [string[]]$Arguments,
        [int]$TimeoutMs = 15000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $result = Invoke-Tmux -Arguments $Arguments -AllowFailure
        if ($result.ExitCode -eq 0) {
            return $result
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "timed out waiting for tmux command: $($Arguments -join ' ')"
}

switch ($Mode) {
    "stale-foreground" {
        Wait-TmuxConnect -Arguments @("-S", $Target, "list-commands") | Out-Null
        Invoke-Tmux -Arguments @("-S", $Target, "kill-server") | Out-Null
        break
    }
    "foreground-race" {
        $runs = @()
        for ($i = 0; $i -lt $ClientCount; $i++) {
            $psi = [System.Diagnostics.ProcessStartInfo]::new()
            $psi.FileName = $TmuxPath
            $psi.WorkingDirectory = (Get-Location).Path
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $psi.Arguments = Join-Win32Arguments @("-L", $Target, "list-commands")
            $process = [System.Diagnostics.Process]::Start($psi)
            $runs += [pscustomobject]@{
                Process = $process
                Stdout = $process.StandardOutput.ReadToEndAsync()
                Stderr = $process.StandardError.ReadToEndAsync()
            }
        }
        foreach ($run in $runs) {
            if (-not $run.Process.WaitForExit(15000)) {
                try {
                    $run.Process.Kill()
                } catch {
                }
                throw "foreground client did not exit"
            }
            if ($run.Process.ExitCode -ne 0) {
                throw "foreground client failed: stdout=$($run.Stdout.Result) stderr=$($run.Stderr.Result)"
            }
        }
        Invoke-Tmux -Arguments @("-L", $Target, "kill-server") | Out-Null
        break
    }
    default {
        throw "unknown helper mode: $Mode"
    }
}
'@
    [System.IO.File]::WriteAllText($scriptPath, $content, [System.Text.UTF8Encoding]::new($false))
    return $scriptPath
}

function Test-DefaultLabelStartup {
    $label = "{0}-default-{1}" -f $LabelPrefix, [guid]::NewGuid().ToString("N").Substring(0, 8)
    Invoke-Tmux -Arguments @("-L", $label, "start-server") | Out-Null
    $socketResult = Invoke-Tmux -Arguments @("-L", $label, "display-message", "-p", "#{socket_path}")
    $socketPath = ($socketResult.Output | Select-Object -Last 1).Trim()

    Assert-True (-not [string]::IsNullOrWhiteSpace($socketPath)) "default label socket path was empty"
    Assert-True (-not (Test-Path -LiteralPath ($socketPath + ".lock"))) "default label left a .lock file after startup: $socketPath.lock"

    Invoke-Tmux -Arguments @("-L", $label, "list-commands") | Out-Null
    Invoke-Tmux -Arguments @("-L", $label, "kill-server") | Out-Null

    Assert-True (-not (Test-Path -LiteralPath ($socketPath + ".lock"))) "default label left a .lock file after shutdown: $socketPath.lock"
    return "default-label startup and lock cleanup"
}

function Test-ExplicitSocketAliasing {
    $aliasPath = Join-Path $script:ArtifactRoot "Alias.sock"
    $relative = ".\Alias.sock"
    $mixed = ([System.IO.Path]::GetFullPath($aliasPath)).ToUpperInvariant().Replace('\', '/')

    Invoke-Tmux -Arguments @("-S", $relative, "new-session", "-d", "-s", "aliascheck") -WorkingDirectory $script:ArtifactRoot | Out-Null
    $socketResult = Invoke-Tmux -Arguments @("-S", $mixed, "display-message", "-p", "#{socket_path}")
    $resolved = ($socketResult.Output | Select-Object -Last 1).Trim()
    $sessions = Invoke-Tmux -Arguments @("-S", $mixed, "list-sessions")

    Assert-True ((($sessions.Output -join "`n") -match "aliascheck")) "case-folded -S path did not reach the existing server"
    Assert-True (Test-ComparablePathEquals $resolved $aliasPath) "resolved socket path '$resolved' did not match expected '$aliasPath'"

    Invoke-Tmux -Arguments @("-S", $aliasPath, "kill-server") | Out-Null
    return "explicit -S aliasing and case folding"
}

function Test-ExplicitMissingParentStartup {
    $socketPath = Join-Path $script:ArtifactRoot "Nest\Sock"

    Invoke-Tmux -Arguments @("-S", $socketPath, "start-server") | Out-Null
    $socketResult = Invoke-Tmux -Arguments @("-S", $socketPath, "display-message", "-p", "#{socket_path}")
    $resolved = ($socketResult.Output | Select-Object -Last 1).Trim()

    Assert-True (Test-ComparablePathEquals $resolved $socketPath) "resolved socket path '$resolved' did not match expected '$socketPath'"
    Assert-True (Test-Path -LiteralPath $socketPath) "explicit missing-parent startup did not create socket path: $socketPath"

    Invoke-Tmux -Arguments @("-S", $socketPath, "kill-server") | Out-Null
    Assert-True (-not (Test-Path -LiteralPath ($socketPath + ".lock"))) "explicit missing-parent startup left a .lock file: $socketPath.lock"
    return "explicit -S startup with missing parent"
}

function Test-ExplicitLocalAppDataStartup {
    $root = Join-Path $env:LOCALAPPDATA ("tmux-explicit-" + [guid]::NewGuid().ToString("N"))
    $socketPath = Join-Path $root "default"

    try {
        Invoke-Tmux -Arguments @("-S", $socketPath, "start-server") | Out-Null
        $socketResult = Invoke-Tmux -Arguments @("-S", $socketPath, "display-message", "-p", "#{socket_path}")
        $resolved = ($socketResult.Output | Select-Object -Last 1).Trim()

        Assert-True (Test-ComparablePathEquals $resolved $socketPath) "resolved socket path '$resolved' did not match expected '$socketPath'"
        Assert-True (Test-Path -LiteralPath $socketPath) "explicit LOCALAPPDATA startup did not create socket path: $socketPath"

        Invoke-Tmux -Arguments @("-S", $socketPath, "kill-server") | Out-Null
        Assert-True (-not (Test-Path -LiteralPath ($socketPath + ".lock"))) "explicit LOCALAPPDATA startup left a .lock file: $socketPath.lock"
    } finally {
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
    return "explicit -S startup under LOCALAPPDATA"
}

function Test-InheritedSocketStartup {
    $socketPath = Join-Path $script:ArtifactRoot "Env\Sock"
    $tmuxValue = $socketPath + ",123,0"

    Invoke-Tmux -Arguments @("new-session", "-d", "-s", "envcheck") -Environment @{ TMUX = $tmuxValue } | Out-Null
    $sessions = Invoke-Tmux -Arguments @("-S", $socketPath, "list-sessions")

    Assert-True ((($sessions.Output -join "`n") -match "envcheck")) "inherited TMUX path did not reach the expected server"

    Invoke-Tmux -Arguments @("-S", $socketPath, "kill-server") | Out-Null
    return "inherited TMUX startup on explicit socket path"
}

function Test-UnsafeExplicitSocketRejected {
    $socketPath = "C:\tmux-unsafe-" + [guid]::NewGuid().ToString("N") + "\Sock"
    $result = Invoke-Tmux -Arguments @("-S", $socketPath, "start-server") -AllowFailure
    $output = $result.Output -join "`n"

    Assert-True ($result.ExitCode -ne 0) "unsafe explicit socket path unexpectedly succeeded: $socketPath"
    Assert-True ($output -match "trusted user|user-owned|reparse|existing directory|not a directory") "unsafe explicit socket path failed without a trust-policy error: $output"
    Assert-True (-not (Test-Path -LiteralPath (Split-Path -Parent $socketPath))) "unsafe explicit socket path unexpectedly created parent directories: $socketPath"
    return "unsafe explicit -S startup rejection"
}

function Test-StaleDetachedStartup {
    $socketPath = Join-Path $script:ArtifactRoot "stale-detached.sock"
    Set-Content -LiteralPath $socketPath -Value "stale" -NoNewline

    Invoke-Tmux -Arguments @("-S", $socketPath, "list-commands") | Out-Null
    Invoke-Tmux -Arguments @("-S", $socketPath, "kill-server") | Out-Null

    Assert-True (-not (Test-Path -LiteralPath ($socketPath + ".lock"))) "stale detached startup left a .lock file: $socketPath.lock"
    return "stale detached autostart"
}

function Test-ConcurrentAutostart {
    $label = "{0}-race-{1}" -f $LabelPrefix, [guid]::NewGuid().ToString("N").Substring(0, 8)
    $runs = @()

    for ($i = 0; $i -lt 8; $i++) {
        $runs += Start-TmuxProcess -Name ("autostart-" + $i) -Arguments @("-L", $label, "list-commands")
    }
    foreach ($run in $runs) {
        Wait-TmuxProcess -Run $run | Out-Null
    }

    Invoke-Tmux -Arguments @("-L", $label, "kill-server") | Out-Null
    return "concurrent autostart"
}

function Test-ConcurrentStartServer {
    $label = "{0}-start-{1}" -f $LabelPrefix, [guid]::NewGuid().ToString("N").Substring(0, 8)
    $runs = @()

    for ($i = 0; $i -lt 8; $i++) {
        $runs += Start-TmuxProcess -Name ("startserver-" + $i) -Arguments @("-L", $label, "start-server")
    }
    foreach ($run in $runs) {
        Wait-TmuxProcess -Run $run | Out-Null
    }

    Invoke-Tmux -Arguments @("-L", $label, "kill-server") | Out-Null
    return "concurrent start-server"
}

function Test-StaleForegroundStartup {
    $socketPath = Join-Path $script:ArtifactRoot "stale-foreground.sock"
    Set-Content -LiteralPath $socketPath -Value "stale" -NoNewline

    $helper = Start-PowerShellHelper -Name "stale-foreground-helper" -Arguments @(
        "-NoProfile",
        "-File",
        $script:HelperScriptPath,
        "-Mode",
        "stale-foreground",
        "-TmuxPath",
        $script:TmuxPath,
        "-Target",
        $socketPath
    )

    & $script:TmuxPath -D -S $socketPath
    if ($LASTEXITCODE -ne 0) {
        throw "foreground stale startup failed with exit code $LASTEXITCODE"
    }

    Wait-PowerShellHelper -Run $helper
    Assert-True (-not (Test-Path -LiteralPath ($socketPath + ".lock"))) "stale foreground startup left a .lock file: $socketPath.lock"
    return "stale foreground -D startup"
}

function Test-ForegroundRace {
    $label = "{0}-fg-{1}" -f $LabelPrefix, [guid]::NewGuid().ToString("N").Substring(0, 8)
    $helper = Start-PowerShellHelper -Name "foreground-race-helper" -Arguments @(
        "-NoProfile",
        "-File",
        $script:HelperScriptPath,
        "-Mode",
        "foreground-race",
        "-TmuxPath",
        $script:TmuxPath,
        "-Target",
        $label,
        "-ClientCount",
        "4"
    )

    & $script:TmuxPath -D -L $label
    if ($LASTEXITCODE -ne 0) {
        throw "foreground race startup failed with exit code $LASTEXITCODE"
    }

    Wait-PowerShellHelper -Run $helper
    return "-D versus ordinary clients"
}

$script:TmuxPath = [System.IO.Path]::GetFullPath($TmuxPath)
$script:HostShellPath = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
$script:ArtifactRoot = Join-Path $env:TEMP ("tmux-ipc-startup-" + [guid]::NewGuid().ToString("N"))
$script:HelperScriptPath = $null

if (-not (Test-Path -LiteralPath $script:TmuxPath)) {
    throw "tmux executable not found at $script:TmuxPath"
}

New-Item -ItemType Directory -Path $script:ArtifactRoot | Out-Null
$script:HelperScriptPath = New-HelperScript

$results = New-Object System.Collections.Generic.List[string]
$skipped = New-Object System.Collections.Generic.List[string]

try {
    $results.Add((Test-DefaultLabelStartup))
    $results.Add((Test-ExplicitSocketAliasing))
    $results.Add((Test-ExplicitMissingParentStartup))
    $results.Add((Test-ExplicitLocalAppDataStartup))
    $results.Add((Test-InheritedSocketStartup))
    $results.Add((Test-UnsafeExplicitSocketRejected))
    $results.Add((Test-StaleDetachedStartup))
    $results.Add((Test-ConcurrentAutostart))
    $results.Add((Test-ConcurrentStartServer))

    if ($SkipForeground) {
        $skipped.Add("foreground -D coverage skipped by request")
    } elseif (-not (Test-InteractiveConsole)) {
        $skipped.Add("foreground -D coverage skipped because this run does not have a real terminal")
    } else {
        $results.Add((Test-StaleForegroundStartup))
        $results.Add((Test-ForegroundRace))
    }

    $summary = "PASS: " + ($results -join "; ")
    if ($skipped.Count -ne 0) {
        $summary += " | SKIP: " + ($skipped -join "; ")
    }
    Write-Output $summary
} finally {
    if (-not $KeepArtifacts -and (Test-Path -LiteralPath $script:ArtifactRoot)) {
        Remove-ArtifactRoot -Path $script:ArtifactRoot
    } elseif ($KeepArtifacts) {
        Write-Output ("artifacts: " + $script:ArtifactRoot)
    }
}
