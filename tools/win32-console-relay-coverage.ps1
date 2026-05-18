param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$SmokePath = (Join-Path $PSScriptRoot "win32-console-relay-smoke.ps1"),
    [string]$LabelPrefix = "w32-relcov",
    [string[]]$Cases = @(
        "baseline",
        "input-credit",
        "utf8-split",
        "invalid-utf8",
        "detach-backlog",
        "simulate-output-loss",
        "simulate-transport-lost"
    ),
    [switch]$IncludeBacklogCases,
    [switch]$IncludeUtf8Cases,
    [switch]$IncludeLossCases,
    [switch]$RemoveArtifactsOnSuccess
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

function Invoke-RelaySmokeCase {
    param(
        [string]$Name,
        [string]$LabelName,
        [string[]]$SmokeArguments,
        [int]$TimeoutMs = 120000
    )

    $caseRoot = Join-Path $script:ArtifactRoot $Name
    New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
    $smokeRoot = Join-Path $caseRoot "smoke"
    $resultPath = Join-Path $caseRoot "result.json"
    $childArguments = @(
        "-NoLogo",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $script:SmokePath,
        "-TmuxPath",
        $script:TmuxPath,
        "-LabelPrefix",
        $LabelName,
        "-RootPath",
        $smokeRoot,
        "-ResultPath",
        $resultPath
    ) + $SmokeArguments

    $commandLine = Join-Win32Arguments $childArguments
    $process = Start-Process -FilePath "powershell.exe" `
        -ArgumentList $commandLine `
        -WorkingDirectory (Split-Path -Parent $script:SmokePath) `
        -WindowStyle Normal `
        -PassThru

    if (-not $process.WaitForExit($TimeoutMs)) {
        try {
            $process.Kill()
        } catch {
        }
        throw "relay smoke case $Name did not exit within ${TimeoutMs}ms"
    }

    $result = $null
    if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    }

    if ($process.ExitCode -ne 0) {
        throw "relay smoke case $Name failed with exit code $($process.ExitCode): result=$($result | ConvertTo-Json -Depth 8)"
    }

    if ($null -eq $result) {
        throw "relay smoke case $Name exited successfully but did not write a result file: $resultPath"
    }

    if (-not $result.passed) {
        throw "relay smoke case $Name reported failure: $($result | ConvertTo-Json -Depth 8)"
    }

    Write-Host "  $Name passed"
    [pscustomobject]@{
        Name = $Name
        Result = $result
        ResultPath = $resultPath
        SmokeRoot = $smokeRoot
        CaseRoot = $caseRoot
    }
}

if (-not (Test-Path -LiteralPath $TmuxPath -PathType Leaf)) {
    throw "tmux executable not found: $TmuxPath"
}
if (-not (Test-Path -LiteralPath $SmokePath -PathType Leaf)) {
    throw "relay smoke script not found: $SmokePath"
}

$script:TmuxPath = (Resolve-Path -LiteralPath $TmuxPath).Path
$script:SmokePath = (Resolve-Path -LiteralPath $SmokePath).Path
$script:ArtifactRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "tmux-win32-console-relay-coverage-" + [Guid]::NewGuid().ToString("N")
)
New-Item -ItemType Directory -Path $script:ArtifactRoot -Force | Out-Null

$expandedCases = [System.Collections.Generic.List[string]]::new()
foreach ($case in $Cases) {
    foreach ($name in @($case -split ',')) {
        $trimmed = $name.Trim()
        if ($trimmed -eq "") {
            continue
        }
        if (-not $expandedCases.Contains($trimmed)) {
            [void]$expandedCases.Add($trimmed)
        }
    }
}
if ($IncludeUtf8Cases) {
    foreach ($case in @("utf8-split", "invalid-utf8")) {
        if (-not $expandedCases.Contains($case)) {
            [void]$expandedCases.Add($case)
        }
    }
}
if ($IncludeLossCases) {
    foreach ($case in @("detach-backlog", "simulate-output-loss", "simulate-transport-lost")) {
        if (-not $expandedCases.Contains($case)) {
            [void]$expandedCases.Add($case)
        }
    }
}
if ($IncludeBacklogCases) {
    foreach ($case in @("output-progress", "resize-backlog")) {
        if (-not $expandedCases.Contains($case)) {
            [void]$expandedCases.Add($case)
        }
    }
}

function Get-SmokeArgumentsForCase {
    param([string]$Case)

    switch ($Case) {
        "baseline" { return @("-AutoDetachAfterMs", "1500") }
        "input-credit" { return @("-ExerciseInputCredit") }
        "output-progress" { return @("-ExerciseOutputProgress") }
        "resize-backlog" { return @("-ExerciseResizeBacklog") }
        "utf8-split" { return @("-ExerciseUtf8Split") }
        "invalid-utf8" { return @("-ExerciseInvalidUtf8") }
        "detach-backlog" { return @("-ExerciseDetachBacklog") }
        "simulate-output-loss" { return @("-SimulateOutputLoss") }
        "simulate-transport-lost" { return @("-SimulateTransportLost") }
        default { throw "unknown relay smoke case: $Case" }
    }
}

function Get-SmokeLabelForCase {
    param([string]$Case)

    $suffix = switch ($Case) {
        "baseline" { "base" }
        "input-credit" { "ic" }
        "output-progress" { "op" }
        "resize-backlog" { "rz" }
        "utf8-split" { "u8s" }
        "invalid-utf8" { "iu8" }
        "detach-backlog" { "db" }
        "simulate-output-loss" { "ol" }
        "simulate-transport-lost" { "tl" }
        default { throw "unknown relay smoke case: $Case" }
    }

    "{0}-{1}" -f $LabelPrefix, $suffix
}

$results = @()
try {
    foreach ($case in $expandedCases) {
        Write-Host "Running relay smoke case: $case"
        $results += Invoke-RelaySmokeCase `
            -Name $case `
            -LabelName (Get-SmokeLabelForCase -Case $case) `
            -SmokeArguments (Get-SmokeArgumentsForCase -Case $case)
    }

    Write-Host ""
    Write-Host "Relay console coverage summary:"
    foreach ($result in $results) {
        Write-Host ("  {0}: {1}" -f $result.Name, $result.ResultPath)
    }

    if ($RemoveArtifactsOnSuccess) {
        Remove-Item -LiteralPath $script:ArtifactRoot -Recurse -Force
    }
    exit 0
} catch {
    Write-Host ""
    Write-Host "Relay console coverage failed."
    Write-Host "Artifacts: $script:ArtifactRoot"
    throw
}
