param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$ProbePath,
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
        $result += '\' * $backslashes
        $backslashes = 0
        $result += $character
    }
    $result += '\' * ($backslashes * 2)
    $result + '"'
}

function Invoke-Tmux {
    param([string[]]$Arguments, [int]$TimeoutMs = 10000, [switch]$AllowFailure)
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $script:Root
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = (@($Arguments) | ForEach-Object { ConvertTo-Win32Argument $_ }) -join ' '
    $p = [Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutMs)) {
        $p.Kill()
        $p.WaitForExit()
        throw "tmux command timed out: $($Arguments -join ' ')"
    }
    $result = [pscustomobject]@{ Code = $p.ExitCode; Out = $stdout.Result; Err = $stderr.Result }
    $p.Dispose()
    if (-not $AllowFailure -and $result.Code -ne 0) {
        throw "tmux failed: $($Arguments -join ' '): $($result.Err)"
    }
    return $result
}

function Get-PipeName {
    param([string]$SocketPath)
    $identity = [IO.Path]::GetFullPath($SocketPath).ToLowerInvariant().Replace('\', '/')
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity))
        'tmux-v2-' + ([BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant())
    } finally {
        $sha.Dispose()
    }
}

function Open-VersionPeer {
    param([string]$SocketPath)
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', (Get-PipeName $SocketPath),
        [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.Connect(2000)
        # MSG_IDENTIFY_FLAGS with the previous protocol version: the server
        # must send MSG_VERSION, then close without waiting for us to read it.
        $header = [byte[]](@(100, 16, ($script:ProtocolVersion - 1), $PID) |
            ForEach-Object { [BitConverter]::GetBytes([uint32]$_) })
        $pipe.Write($header, 0, $header.Length)
        return $pipe
    } catch {
        $pipe.Dispose()
        throw
    }
}

