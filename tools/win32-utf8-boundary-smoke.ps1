param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-utf8-boundary",
    [switch]$KeepArtifacts
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
    $stdoutReader = [System.IO.StreamReader]::new(
        $process.StandardOutput.BaseStream,
        [System.Text.UTF8Encoding]::new($false),
        $true)
    $stderrReader = [System.IO.StreamReader]::new(
        $process.StandardError.BaseStream,
        [System.Text.UTF8Encoding]::new($false),
        $true)
    $stdout = $stdoutReader.ReadToEnd()
    $stderr = $stderrReader.ReadToEnd()
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

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-Utf8Bytes {
    param([string]$Text)

    [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
}

function Test-ByteSequence {
    param(
        [byte[]]$Haystack,
        [byte[]]$Needle
    )

    if ($Needle.Length -eq 0) {
        return $true
    }
    for ($i = 0; $i -le $Haystack.Length - $Needle.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) {
                $match = $false
                break
            }
        }
        if ($match) {
            return $true
        }
    }
    return $false
}

$script:TmuxPath = [System.IO.Path]::GetFullPath($TmuxPath)
$root = Join-Path $env:TEMP ($LabelPrefix + "-" + [guid]::NewGuid().ToString("N"))
$caseDir = Join-Path $root "工作目录"
$configPath = Join-Path $root "配置.tmux.conf"
$label = $LabelPrefix + "-" + [guid]::NewGuid().ToString("N")

New-Item -ItemType Directory -Path $caseDir -Force | Out-Null
[System.IO.File]::WriteAllText(
    $configPath,
    "set -g status-left '边界_OK'`n",
    [System.Text.UTF8Encoding]::new($false))

$envMap = @{
    HOME = $caseDir
    USERPROFILE = $caseDir
    TERM = "xterm-256color"
    VISUAL = "编辑器.exe"
}

try {
    Invoke-Tmux -Arguments @("-L", $label, "new-session", "-d") -WorkingDirectory $caseDir -Environment $envMap | Out-Null
    Invoke-Tmux -Arguments @("-L", $label, "source-file", $configPath) -WorkingDirectory $caseDir -Environment $envMap | Out-Null

    $showHome = Invoke-Tmux -Arguments @("-L", $label, "show-environment", "-g", "HOME") -WorkingDirectory $caseDir -Environment $envMap
    Assert-True ($showHome.Output -contains "HOME=$caseDir") "HOME did not round-trip through global environment"

    $showTerm = Invoke-Tmux -Arguments @("-L", $label, "show-environment", "-g", "TERM") -WorkingDirectory $caseDir -Environment $envMap
    Assert-True ($showTerm.Output -contains "TERM=xterm-256color") "TERM did not round-trip through global environment"

    $showEditor = Invoke-Tmux -Arguments @("-L", $label, "show-options", "-g", "editor") -WorkingDirectory $caseDir -Environment $envMap
    $showEditorText = $showEditor.Output -join "`n"
    $editorNeedle = Get-Utf8Bytes "editor 编辑器.exe"
    $editorBytes = Get-Utf8Bytes $showEditorText
    Assert-True (Test-ByteSequence -Haystack $editorBytes -Needle $editorNeedle) ("VISUAL did not round-trip through editor option: " + $showEditorText)

    $showStatus = Invoke-Tmux -Arguments @("-L", $label, "show-options", "-g", "status-left") -WorkingDirectory $caseDir -Environment $envMap
    $showStatusText = $showStatus.Output -join "`n"
    Assert-True ($showStatusText -match "status-left.*边界_OK") ("UTF-8 config path did not apply: " + $showStatusText)

    Write-Output "win32 utf8 boundary smoke passed"
}
finally {
    Invoke-Tmux -Arguments @("-L", $label, "kill-server") -WorkingDirectory $caseDir -Environment $envMap -AllowFailure | Out-Null
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Output "kept artifacts at $root"
    }
}
