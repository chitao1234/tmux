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

function Wait-ForPath {
    param(
        [string]$Path,
        [int]$TimeoutSeconds = 5,
        [string]$Description = $Path
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not (Test-Path -LiteralPath $Path)) {
        if ([DateTime]::UtcNow -gt $deadline) {
            throw "timed out waiting for $Description"
        }
        Start-Sleep -Milliseconds 100
    }
}

function Normalize-ComparablePath {
    param([string]$Path)

    if ($null -eq $Path) {
        return $null
    }
    return $Path.TrimEnd('\', '/')
}

$script:TmuxPath = [System.IO.Path]::GetFullPath($TmuxPath)
$root = Join-Path $env:TEMP ("tmux-u8-" + [guid]::NewGuid().ToString("N"))
$caseDir = Join-Path $root "工作目录"
$childCwdDir = Join-Path $caseDir "子进程目录"
$childOutputPath = Join-Path $childCwdDir "子输出.txt"
$bufferPath = Join-Path $caseDir "缓冲区-文件.txt"
$configPath = Join-Path $root "配置.tmux.conf"
$label = $LabelPrefix + "-" + [guid]::NewGuid().ToString("N")
$unicodeLabel = "标签-" + [guid]::NewGuid().ToString("N")
$tmuxValuePath = Join-Path $caseDir "tmux-value.txt"
$bufferName = "unicode-file"
$loadedBufferName = "unicode-load"
$bufferContent = "文件_内容_边界"

New-Item -ItemType Directory -Path $caseDir -Force | Out-Null
New-Item -ItemType Directory -Path $childCwdDir -Force | Out-Null
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

    Invoke-Tmux -Arguments @("-L", $label, "set-buffer", "-b", $bufferName, $bufferContent) -WorkingDirectory $caseDir -Environment $envMap | Out-Null
    Invoke-Tmux -Arguments @("-L", $label, "save-buffer", "-b", $bufferName, $bufferPath) -WorkingDirectory $caseDir -Environment $envMap | Out-Null
    $savedBufferText = [System.IO.File]::ReadAllText(
        $bufferPath,
        [System.Text.UTF8Encoding]::new($false)
    )
    Assert-True ($savedBufferText -eq $bufferContent) "Unicode file write path did not preserve buffer content"

    Invoke-Tmux -Arguments @("-L", $label, "load-buffer", "-b", $loadedBufferName, $bufferPath) -WorkingDirectory $caseDir -Environment $envMap | Out-Null
    $showLoadedBuffer = Invoke-Tmux -Arguments @("-L", $label, "show-buffer", "-b", $loadedBufferName) -WorkingDirectory $caseDir -Environment $envMap
    $loadedBufferText = $showLoadedBuffer.Output -join "`n"
    Assert-True ($loadedBufferText -eq $bufferContent) "Unicode file read path did not round-trip buffer content"

    Invoke-Tmux -Arguments @(
        "-L",
        $label,
        "new-session",
        "-d",
        "-s",
        "cwdcheck",
        "-c",
        $childCwdDir,
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-Command",
        '[System.IO.File]::WriteAllText(".\子输出.txt", (Get-Location).Path, [System.Text.UTF8Encoding]::new($false))'
    ) -WorkingDirectory $root -Environment $envMap | Out-Null

    $showPaneCwd = Invoke-Tmux -Arguments @("-L", $label, "display-message", "-p", "-t", "cwdcheck", "#{pane_current_path}") -WorkingDirectory $caseDir -Environment $envMap
    $paneCwdText = $showPaneCwd.Output | Select-Object -First 1
    Assert-True (
        (Normalize-ComparablePath $paneCwdText) -eq (Normalize-ComparablePath $childCwdDir)
    ) ("Pane current path did not preserve the Unicode cwd: " + $paneCwdText)

    Wait-ForPath -Path $childOutputPath -Description "Unicode child cwd output"
    $childCwdText = [System.IO.File]::ReadAllText(
        $childOutputPath,
        [System.Text.UTF8Encoding]::new($false)
    ).Trim()
    Assert-True (
        (Normalize-ComparablePath $childCwdText) -eq (Normalize-ComparablePath $childCwdDir)
    ) ("Child process startup from Unicode cwd did not preserve the working directory: " + $childCwdText)

    $captureEnvMap = @{}
    foreach ($entry in $envMap.GetEnumerator()) {
        $captureEnvMap[$entry.Key] = $entry.Value
    }
    $captureEnvMap["TMUX_CAPTURE"] = $tmuxValuePath

    Invoke-Tmux -Arguments @(
        "-L",
        $unicodeLabel,
        "new-session",
        "-d",
        "-s",
        "unicode",
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-Command",
        '[System.IO.File]::WriteAllText($env:TMUX_CAPTURE, $env:TMUX, [System.Text.UTF8Encoding]::new($false))'
    ) -WorkingDirectory $caseDir -Environment $captureEnvMap | Out-Null

    Wait-ForPath -Path $tmuxValuePath -Description "child TMUX capture from unicode-labeled server"

    $showSocket = Invoke-Tmux -Arguments @("-L", $unicodeLabel, "display-message", "-p", "#{socket_path}") -WorkingDirectory $caseDir -Environment $envMap
    $socketPath = $showSocket.Output | Select-Object -First 1
    Assert-True ($socketPath -ne $null -and $socketPath.Length -ne 0) "could not read socket path from unicode-labeled server"

    $tmuxValue = [System.IO.File]::ReadAllText(
        $tmuxValuePath,
        [System.Text.UTF8Encoding]::new($false)
    ).Trim()
    Assert-True ($tmuxValue.Length -ne 0) "captured child TMUX was empty"
    Assert-True ($tmuxValue.StartsWith($socketPath + ",")) "captured child TMUX did not preserve the unicode socket path"

    $tmuxEnvMap = @{}
    foreach ($entry in $envMap.GetEnumerator()) {
        $tmuxEnvMap[$entry.Key] = $entry.Value
    }
    $tmuxEnvMap["TMUX"] = $tmuxValue

    $showTmux = Invoke-Tmux -Arguments @("show-environment", "-g", "HOME") -WorkingDirectory $caseDir -Environment $tmuxEnvMap
    Assert-True ($showTmux.Output -contains "HOME=$caseDir") "Unicode TMUX socket path did not resolve the server"

    Write-Output "win32 utf8 boundary smoke passed"
}
finally {
    Invoke-Tmux -Arguments @("-L", $label, "kill-server") -WorkingDirectory $caseDir -Environment $envMap -AllowFailure | Out-Null
    Invoke-Tmux -Arguments @("-L", $unicodeLabel, "kill-server") -WorkingDirectory $caseDir -Environment $envMap -AllowFailure | Out-Null
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Output "kept artifacts at $root"
    }
}
