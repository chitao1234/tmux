param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-glob",
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
        [hashtable]$Environment = @{},
        [string[]]$RemoveEnvironment = @(),
        [switch]$AllowFailure
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = Join-Win32Arguments $Arguments

    foreach ($name in $RemoveEnvironment) {
        $null = $psi.Environment.Remove($name)
    }
    foreach ($entry in $Environment.GetEnumerator()) {
        $psi.Environment[$entry.Key] = $entry.Value
    }

    $process = [System.Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    $output = @()
    if ($stdout.Length -ne 0) {
        $output += @($stdout -split "`r?`n" |
            Where-Object { $_.Length -ne 0 })
    }
    if ($stderr.Length -ne 0) {
        $output += @($stderr -split "`r?`n" |
            Where-Object { $_.Length -ne 0 })
    }

    if (-not $AllowFailure -and $process.ExitCode -ne 0) {
        throw ("tmux $($Arguments -join ' ') failed with exit code " +
            "$($process.ExitCode): $($output -join "`n")")
    }

    [pscustomobject]@{
        ExitCode = $process.ExitCode
        Output = $output
    }
}

function New-ConfigFile {
    param(
        [string]$Path,
        [string]$Option
    )

    Set-Content -LiteralPath $Path -Encoding ascii `
        -Value "set -gq $Option yes"
}

function Assert-TmuxOption {
    param(
        [string]$Label,
        [string]$Root,
        [hashtable]$Environment,
        [string[]]$RemoveEnvironment,
        [string]$Option
    )

    $result = Invoke-Tmux -Arguments @(
        "-L", $Label, "show-options", "-gqv", $Option
    ) -WorkingDirectory $Root -Environment $Environment `
        -RemoveEnvironment $RemoveEnvironment
    $value = @($result.Output | Select-Object -First 1)
    Assert-True ($value.Count -eq 1 -and $value[0] -eq "yes") `
        "expected $Option to be yes, got '$($value -join ',')'"
}

$label = "$LabelPrefix-$([Guid]::NewGuid().ToString("N"))"
$root = Join-Path ([System.IO.Path]::GetTempPath()) $label
$envMap = @{
    HOME = $root
    USERPROFILE = $root
}
$removeEnv = @("TMUX", "TMUX_WIN32_HANDLE_TTY")

New-Item -ItemType Directory -Path $root -Force | Out-Null
$caseRoot = Join-Path $root "cwd-[literal]"
New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null

try {
    New-ConfigFile (Join-Path $caseRoot "bracket-a.conf") "@glob-bracket-a"
    New-ConfigFile (Join-Path $caseRoot "bracket-b.conf") "@glob-bracket-b"

    $groupA = Join-Path $caseRoot "group-a"
    $groupB = Join-Path $caseRoot "group-b"
    New-Item -ItemType Directory -Path $groupA -Force | Out-Null
    New-Item -ItemType Directory -Path $groupB -Force | Out-Null
    New-ConfigFile (Join-Path $groupA "inside.conf") "@glob-group-a"
    New-ConfigFile (Join-Path $groupB "inside.conf") "@glob-group-b"

    $caseDir = Join-Path $caseRoot "case-dir"
    New-Item -ItemType Directory -Path $caseDir -Force | Out-Null
    New-ConfigFile (Join-Path $caseDir "CASE-CONFIG.CONF") "@glob-case"

    $slashDir = Join-Path $caseRoot "slash-dir"
    New-Item -ItemType Directory -Path $slashDir -Force | Out-Null
    New-ConfigFile (Join-Path $slashDir "slash-one.conf") "@glob-slash"

    $unicodeName = "unicode-" + [char]0x914D + [char]0x7F6E + ".conf"
    New-ConfigFile (Join-Path $caseRoot $unicodeName) "@glob-unicode"

    Invoke-Tmux -Arguments @("-L", $label, "new-session", "-d") `
        -WorkingDirectory $caseRoot -Environment $envMap `
        -RemoveEnvironment $removeEnv | Out-Null

    foreach ($pattern in @(
        "bracket-[ab].conf",
        "group-[ab]/inside.conf",
        "case-dir/case-*.conf",
        "slash-dir\slash-*.conf",
        "unicode-*.conf"
    )) {
        Invoke-Tmux -Arguments @("-L", $label, "source-file", $pattern) `
            -WorkingDirectory $caseRoot -Environment $envMap `
            -RemoveEnvironment $removeEnv | Out-Null
    }

    foreach ($option in @(
        "@glob-bracket-a",
        "@glob-bracket-b",
        "@glob-group-a",
        "@glob-group-b",
        "@glob-case",
        "@glob-slash",
        "@glob-unicode"
    )) {
        Assert-TmuxOption $label $caseRoot $envMap $removeEnv $option
    }

    Write-Output "win32 glob smoke passed"
} finally {
    Invoke-Tmux -Arguments @("-L", $label, "kill-server") `
        -WorkingDirectory $caseRoot -Environment $envMap `
        -RemoveEnvironment $removeEnv -AllowFailure | Out-Null
    if ($KeepArtifacts) {
        Write-Output "kept artifacts in $root"
    } else {
        Remove-Item -LiteralPath $root -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
