param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-pane-job-lifecycle",
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
        [switch]$AllowFailure
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $WorkingDirectory
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

function New-CaseDirectory {
    param(
        [string]$Root,
        [string]$Name
    )

    $path = Join-Path $Root $Name
    New-Item -ItemType Directory -Path $path | Out-Null
    $path
}

function Get-LatestServerLog {
    param([string]$CaseDir)

    Get-ChildItem -LiteralPath $CaseDir -Filter "tmux-server-*.log" -File |
        Sort-Object LastWriteTime | Select-Object -Last 1
}

function New-Label {
    param([string]$Suffix)

    $token = [guid]::NewGuid().ToString("N").Substring(0, 8)
    "$LabelPrefix-$Suffix-$token"
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

function Invoke-DeadPaneGateCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "dead-pane-gate"
    $label = New-Label "dead"
    $payload = '$i=0; while($i -lt 120){ [Console]::Out.WriteLine((''x''*200)); Start-Sleep -Milliseconds 25; $i++ }'

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "powershell.exe", "-NoProfile", "-Command",
            "Start-Sleep -Seconds 30") | Out-Null
        $pane = (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
            "#{pane_id}")).Output[0]
        Invoke-Tmux -Arguments @("-L", $label, "set-option", "-pt", "0.0",
            "remain-on-exit", "on") | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-k", "-t",
            $pane, "cmd.exe", "/c", "start", "/b", "powershell.exe",
            "-NoProfile", "-Command", $payload) | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 25 -Description "pane death" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
                    "#{pane_dead}")).Output[0] -eq "1"
            }

        $respawn = Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-t",
            $pane, "cmd.exe", "/c", "exit", "0") -AllowFailure
        $respawnText = $respawn.Output -join "`n"
        Assert-True -Condition ($respawn.ExitCode -ne 0 -and
            $respawnText.Contains("still active")) `
            -Message "respawn-pane without -k should fail while output is still draining"

        Invoke-Tmux -Arguments @("-L", $label, "kill-pane", "-t", $pane) | Out-Null
        $hasSession = Invoke-Tmux -Arguments @("-L", $label, "has-session") -AllowFailure
        Assert-True -Condition ($hasSession.ExitCode -ne 0) `
            -Message "kill-pane should remove the only pane even when quiescence is pending"
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-RespawnQuiesceCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "respawn-quiesce"
    $label = New-Label "rq"

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-vv", "-L", $label, "-f", "NUL",
            "new-session", "-d", "powershell.exe", "-NoProfile",
            "-Command", "Start-Sleep -Seconds 30") | Out-Null
        $pane = (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
            "#{pane_id}")).Output[0]
        Invoke-Tmux -Arguments @("-L", $label, "set-option", "-pt", "0.0",
            "remain-on-exit", "on") | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-k", "-t",
            $pane, "cmd.exe", "/c", "exit", "7") | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 50 -Description "dead pane" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
                    "#{pane_dead}")).Output[0] -eq "1"
            }

        Wait-Until -TimeoutMs 5000 -IntervalMs 100 -Description "respawn after quiescence" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-t",
                    $pane, "cmd.exe", "/c", "exit", "0") -AllowFailure).ExitCode -eq 0
            }
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-RespawnDrainCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "respawn-drain"
    $label = New-Label "rd"
    $payload = '$i=0; while($i -lt 120){ [Console]::Out.WriteLine((''x''*200)); Start-Sleep -Milliseconds 25; $i++ }'

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-vv", "-L", $label, "-f", "NUL",
            "new-session", "-d", "powershell.exe", "-NoProfile",
            "-Command", "Start-Sleep -Seconds 30") | Out-Null
        $pane = (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
            "#{pane_id}")).Output[0]
        Invoke-Tmux -Arguments @("-L", $label, "set-option", "-pt", "0.0",
            "remain-on-exit", "on") | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-k", "-t",
            $pane, "cmd.exe", "/c", "start", "/b", "powershell.exe",
            "-NoProfile", "-Command", $payload) | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 25 -Description "dead pane" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
                    "#{pane_dead}")).Output[0] -eq "1"
            }

        $respawn = Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-t",
            $pane, "cmd.exe", "/c", "exit", "0") -AllowFailure
        $respawnText = $respawn.Output -join "`n"
        Assert-True -Condition ($respawn.ExitCode -ne 0 -and
            $respawnText.Contains("still active")) `
            -Message "respawn-pane without -k should fail while output is still draining"

        Wait-Until -TimeoutMs 10000 -IntervalMs 100 -Description "respawn after drain" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-t",
                    $pane, "cmd.exe", "/c", "exit", "0") -AllowFailure).ExitCode -eq 0
            }
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-RemainOnExitCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "remain-on-exit"
    $label = New-Label "remain"

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "powershell.exe", "-NoProfile", "-Command",
            "Start-Sleep -Seconds 1; exit 7") | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "set-option", "-pt", "0.0",
            "remain-on-exit", "on") | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 50 -Description "dead pane" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
                    "#{pane_dead}")).Output[0] -eq "1"
            }

        $state = Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
            "#{pane_dead} #{pane_dead_status}")
        Assert-True -Condition (($state.Output -join "`n").Contains("1 7")) `
            -Message "remain-on-exit should keep a dead pane visible with status"
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-RespawnKillCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "respawn-kill"
    $label = New-Label "rpk"

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "powershell.exe", "-NoProfile", "-Command",
            "Start-Sleep -Seconds 30") | Out-Null
        $pane = (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
            "#{pane_id}")).Output[0]

        Invoke-Tmux -Arguments @("-L", $label, "respawn-pane", "-k", "-t",
            $pane, "cmd.exe", "/c", "exit", "0") | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 50 -Description "respawned pane death" `
            -Condition {
                (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
                    "#{pane_dead}")).Output[0] -eq "1"
            }
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-PipeCloseCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "pipe-close"
    $label = New-Label "pipe"
    $marker = Join-Path $caseDir "pipe-close.txt"

    Push-Location $caseDir
    try {
        $helper = 'powershell.exe -NoProfile -Command "$null = $input | Out-Null; Set-Content -Path ''' + $marker + ''' -Value done"'

        Invoke-Tmux -Arguments @("-L", $label, "-f", "NUL", "new-session",
            "-d", "powershell.exe", "-NoProfile", "-Command",
            "Start-Sleep -Seconds 5") | Out-Null
        $pane = (Invoke-Tmux -Arguments @("-L", $label, "list-panes", "-F",
            "#{pane_id}")).Output[0]

        Invoke-Tmux -Arguments @("-L", $label, "pipe-pane", "-t", $pane,
            "-O", $helper) | Out-Null
        Start-Sleep -Milliseconds 300
        Invoke-Tmux -Arguments @("-L", $label, "pipe-pane", "-t", $pane) | Out-Null

        Wait-Until -TimeoutMs 5000 -IntervalMs 100 -Description "pipe helper EOF" `
            -Condition { Test-Path -LiteralPath $marker }
    } finally {
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Pop-Location
    }
}

