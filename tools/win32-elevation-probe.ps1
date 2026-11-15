$ErrorActionPreference = 'Stop'
$out = $args[0]

$lines = New-Object System.Collections.Generic.List[string]
try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    $adm = $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $lines.Add("user=" + $id.Name)
    $lines.Add("sid=" + $id.User.Value)
    $lines.Add("elevated=" + $adm)
    $lines.Add("pid=" + $PID)
    $lines.Add("mode=" + [System.Environment]::GetEnvironmentVariable('TMUX_TEST_MODE'))
} catch {
    $lines.Add("ERROR=" + $_.Exception.Message)
}
[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