$script:TmuxPath = [IO.Path]::GetFullPath($TmuxPath)
$protocol = Get-Content (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmux-protocol.h') -Raw
if ($protocol -notmatch '#define PROTOCOL_VERSION (\d+)') { throw 'Missing protocol version' }
$script:ProtocolVersion = [int]$Matches[1]
$script:Root = Join-Path $env:LOCALAPPDATA ('tmux-ipc-transport-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $script:Root | Out-Null
$socket = Join-Path $script:Root 'socket-é'
$other = Join-Path $script:Root 'other'
$peer = $null
$collisionPaths = @()

try {
    if ($ProbePath) {
        $collisionPaths = @(& $ProbePath --collision (Join-Path $script:Root 'collision-'))
        if ($LASTEXITCODE -ne 0 -or $collisionPaths.Count -ne 2) {
            throw 'Could not generate the old 32-bit pipe-name collision'
        }
        $serverIds = @()
        foreach ($path in $collisionPaths) {
            Invoke-Tmux -Arguments @('-S', $path, '-f', 'NUL', 'new-session', '-d') | Out-Null
            $serverIds += (Invoke-Tmux -Arguments @('-S', $path, 'display-message', '-p', '#{pid}')).Out.Trim()
        }
        if ($serverIds[0] -eq $serverIds[1]) { throw 'Colliding legacy names reached the same server' }
        'PASS: two paths with the same legacy 32-bit hash start independent servers'
    }
    Invoke-Tmux -Arguments @('-S', $socket, '-f', 'NUL', 'new-session', '-d') | Out-Null
    $pid1 = (Invoke-Tmux -Arguments @('-S', $socket, 'display-message', '-p', '#{pid}')).Out.Trim()
    $alias = $socket.ToUpperInvariant().Replace('\', '/')
    $pid2 = (Invoke-Tmux -Arguments @('-S', $alias, 'display-message', '-p', '#{pid}')).Out.Trim()
    if ($pid1 -ne $pid2) { throw 'Unicode case alias reached a different server' }

    Invoke-Tmux -Arguments @('-S', $other, '-f', 'NUL', 'new-session', '-d') | Out-Null
    $pid2 = (Invoke-Tmux -Arguments @('-S', $other, 'display-message', '-p', '#{pid}')).Out.Trim()
    if ($pid1 -eq $pid2) { throw 'Distinct endpoints reached the same server' }
    'PASS: Unicode endpoint alias and distinct endpoints'

    $payload = ('0123456789abcdefghijklmnopqrstuvwxyz' * 40000) + 'FINAL_BYTES'
    $inputFile = Join-Path $script:Root 'input'
    [IO.File]::WriteAllText($inputFile, $payload, [Text.Encoding]::ASCII)
    Invoke-Tmux -Arguments @('-S', $socket, 'load-buffer', '-b', 'ipc-test', $inputFile) | Out-Null
    $received = Invoke-Tmux -Arguments @('-S', $socket, 'save-buffer', '-b', 'ipc-test', '-')
    if ($received.Out -cne $payload) { throw 'Bulk bidirectional IPC lost or duplicated bytes' }
    $failed = Invoke-Tmux -Arguments @('-S', $socket, 'not-a-command') -AllowFailure
    if ($failed.Code -eq 0 -or $failed.Err -notmatch 'unknown command') {
        throw 'Command error or exit status was lost'
    }
    'PASS: 1.4 MB buffer round trip and final command error'

    if ($ProbePath) {
        foreach ($mode in @('', '--reject')) {
            $probePsi = [Diagnostics.ProcessStartInfo]::new()
            $probePsi.FileName = [IO.Path]::GetFullPath($ProbePath)
            $probePsi.UseShellExecute = $false
            $probePsi.CreateNoWindow = $true
            $probePsi.RedirectStandardOutput = $true
            $probePsi.RedirectStandardError = $true
            $probePsi.Arguments = ($mode + ' ' + (Get-PipeName $socket)).Trim()
            $probe = [Diagnostics.Process]::Start($probePsi)
            try {
                $ready = $probe.StandardOutput.ReadLineAsync()
                if (-not $ready.Wait(5000) -or $ready.Result -ne 'Holding bulk output unread') {
                    throw 'Bulk-output probe did not start'
                }
                Start-Sleep -Milliseconds 200
                $reply = Invoke-Tmux -Arguments @('-S', $socket, 'display-message', '-p', 'responsive') -TimeoutMs 1500
                if ($reply.Out.Trim() -ne 'responsive') { throw 'Full IPC pipe blocked another client' }
                if (-not $probe.WaitForExit(8000) -or $probe.ExitCode -ne 0) {
                    throw 'Bulk-output probe failed'
                }
            } finally {
                if (-not $probe.HasExited) { $probe.Kill(); $probe.WaitForExit() }
                $probe.Dispose()
            }
        }
        'PASS: full IPC pipe stays responsive; disconnect cancels writes and bad peers expire'
    }
    $peer = Open-VersionPeer $socket
    Start-Sleep -Milliseconds 100
    $reply = Invoke-Tmux -Arguments @('-S', $socket, 'display-message', '-p', 'responsive') -TimeoutMs 2000
    if ($reply.Out.Trim() -ne 'responsive') { throw 'Nonreading peer blocked the server' }
    # The response must survive closing the server's handle. Read it only
    # after giving the server time to close, without FlushFileBuffers.
    $header = New-Object byte[] 16
    $read = $peer.ReadAsync($header, 0, $header.Length)
    if (-not $read.Wait(2000) -or $read.Result -ne 16 -or
        [BitConverter]::ToUInt32($header, 0) -ne 12 -or
        [BitConverter]::ToUInt32($header, 8) -ne $script:ProtocolVersion) {
        throw 'Final protocol-version reply was lost on close'
    }
    $peer.Dispose()
    $peer = $null
    'PASS: nonreading peer cannot stall server; delayed final reply survives close'

    Start-Sleep -Milliseconds 200
    $server = [Diagnostics.Process]::GetProcessById([int]$pid1)
    $server.Refresh()
    $handlesBefore = $server.HandleCount
    for ($i = 0; $i -lt 100; $i++) {
        $peer = Open-VersionPeer $socket
        $peer.Dispose()
        $peer = $null
    }
    $reply = Invoke-Tmux -Arguments @('-S', $socket, 'display-message', '-p', 'survived')
    if ($reply.Out.Trim() -ne 'survived') { throw 'Disconnect race damaged the server' }
    Start-Sleep -Milliseconds 250
    $server.Refresh()
    if ($server.HandleCount -gt $handlesBefore + 8) {
        throw "Disconnects leaked handles: $handlesBefore -> $($server.HandleCount)"
    }
    $server.Dispose()
    'PASS: 100 peers disconnect during response and cancellation'
} finally {
    if ($null -ne $peer) { $peer.Dispose() }
    foreach ($path in (@($socket, $other) + $collisionPaths)) {
        Invoke-Tmux -Arguments @('-S', $path, 'kill-server') -AllowFailure | Out-Null
    }
    if ($KeepArtifacts) {
        "Artifacts: $script:Root"
    } else {
        # Root is the exact directory created above, not a glob or parent.
        Remove-Item -LiteralPath $script:Root -Recurse -Force
    }
}