function Invoke-NaturalJobCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "natural-job"
    $label = New-Label "jfree"

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-vv", "-L", $label, "-f", "NUL",
            "new-session", "-d") | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "run-shell", "-b",
            "cmd.exe /c exit 0") | Out-Null
        Start-Sleep -Seconds 1
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null

        $log = Get-LatestServerLog $caseDir
        Assert-True -Condition ($null -ne $log) `
            -Message "natural job case did not create a server log"
        $text = Get-Content -LiteralPath $log.FullName -Raw
        Assert-True -Condition ($text.Contains("free job")) `
            -Message "natural job completion did not log passive free"
        Assert-True -Condition (-not $text.Contains("kill job")) `
            -Message "natural job completion should not log explicit kill"
    } finally {
        Pop-Location
    }
}

function Invoke-KillAllJobCase {
    param([string]$Root)

    $caseDir = New-CaseDirectory $Root "killall-job"
    $label = New-Label "jkill"
    $marker = Join-Path $caseDir "done.txt"
    $scriptPath = Join-Path $caseDir "job.ps1"
    $scriptForCmd = $scriptPath -replace '\\', '/'

    Set-Content -Path $scriptPath -Value "Start-Sleep -Seconds 3`nSet-Content -Path '$marker' -Value done`n"

    Push-Location $caseDir
    try {
        Invoke-Tmux -Arguments @("-vv", "-L", $label, "-f", "NUL",
            "new-session", "-d") | Out-Null
        Invoke-Tmux -Arguments @("-L", $label, "run-shell", "-b",
            "powershell.exe -NoProfile -File '$scriptForCmd'") | Out-Null
        Start-Sleep -Milliseconds 500
        Invoke-Tmux -Arguments @("-L", $label, "kill-server") -AllowFailure | Out-Null
        Start-Sleep -Seconds 4
        Assert-True -Condition (-not (Test-Path -LiteralPath $marker)) `
            -Message "job_kill_all should cancel a background job before it reaches its marker"
    } finally {
        Pop-Location
    }
}

$script:TmuxPath = [System.IO.Path]::GetFullPath($TmuxPath)
$root = Join-Path $env:TEMP ($LabelPrefix + "-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root | Out-Null

try {
    Invoke-RespawnQuiesceCase -Root $root
    Invoke-RespawnDrainCase -Root $root
    Invoke-DeadPaneGateCase -Root $root
    Invoke-RemainOnExitCase -Root $root
    Invoke-RespawnKillCase -Root $root
    Invoke-PipeCloseCase -Root $root
    Invoke-NaturalJobCase -Root $root
    Invoke-KillAllJobCase -Root $root
    "Win32 pane/job lifecycle smoke passed"
} finally {
    if ($KeepArtifacts) {
        "Artifacts kept at $root"
    } else {
        Remove-Item -Recurse -Force $root
    }
}
