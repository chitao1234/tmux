param(
    [string]$TmuxPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "tmux.exe"),
    [string]$LabelPrefix = "win32-console-relay-smoke",
    [string]$RootPath,
    [string]$ResultPath,
    [int]$AutoDetachAfterMs = 0,
    [switch]$ExerciseMouse,
    [switch]$ExerciseInputCredit,
    [switch]$ExerciseCtrlJ,
    [switch]$SimulateOutputLoss,
    [switch]$SimulateTransportLost,
    [switch]$ExerciseDetachBacklog,
    [switch]$ExerciseOutputProgress,
    [switch]$ExerciseResizeBacklog,
    [switch]$ExerciseUtf8Split,
    [switch]$ExerciseInvalidUtf8,
    [switch]$RemoveLogsOnSuccess
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

function Invoke-TmuxInteractive {
    param(
        [string[]]$Arguments,
        [int]$TimeoutMs = 0,
        [scriptblock]$AfterStart
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:TmuxPath
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.Arguments = Join-Win32Arguments $Arguments

    $process = [System.Diagnostics.Process]::Start($psi)
    $processId = $process.Id
    if ($AfterStart -ne $null) {
        & $AfterStart $processId
    }
    if ($TimeoutMs -gt 0) {
        if (-not $process.WaitForExit($TimeoutMs)) {
            $process.Kill()
            throw "tmux $($Arguments -join ' ') did not exit within ${TimeoutMs}ms"
        }
    } else {
        $process.WaitForExit()
    }
    [pscustomobject]@{
        ExitCode = $process.ExitCode
        ProcessId = $processId
    }
}

function Initialize-ConsoleInputInterop {
    if ("Win32ConsoleInput" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class Win32ConsoleInput
{
    [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
    public struct INPUT_RECORD
    {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public KEY_EVENT_RECORD KeyEvent;
        [FieldOffset(4)] public MOUSE_EVENT_RECORD MouseEvent;
    }

    [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
    public struct KEY_EVENT_RECORD
    {
        [FieldOffset(0)]
        [MarshalAs(UnmanagedType.Bool)] public bool bKeyDown;
        [FieldOffset(4)] public ushort wRepeatCount;
        [FieldOffset(6)] public ushort wVirtualKeyCode;
        [FieldOffset(8)] public ushort wVirtualScanCode;
        [FieldOffset(10)] public char UnicodeChar;
        [FieldOffset(12)] public uint dwControlKeyState;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct COORD
    {
        public short X;
        public short Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSE_EVENT_RECORD
    {
        public COORD dwMousePosition;
        public uint dwButtonState;
        public uint dwControlKeyState;
        public uint dwEventFlags;
    }

    const ushort KEY_EVENT = 0x0001;
    const ushort MOUSE_EVENT = 0x0002;
    const ushort VK_CONTROL = 0x0011;
    const ushort VK_J = 0x004a;
    const uint LEFT_CTRL_PRESSED = 0x0008;
    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 0x00000001;
    const uint FILE_SHARE_WRITE = 0x00000002;
    const uint OPEN_EXISTING = 3;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint desiredAccess,
        uint shareMode, IntPtr securityAttributes, uint creationDisposition,
        uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteConsoleInputW(IntPtr consoleInput,
        INPUT_RECORD[] buffer, uint length, out uint eventsWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);

    static IntPtr OpenConsoleInput()
    {
        IntPtr handle = CreateFileW("CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0,
            IntPtr.Zero);
        if (handle == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "CreateFileW(CONIN$) failed");
        return handle;
    }

    static INPUT_RECORD KeyRecord(bool keyDown, ushort virtualKey,
        ushort scanCode, char unicodeChar, uint controlKeyState)
    {
        INPUT_RECORD record = new INPUT_RECORD();
        record.EventType = KEY_EVENT;
        record.KeyEvent.bKeyDown = keyDown;
        record.KeyEvent.wRepeatCount = 1;
        record.KeyEvent.wVirtualKeyCode = virtualKey;
        record.KeyEvent.wVirtualScanCode = scanCode;
        record.KeyEvent.UnicodeChar = unicodeChar;
        record.KeyEvent.dwControlKeyState = controlKeyState;
        return record;
    }

    static void WriteRecords(INPUT_RECORD[] records, string what)
    {
        IntPtr handle = OpenConsoleInput();

        try
        {
            uint written;
            if (!WriteConsoleInputW(handle, records, (uint)records.Length,
                out written))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "WriteConsoleInputW(" + what + ") failed");
            if (written != (uint)records.Length)
                throw new InvalidOperationException(
                    "WriteConsoleInputW(" + what + ") wrote " + written +
                    " of " + records.Length + " events");
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    public static void WriteText(string text)
    {
        if (string.IsNullOrEmpty(text))
            return;

        INPUT_RECORD[] records = new INPUT_RECORD[text.Length];
        for (int i = 0; i < text.Length; i++)
        {
            records[i] = KeyRecord(true, 0, 0, text[i], 0);
        }

        WriteRecords(records, "text");
    }

    public static void WriteCtrlJ()
    {
        INPUT_RECORD[] records = new INPUT_RECORD[4];

        records[0] = KeyRecord(true, VK_CONTROL, 0, '\0',
            LEFT_CTRL_PRESSED);
        records[1] = KeyRecord(true, VK_J, 0, '\n',
            LEFT_CTRL_PRESSED);
        records[2] = KeyRecord(false, VK_J, 0, '\0',
            LEFT_CTRL_PRESSED);
        records[3] = KeyRecord(false, VK_CONTROL, 0, '\0', 0);

        WriteRecords(records, "ctrl-j");
    }

    public static void WriteMouse(short x, short y, uint buttonState,
        uint controlKeyState, uint eventFlags)
    {
        IntPtr handle = OpenConsoleInput();

        try
        {
            INPUT_RECORD[] records = new INPUT_RECORD[1];
            records[0].EventType = MOUSE_EVENT;
            records[0].MouseEvent.dwMousePosition.X = x;
            records[0].MouseEvent.dwMousePosition.Y = y;
            records[0].MouseEvent.dwButtonState = buttonState;
            records[0].MouseEvent.dwControlKeyState = controlKeyState;
            records[0].MouseEvent.dwEventFlags = eventFlags;

            uint written;
            if (!WriteConsoleInputW(handle, records, 1, out written))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "WriteConsoleInputW(mouse) failed");
            if (written != 1)
                throw new InvalidOperationException(
                    "WriteConsoleInputW(mouse) wrote " + written +
                    " of 1 events");
        }
        finally
        {
            CloseHandle(handle);
        }
    }
}
"@
}

function Write-ConsoleInputText {
    param(
        [string]$Text,
        [int]$ChunkSize = 512,
        [int]$ChunkDelayMs = 1
    )

    Initialize-ConsoleInputInterop

    $offset = 0
    while ($offset -lt $Text.Length) {
        $length = [Math]::Min($ChunkSize, $Text.Length - $offset)
        [Win32ConsoleInput]::WriteText($Text.Substring($offset, $length))
        $offset += $length
        if ($ChunkDelayMs -gt 0 -and $offset -lt $Text.Length) {
            Start-Sleep -Milliseconds $ChunkDelayMs
        }
    }
}

function Write-ConsoleInputCtrlJ {
    Initialize-ConsoleInputInterop

    [Win32ConsoleInput]::WriteCtrlJ()
}

function Write-ConsoleInputMouse {
    param(
        [int]$X,
        [int]$Y,
        [uint32]$ButtonState,
        [uint32]$EventFlags = 0,
        [uint32]$ControlKeyState = 0
    )

    Initialize-ConsoleInputInterop

    [Win32ConsoleInput]::WriteMouse(
        [int16]$X,
        [int16]$Y,
        $ButtonState,
        $ControlKeyState,
        $EventFlags
    )
}

function Initialize-ConsoleOutputInterop {
    if ("Win32ConsoleOutput" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class Win32ConsoleOutput
{
    [StructLayout(LayoutKind.Sequential)]
    public struct COORD
    {
        public short X;
        public short Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SMALL_RECT
    {
        public short Left;
        public short Top;
        public short Right;
        public short Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CONSOLE_SCREEN_BUFFER_INFO
    {
        public COORD dwSize;
        public COORD dwCursorPosition;
        public short wAttributes;
        public SMALL_RECT srWindow;
        public COORD dwMaximumWindowSize;
    }

    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 0x00000001;
    const uint FILE_SHARE_WRITE = 0x00000002;
    const uint OPEN_EXISTING = 3;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint desiredAccess,
        uint shareMode, IntPtr securityAttributes, uint creationDisposition,
        uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetConsoleScreenBufferInfo(IntPtr consoleOutput,
        out CONSOLE_SCREEN_BUFFER_INFO info);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleScreenBufferSize(IntPtr consoleOutput,
        COORD size);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleWindowInfo(IntPtr consoleOutput,
        [MarshalAs(UnmanagedType.Bool)] bool absolute,
        ref SMALL_RECT window);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool ReadConsoleOutputCharacterW(IntPtr consoleOutput,
        StringBuilder buffer, uint length, COORD coord,
        out uint numberOfCharsRead);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);

    public static string ReadVisibleText()
    {
        IntPtr handle = CreateFileW("CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0,
            IntPtr.Zero);
        if (handle == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "CreateFileW(CONOUT$) failed");

        try
        {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(handle, out info))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "GetConsoleScreenBufferInfo failed");

            int width = info.srWindow.Right - info.srWindow.Left + 1;
            int top = info.srWindow.Top;
            int bottom = info.srWindow.Bottom;
            StringBuilder all = new StringBuilder(width * (bottom - top + 1));

            for (int row = top; row <= bottom; row++)
            {
                StringBuilder line = new StringBuilder(width);
                COORD coord;
                coord.X = info.srWindow.Left;
                coord.Y = (short)row;

                uint read;
                if (!ReadConsoleOutputCharacterW(handle, line, (uint)width,
                    coord, out read))
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "ReadConsoleOutputCharacterW failed");

                string text = line.ToString();
                if (text.Length < width)
                    text = text.PadRight(width);
                all.Append(text, 0, width);
            }
            return all.ToString();
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    public static string GetWindowSize()
    {
        IntPtr handle = CreateFileW("CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0,
            IntPtr.Zero);
        if (handle == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "CreateFileW(CONOUT$) failed");

        try
        {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(handle, out info))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "GetConsoleScreenBufferInfo failed");

            int width = info.srWindow.Right - info.srWindow.Left + 1;
            int height = info.srWindow.Bottom - info.srWindow.Top + 1;
            return width + "x" + height;
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    public static void SetWindowSize(int width, int height)
    {
        IntPtr handle = CreateFileW("CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0,
            IntPtr.Zero);
        if (handle == new IntPtr(-1))
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "CreateFileW(CONOUT$) failed");

        try
        {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(handle, out info))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "GetConsoleScreenBufferInfo failed");

            COORD bufferSize;
            bufferSize.X = (short)Math.Max(info.dwSize.X, width);
            bufferSize.Y = (short)Math.Max(info.dwSize.Y, height);
            if ((int)bufferSize.X != info.dwSize.X ||
                (int)bufferSize.Y != info.dwSize.Y) {
                if (!SetConsoleScreenBufferSize(handle, bufferSize))
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "SetConsoleScreenBufferSize failed");
            }

            SMALL_RECT window;
            window.Left = info.srWindow.Left;
            window.Top = info.srWindow.Top;
            window.Right = (short)(window.Left + width - 1);
            window.Bottom = (short)(window.Top + height - 1);
            if (!SetConsoleWindowInfo(handle, true, ref window))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "SetConsoleWindowInfo failed");
        }
        finally
        {
            CloseHandle(handle);
        }
    }
}
"@
}

function Get-ConsoleVisibleText {
    Initialize-ConsoleOutputInterop
    [Win32ConsoleOutput]::ReadVisibleText()
}

function Get-ConsoleWindowSizeNative {
    Initialize-ConsoleOutputInterop
    $sizeText = [Win32ConsoleOutput]::GetWindowSize()
    if ($sizeText -notmatch '^([0-9]+)x([0-9]+)$') {
        throw "Unexpected native console size text: $sizeText"
    }
    [pscustomobject]@{
        Width = [int]$Matches[1]
        Height = [int]$Matches[2]
    }
}

function Wait-ConsoleVisibleText {
    param(
        [string]$Needle,
        [int]$TimeoutMs = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        if ((Get-ConsoleVisibleText).Contains($Needle)) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $false
}

function Wait-ConsoleWindowSizeNative {
    param(
        [int]$Width,
        [int]$Height,
        [int]$TimeoutMs = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $size = Get-ConsoleWindowSizeNative
        if ($size.Width -eq $Width -and $size.Height -eq $Height) {
            return $size
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $null
}

function Get-ConsoleWindowSize {
    $size = $Host.UI.RawUI.WindowSize
    [pscustomobject]@{
        Width = $size.Width
        Height = $size.Height
    }
}

function Get-AlternateConsoleWindowSize {
    param(
        [int]$Width,
        [int]$Height
    )

    $targetWidth = $Width
    $targetHeight = $Height

    if ($Width -gt 100) {
        $targetWidth = $Width - 8
    } elseif ($Height -gt 32) {
        $targetHeight = $Height - 4
    } elseif ($Width -gt 52) {
        $targetWidth = $Width - 4
    } elseif ($Height -gt 20) {
        $targetHeight = $Height - 2
    } else {
        $targetWidth = $Width + 4
    }
    if ($targetWidth -eq $Width -and $targetHeight -eq $Height) {
        throw "Could not choose an alternate console size from ${Width}x${Height}."
    }

    [pscustomobject]@{
        Width = $targetWidth
        Height = $targetHeight
    }
}

function Get-AlternateConsoleWindowSizeCandidates {
    param(
        [int]$Width,
        [int]$Height
    )

    $candidates = [System.Collections.Generic.List[object]]::new()

    function Add-SizeCandidate {
        param(
            [int]$CandidateWidth,
            [int]$CandidateHeight
        )

        if ($CandidateWidth -lt 20 -or $CandidateHeight -lt 8) {
            return
        }
        if ($CandidateWidth -eq $Width -and $CandidateHeight -eq $Height) {
            return
        }
        foreach ($existing in $candidates) {
            if ($existing.Width -eq $CandidateWidth -and
                $existing.Height -eq $CandidateHeight) {
                return
            }
        }
        [void]$candidates.Add([pscustomobject]@{
            Width = $CandidateWidth
            Height = $CandidateHeight
        })
    }

    $preferred = Get-AlternateConsoleWindowSize -Width $Width -Height $Height
    Add-SizeCandidate -CandidateWidth $preferred.Width `
        -CandidateHeight $preferred.Height
    Add-SizeCandidate -CandidateWidth ($Width - 8) -CandidateHeight $Height
    Add-SizeCandidate -CandidateWidth ($Width - 4) -CandidateHeight $Height
    Add-SizeCandidate -CandidateWidth ($Width + 4) -CandidateHeight $Height
    Add-SizeCandidate -CandidateWidth $Width -CandidateHeight ($Height - 4)
    Add-SizeCandidate -CandidateWidth $Width -CandidateHeight ($Height - 2)
    Add-SizeCandidate -CandidateWidth $Width -CandidateHeight ($Height + 2)
    Add-SizeCandidate -CandidateWidth $Width -CandidateHeight ($Height + 4)

    $candidates
}

function Invoke-ObservedConsoleResize {
    param(
        [int]$OriginalWidth,
        [int]$OriginalHeight,
        [int]$RequestedWidth,
        [int]$RequestedHeight,
        [int]$TimeoutMs = 2000
    )

    try {
        Set-ConsoleWindowSize -Width $RequestedWidth -Height $RequestedHeight
    } catch {
        return $null
    }

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $size = Get-ConsoleWindowSizeNative
        if ($size.Width -eq $RequestedWidth -and
            $size.Height -eq $RequestedHeight) {
            return $size
        }
        if ($size.Width -ne $OriginalWidth -or
            $size.Height -ne $OriginalHeight) {
            return $size
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $null
}

function Set-ConsoleWindowSize {
    param(
        [int]$Width,
        [int]$Height
    )

    Initialize-ConsoleOutputInterop
    [Win32ConsoleOutput]::SetWindowSize($Width, $Height)
}

function Test-AnyLogMatch {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    foreach ($log in $Logs) {
        if (Select-String -LiteralPath $log.FullName -Pattern $Pattern -Quiet) {
            return $true
        }
    }
    $false
}

function Get-LogMatches {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    foreach ($log in $Logs) {
        Select-String -LiteralPath $log.FullName -Pattern $Pattern |
            ForEach-Object {
                "{0}:{1}: {2}" -f $log.Name, $_.LineNumber, $_.Line
            }
    }
}

function Get-LogMatchCount {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    $count = 0
    foreach ($log in $Logs) {
        $count += @(Select-String -LiteralPath $log.FullName -Pattern $Pattern).Count
    }
    $count
}

function Get-LogMaximumCapture {
    param(
        [System.IO.FileInfo[]]$Logs,
        [string]$Pattern
    )

    $maximum = $null
    foreach ($log in $Logs) {
        foreach ($match in Select-String -LiteralPath $log.FullName -Pattern $Pattern) {
            $value = [int64]$match.Matches[0].Groups[1].Value
            if ($maximum -eq $null -or $value -gt $maximum) {
                $maximum = $value
            }
        }
    }
    if ($maximum -eq $null) {
        return -1
    }
    $maximum
}

function Get-CurrentTmuxLogs {
    param(
        [string]$Path
    )

    @(Get-ChildItem -LiteralPath $Path -Filter "tmux-*.log" -File)
}

function Wait-LogMatch {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$TimeoutMs = 5000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $logs = Get-CurrentTmuxLogs -Path $Path
        if (Test-AnyLogMatch $logs $Pattern) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $false
}

function Get-TmuxClientSize {
    param(
        [string]$Config,
        [string]$Label,
        [int]$ClientPid
    )

    $result = Invoke-Tmux -Arguments @(
        "-f",
        $Config,
        "-L",
        $Label,
        "list-clients",
        "-F",
        "#{client_name} #{client_width} #{client_height}"
    ) -AllowFailure
    if ($result.ExitCode -ne 0) {
        return $null
    }
    foreach ($line in $result.Output) {
        if ($line -match "^client-$ClientPid\s+([0-9]+)\s+([0-9]+)$") {
            return [pscustomobject]@{
                Width = [int]$Matches[1]
                Height = [int]$Matches[2]
            }
        }
    }
    $null
}

function Wait-TmuxClientSize {
    param(
        [string]$Config,
        [string]$Label,
        [int]$ClientPid,
        [int]$Width,
        [int]$Height,
        [int]$TimeoutMs = 3000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $size = Get-TmuxClientSize -Config $Config -Label $Label -ClientPid $ClientPid
        if ($size -ne $null -and $size.Width -eq $Width -and $size.Height -eq $Height) {
            return $size
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    $null
}

function Write-TextFileUtf8NoBom {
    param(
        [string]$Path,
        [string]$Content
    )

    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Write-SmokeResult {
    param(
        [string]$Path,
        [object]$Result
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $directory = Split-Path -Parent $Path
    if ($directory -ne "" -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    Write-TextFileUtf8NoBom -Path $Path -Content ($Result | ConvertTo-Json -Depth 8)
}

function Get-SmokeScenarioName {
    if ($ExerciseMouse) {
        return "mouse"
    }
    if ($ExerciseInputCredit) {
        return "input-credit"
    }
    if ($ExerciseCtrlJ) {
        return "ctrl-j"
    }
    if ($SimulateOutputLoss) {
        return "simulate-output-loss"
    }
    if ($SimulateTransportLost) {
        return "simulate-transport-lost"
    }
    if ($ExerciseDetachBacklog) {
        return "detach-backlog"
    }
    if ($ExerciseOutputProgress) {
        return "output-progress"
    }
    if ($ExerciseResizeBacklog) {
        return "resize-backlog"
    }
    if ($ExerciseUtf8Split) {
        return "utf8-split"
    }
    if ($ExerciseInvalidUtf8) {
        return "invalid-utf8"
    }
    if ($AutoDetachAfterMs -gt 0) {
        return "baseline-auto-detach"
    }
    return "baseline-manual-detach"
}

function Write-Utf8SplitHelperScript {
    param([string]$Path)

    $content = @"
Start-Sleep -Milliseconds 750
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(`$false)
[Console]::Out.WriteLine(([string][char]0x6587) + 'UTF8-SPLIT-OK')
Start-Sleep -Seconds 30
"@
    Write-TextFileUtf8NoBom -Path $Path -Content $content
}

function Write-InvalidUtf8HelperScript {
    param([string]$Path)

    $content = @"
Start-Sleep -Milliseconds 750
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(`$false)
[Console]::Out.WriteLine('UTF8-INVALID-PREFIX-OK')
Start-Sleep -Seconds 30
"@
    Write-TextFileUtf8NoBom -Path $Path -Content $content
}

function Write-CtrlJHelperScript {
    param(
        [string]$Path,
        [string]$ResultPath
    )

    $escapedResultPath = $ResultPath.Replace("'", "''")
    $content = @"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(`$false)
[Console]::Out.WriteLine('CTRLJ-READY')

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class CtrlJNativeInput
{
    const int STD_INPUT_HANDLE = -10;

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int nStdHandle);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr handle, byte[] buffer,
        uint bytesToRead, out uint bytesRead, IntPtr overlapped);

    public static uint Read(byte[] buffer)
    {
        IntPtr handle = GetStdHandle(STD_INPUT_HANDLE);
        if (handle == new IntPtr(-1) || handle == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "GetStdHandle(STD_INPUT_HANDLE) failed");

        uint bytesRead;
        if (!ReadFile(handle, buffer, (uint)buffer.Length, out bytesRead,
            IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "ReadFile(STDIN) failed");
        return bytesRead;
    }
}
'@

`$buffer = New-Object byte[] 16
`$captured = New-Object 'System.Collections.Generic.List[byte]'

while (`$captured.Count -lt 1) {
    `$nread = [CtrlJNativeInput]::Read(`$buffer)
    if (`$nread -le 0) {
        break
    }
    for (`$i = 0; `$i -lt `$nread; `$i++) {
        [void]`$captured.Add(`$buffer[`$i])
    }
    if (`$captured.Contains([byte]10)) {
        break
    }
}

[System.IO.File]::WriteAllBytes('$escapedResultPath', `$captured.ToArray())
if (`$captured.Contains([byte]10)) {
    [Console]::Out.WriteLine('CTRLJ-CAPTURED')
}
Start-Sleep -Seconds 30
"@
    Write-TextFileUtf8NoBom -Path $Path -Content $content
}

function Write-MouseHelperScript {
    param(
        [string]$Path,
        [string]$ResultPath
    )

    $escapedResultPath = $ResultPath.Replace("'", "''")
    $content = @"
Start-Sleep -Milliseconds 750
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new(`$false)
`$stdout = [Console]::OpenStandardOutput()
`$enable = [System.Text.Encoding]::ASCII.GetBytes(
    ([string][char]27) + '[?1000h' +
    ([string][char]27) + '[?1002h' +
    ([string][char]27) + '[?1006h'
)
`$stdout.Write(`$enable, 0, `$enable.Length)
`$stdout.Flush()
[Console]::Out.WriteLine('MOUSE-READY')

`$input = [Console]::OpenStandardInput()
`$buffer = New-Object byte[] 256
`$captured = New-Object 'System.Collections.Generic.List[byte]'
`$escape = [string][char]27

while (`$true) {
    `$nread = `$input.Read(`$buffer, 0, `$buffer.Length)
    if (`$nread -le 0) {
        break
    }
    for (`$i = 0; `$i -lt `$nread; `$i++) {
        [void]`$captured.Add(`$buffer[`$i])
    }
    `$text = [System.Text.Encoding]::ASCII.GetString(`$captured.ToArray())
    if (`$text.Contains("`${escape}[<0;")) {
        break
    }
}

[System.IO.File]::WriteAllBytes('$escapedResultPath', `$captured.ToArray())
[Console]::Out.WriteLine('MOUSE-CAPTURED')
Start-Sleep -Seconds 30
"@
    Write-TextFileUtf8NoBom -Path $Path -Content $content
}

if ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected) {
    throw "This smoke must be run from a real Windows console, not redirected output or the Codex runner."
}
if (-not (Test-Path -LiteralPath $TmuxPath -PathType Leaf)) {
    throw "tmux executable not found: $TmuxPath"
}
$script:TmuxPath = (Resolve-Path -LiteralPath $TmuxPath).Path

$savedEnv = @{
    TMUX = $env:TMUX
    TMUX_WIN32_HANDLE_TTY = $env:TMUX_WIN32_HANDLE_TTY
    TMUX_WIN32_CONSOLE_RELAY = $env:TMUX_WIN32_CONSOLE_RELAY
    TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS
    TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST
    TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT = $env:TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT
    TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8 = $env:TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8
}

$root = if ([string]::IsNullOrWhiteSpace($RootPath)) {
    Join-Path ([System.IO.Path]::GetTempPath()) ("tmux-win32-console-relay-smoke-" + [Guid]::NewGuid().ToString("N"))
} else {
    $RootPath
}
$label = "$LabelPrefix-" + [Guid]::NewGuid().ToString("N")
$config = Join-Path $root "empty.conf"
$utf8SplitMarker = ([string][char]0x6587) + "UTF8-SPLIT-OK"
$invalidUtf8Prefix = "UTF8-INVALID-PREFIX-"
$mouseReadyMarker = "MOUSE-READY"
$mouseCapturedMarker = "MOUSE-CAPTURED"
$ctrlJReadyMarker = "CTRLJ-READY"
$ctrlJCapturedMarker = "CTRLJ-CAPTURED"
New-Item -ItemType Directory -Path $root -Force | Out-Null
New-Item -ItemType File -Path $config | Out-Null

try {
    $restoreConsoleSize = $null
    $resizeTarget = $null
    $resizeObserved = $null
    $outputProgressSmallProgressCount = -1
    $outputProgressSmallStatusRedrawCount = -1
    $outputProgressSmallThresholdDeferredCount = -1
    $outputProgressLargeProgressCount = -1
    $outputProgressLargeStatusRedrawCount = -1
    $outputProgressLargeThresholdDeferredCount = -1
    $utf8SplitVisible = $false
    $utf8SplitCarryLogged = $false
    $invalidUtf8PrefixVisible = $false
    $mouseResultBytes = [byte[]]@()
    $mouseResultText = ""
    $mouseReadyVisible = $false
    $mouseCapturedVisible = $false
    $mousePaneFlags = $null
    $ctrlJReadyVisible = $false
    $ctrlJCapturedVisible = $false
    $ctrlJResultBytes = [byte[]]@()
    $resizeConsoleObserved = $null

    $env:TMUX = $null
    $env:TMUX_WIN32_HANDLE_TTY = "0"
    $env:TMUX_WIN32_CONSOLE_RELAY = $null
    if ($SimulateOutputLoss) {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = "1"
    } else {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = $null
    }
    if ($SimulateTransportLost) {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = "1"
    } else {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = $null
    }
    if ($ExerciseUtf8Split) {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT = "1"
    } else {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT = $null
    }
    if ($ExerciseInvalidUtf8) {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8 = "1"
    } else {
        $env:TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8 = $null
    }

    Push-Location $root
    try {
        $sessionCommandArgs = @("cmd.exe")
        $mouseHelper = $null
        $mouseHelperCommand = $null
        $ctrlJResult = $null
        if ($ExerciseMouse) {
            $mouseHelper = Join-Path $root "mouse-helper.ps1"
            $mouseResult = Join-Path $root "mouse-result.bin"
            Write-MouseHelperScript -Path $mouseHelper -ResultPath $mouseResult
            $mouseHelperCommand = Join-Win32Arguments @(
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                $mouseHelper
            )
        } elseif ($ExerciseCtrlJ) {
            $ctrlJHelper = Join-Path $root "ctrl-j-helper.ps1"
            $ctrlJResult = Join-Path $root "ctrl-j-result.bin"
            Write-CtrlJHelperScript -Path $ctrlJHelper -ResultPath $ctrlJResult
            $sessionCommandArgs = @(
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                $ctrlJHelper
            )
        } elseif ($ExerciseInputCredit) {
            $sessionCommandArgs = @("cmd.exe", "/Q", "/K", "findstr .* >nul")
        } elseif ($ExerciseUtf8Split) {
            $splitHelper = Join-Path $root "utf8-split-helper.ps1"
            Write-Utf8SplitHelperScript -Path $splitHelper
            $sessionCommandArgs = @(
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                $splitHelper
            )
        } elseif ($ExerciseInvalidUtf8) {
            $invalidHelper = Join-Path $root "utf8-invalid-helper.ps1"
            Write-InvalidUtf8HelperScript -Path $invalidHelper
            $sessionCommandArgs = @(
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                $invalidHelper
            )
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
        Invoke-Tmux -Arguments (@(
            "-f",
            $config,
            "-vv",
            "-L",
            $label,
            "new-session",
            "-d",
            "-s",
            "relay"
        ) + $sessionCommandArgs) | Out-Null
        if ($ExerciseMouse -or $ExerciseCtrlJ -or $ExerciseUtf8Split -or
            $ExerciseInvalidUtf8) {
            Invoke-Tmux -Arguments @(
                "-f",
                $config,
                "-L",
                $label,
                "set-option",
                "-g",
                "status",
                "off"
            ) | Out-Null
        }
        if ($ExerciseMouse) {
            Invoke-Tmux -Arguments @(
                "-f",
                $config,
                "-L",
                $label,
                "set-option",
                "-g",
                "mouse",
                "on"
            ) | Out-Null
        }

        Write-Host "Launching native-console relay attach."
        if ($ExerciseMouse) {
            Write-Host "Relay mouse exercise is enabled. Relay mouse-state plumbing will be verified and a synthetic CONIN$ mouse injection will be attempted."
            Write-Host "Direct ReadFile VT mouse generation is host-sensitive; missing VT bytes from synthetic mouse records is reported but not treated as authoritative failure."
            Write-Host "Logs will be checked afterward: $root"
            Clear-Host
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 15000 -AfterStart {
                param([int]$AttachPid)

                $mouseResult = Join-Path $root "mouse-result.bin"

                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    $mouseHelperCommand
                ) | Out-Null
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "Enter"
                ) | Out-Null

                if (-not (Wait-ConsoleVisibleText -Needle $script:mouseReadyMarker -TimeoutMs 5000)) {
                    throw "timed out waiting for mouse helper readiness marker"
                }
                $script:mouseReadyVisible = $true
                $paneFlags = Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "display-message",
                    "-p",
                    "-t",
                    "relay:0.0",
                    "#{mouse_any_flag} #{mouse_button_flag} #{mouse_sgr_flag}"
                )
                if ($paneFlags.ExitCode -eq 0 -and $paneFlags.Output.Count -ge 1) {
                    $script:mousePaneFlags = $paneFlags.Output[0]
                }
                Start-Sleep -Milliseconds 500

                Write-ConsoleInputMouse -X 5 -Y 5 -ButtonState 1
                Start-Sleep -Milliseconds 750

                if (Test-Path -LiteralPath $mouseResult -PathType Leaf) {
                    $script:mouseResultBytes = [System.IO.File]::ReadAllBytes($mouseResult)
                    $script:mouseResultText = [System.Text.Encoding]::ASCII.GetString($script:mouseResultBytes)
                }
                if (Wait-ConsoleVisibleText -Needle $script:mouseCapturedMarker -TimeoutMs 250) {
                    $script:mouseCapturedVisible = $true
                }

                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseCtrlJ) {
            Write-Host "Relay Ctrl-J exercise is enabled. A physical-style Ctrl+J console key record will be injected."
            Write-Host "Logs will be checked afterward: $root"
            Clear-Host
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 15000 -AfterStart {
                param([int]$AttachPid)

                if (-not (Wait-ConsoleVisibleText -Needle $script:ctrlJReadyMarker -TimeoutMs 5000)) {
                    throw "timed out waiting for Ctrl-J helper readiness marker"
                }
                $script:ctrlJReadyVisible = $true

                Write-ConsoleInputCtrlJ
                Start-Sleep -Milliseconds 500
                if (Test-Path -LiteralPath $ctrlJResult -PathType Leaf) {
                    $script:ctrlJResultBytes = [System.IO.File]::ReadAllBytes($ctrlJResult)
                }
                if (Wait-ConsoleVisibleText -Needle $script:ctrlJCapturedMarker -TimeoutMs 250) {
                    $script:ctrlJCapturedVisible = $true
                }
                if (Test-Path -LiteralPath $ctrlJResult -PathType Leaf) {
                    $script:ctrlJResultBytes = [System.IO.File]::ReadAllBytes($ctrlJResult)
                }

                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseInputCredit) {
            Write-Host "Relay input-credit exercise is enabled. Large native console input will be injected and the attach client will be detached automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 25000 -AfterStart {
                param([int]$AttachPid)

                $builder = [System.Text.StringBuilder]::new()
                $line = "relay-input-credit-" + [string]::new([char]'x', 48)

                Start-Sleep -Milliseconds 750
                for ($i = 0; $i -lt 1100; $i++) {
                    [void]$builder.Append($line).Append("`r")
                }
                Write-ConsoleInputText -Text $builder.ToString() -ChunkSize 4096 -ChunkDelayMs 0
                Start-Sleep -Milliseconds 1500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseUtf8Split) {
            Write-Host "Relay UTF-8 split exercise is enabled. A multibyte character will be split across relay output messages and the visible console buffer will be checked."
            Write-Host "Logs will be checked afterward: $root"
            Clear-Host
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 15000 -AfterStart {
                param([int]$AttachPid)

                if (-not (Wait-ConsoleVisibleText -Needle $script:utf8SplitMarker -TimeoutMs 5000)) {
                    throw "timed out waiting for UTF-8 split marker in visible console output"
                }
                $script:utf8SplitVisible = $true
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseInvalidUtf8) {
            Write-Host "Relay invalid UTF-8 exercise is enabled. The console writer should log the boundary bug, sanitize the invalid bytes, and keep the relay attached."
            Write-Host "Logs will be checked afterward: $root"
            Clear-Host
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 15000 -AfterStart {
                param([int]$AttachPid)

                if (-not (Wait-ConsoleVisibleText -Needle $script:invalidUtf8Prefix -TimeoutMs 5000)) {
                    throw "timed out waiting for invalid UTF-8 output in visible console output"
                }
                $script:invalidUtf8PrefixVisible = $true
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($SimulateOutputLoss) {
            Write-Host "Relay output loss is being simulated. Attach should fail automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 10000
        } elseif ($SimulateTransportLost) {
            Write-Host "Relay transport loss is being simulated while output is in flight. Attach should fail automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 15000 -AfterStart {
                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,6000) do @echo relay-transport-loss-0123456789abcdefghijklmnopqrstuvwxyz"
                ) | Out-Null
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "Enter"
                ) | Out-Null
            }
        } elseif ($ExerciseDetachBacklog) {
            Write-Host "Relay detach-under-backlog exercise is enabled. Output will be generated and detach will happen while relay output is still pending."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 30000 -AfterStart {
                param([int]$AttachPid)

                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,20000) do @echo relay-detach-backlog-0123456789abcdefghijklmnopqrstuvwxyz"
                ) | Out-Null
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "Enter"
                ) | Out-Null
                Start-Sleep -Milliseconds 150
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseOutputProgress) {
            Write-Host "Relay output progress exercise is enabled. Output will be generated and the attach client will be detached automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 20000 -AfterStart {
                param([int]$AttachPid)

                $baselineLogs = Get-CurrentTmuxLogs -Path $root
                $baselineProgress = Get-LogMatchCount $baselineLogs "client_win32_output_progress: progressed"
                $baselineStatusRedraw = Get-LogMatchCount $baselineLogs "redraw status"
                $baselineThresholdDeferred = Get-LogMatchCount $baselineLogs "Win32 output bytes >"

                Start-Sleep -Milliseconds 750
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,300) do @echo relay-progress-light-0123456789abcdefghijklmnopqrstuvwxyz"
                ) | Out-Null
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "Enter"
                ) | Out-Null
                Start-Sleep -Milliseconds 100
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "set-option",
                    "-g",
                    "status-left",
                    "relay-output-progress-light"
                ) | Out-Null
                Start-Sleep -Milliseconds 1250

                $lightLogs = Get-CurrentTmuxLogs -Path $root
                $lightProgress = Get-LogMatchCount $lightLogs "client_win32_output_progress: progressed"
                $lightStatusRedraw = Get-LogMatchCount $lightLogs "redraw status"
                $lightThresholdDeferred = Get-LogMatchCount $lightLogs "Win32 output bytes >"
                $script:outputProgressSmallProgressCount =
                    $lightProgress - $baselineProgress
                $script:outputProgressSmallStatusRedrawCount =
                    $lightStatusRedraw - $baselineStatusRedraw
                $script:outputProgressSmallThresholdDeferredCount =
                    $lightThresholdDeferred - $baselineThresholdDeferred

                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,20000) do @echo relay-progress-0123456789abcdefghijklmnopqrstuvwxyz"
                ) | Out-Null
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "Enter"
                ) | Out-Null
                Start-Sleep -Milliseconds 100
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "set-option",
                    "-g",
                    "status-left",
                    "relay-output-progress-backlog"
                ) | Out-Null
                Start-Sleep -Milliseconds 1500

                $heavyLogs = Get-CurrentTmuxLogs -Path $root
                $heavyProgress = Get-LogMatchCount $heavyLogs "client_win32_output_progress: progressed"
                $heavyStatusRedraw = Get-LogMatchCount $heavyLogs "redraw status"
                $heavyThresholdDeferred = Get-LogMatchCount $heavyLogs "Win32 output bytes >"
                $script:outputProgressLargeProgressCount =
                    $heavyProgress - $lightProgress
                $script:outputProgressLargeStatusRedrawCount =
                    $heavyStatusRedraw - $lightStatusRedraw
                $script:outputProgressLargeThresholdDeferredCount =
                    $heavyThresholdDeferred - $lightThresholdDeferred

                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } elseif ($ExerciseResizeBacklog) {
            Write-Host "Relay resize-under-backlog exercise is enabled. Output will be generated, the real console will be resized, and the attach client will be detached automatically."
            Write-Host "Logs will be checked afterward: $root"
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs 30000 -AfterStart {
                param([int]$AttachPid)

                Start-Sleep -Milliseconds 750
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "-l",
                    "for /L %i in (1,1,20000) do @echo relay-resize-backlog-0123456789abcdefghijklmnopqrstuvwxyz"
                ) | Out-Null
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "send-keys",
                    "-t",
                    "relay",
                    "Enter"
                ) | Out-Null
                Start-Sleep -Milliseconds 100
                $script:restoreConsoleSize = Get-ConsoleWindowSizeNative
                foreach ($candidate in (
                    Get-AlternateConsoleWindowSizeCandidates `
                        -Width $script:restoreConsoleSize.Width `
                        -Height $script:restoreConsoleSize.Height
                )) {
                    $observed = Invoke-ObservedConsoleResize `
                        -OriginalWidth $script:restoreConsoleSize.Width `
                        -OriginalHeight $script:restoreConsoleSize.Height `
                        -RequestedWidth $candidate.Width `
                        -RequestedHeight $candidate.Height `
                        -TimeoutMs 2000
                    if ($observed -ne $null) {
                        $script:resizeTarget = $observed
                        $script:resizeConsoleObserved = $observed
                        break
                    }
                }
                if ($script:resizeConsoleObserved -eq $null) {
                    throw "Timed out waiting for native console resize away from $($script:restoreConsoleSize.Width)x$($script:restoreConsoleSize.Height)"
                }
                $clientResizePattern = (
                    "console size is now " +
                    "$($script:resizeTarget.Width)x$($script:resizeTarget.Height)"
                )
                $serverResizePattern = (
                    "server_client_win32_resize: client-$AttachPid now " +
                    "$($script:resizeTarget.Width)x$($script:resizeTarget.Height)"
                )
                if (-not (Wait-LogMatch -Path $root `
                    -Pattern $clientResizePattern -TimeoutMs 5000)) {
                    throw "Timed out waiting for client resize log: $clientResizePattern"
                }
                if (-not (Wait-LogMatch -Path $root `
                    -Pattern $serverResizePattern -TimeoutMs 5000)) {
                    throw "Timed out waiting for server resize log: $serverResizePattern"
                }
                $script:resizeObserved = Wait-TmuxClientSize `
                    -Config $config `
                    -Label $label `
                    -ClientPid $AttachPid `
                    -Width $script:resizeTarget.Width `
                    -Height $script:resizeTarget.Height `
                    -TimeoutMs 5000
                Start-Sleep -Milliseconds 500
                Invoke-Tmux -Arguments @(
                    "-f",
                    $config,
                    "-L",
                    $label,
                    "detach-client",
                    "-t",
                    "client-$AttachPid"
                ) | Out-Null
            }
        } else {
            Write-Host "Press Ctrl-b then d to detach. Logs will be checked afterward: $root"
            $attachTimeoutMs = 0
            if ($AutoDetachAfterMs -gt 0) {
                $attachTimeoutMs = $AutoDetachAfterMs + 15000
            }
            $attach = Invoke-TmuxInteractive -Arguments @(
                "-f",
                $config,
                "-vv",
                "-L",
                $label,
                "attach-session",
                "-t",
                "relay"
            ) -TimeoutMs $attachTimeoutMs -AfterStart {
                param([int]$AttachPid)

                if ($AutoDetachAfterMs -gt 0) {
                    Start-Sleep -Milliseconds $AutoDetachAfterMs
                    Invoke-Tmux -Arguments @(
                        "-f",
                        $config,
                        "-L",
                        $label,
                        "detach-client",
                        "-t",
                        "client-$AttachPid"
                    ) | Out-Null
                }
            }
        }

        $attachCode = $attach.ExitCode
        $attachPid = $attach.ProcessId
        if ($ExerciseCtrlJ -and $ctrlJResult -ne $null -and
            (Test-Path -LiteralPath $ctrlJResult -PathType Leaf)) {
            $ctrlJResultBytes = [System.IO.File]::ReadAllBytes($ctrlJResult)
        }
        if ($ExerciseUtf8Split -or $ExerciseInvalidUtf8) {
            Start-Sleep -Milliseconds 200
            $visibleText = Get-ConsoleVisibleText
            if ($ExerciseUtf8Split) {
                $utf8SplitVisible = $utf8SplitVisible -or
                    $visibleText.Contains($utf8SplitMarker)
            } else {
                $invalidUtf8PrefixVisible = $invalidUtf8PrefixVisible -or
                    $visibleText.Contains($invalidUtf8Prefix)
            }
        }
        Invoke-Tmux -Arguments @("-f", $config, "-L", $label, "kill-server") -AllowFailure | Out-Null
    } finally {
        Pop-Location
    }

    $logs = @(Get-ChildItem -LiteralPath $root -Filter "tmux-*.log" -File)
    $attachLogs = @()
    $attachLogPath = Join-Path $root ("tmux-client-{0}.log" -f $attachPid)
    if (Test-Path -LiteralPath $attachLogPath -PathType Leaf) {
        $attachLogs = @([System.IO.FileInfo](Get-Item -LiteralPath $attachLogPath))
    }
    if ($attachLogs.Count -eq 0) {
        $attachLogs = $logs
    }

    $relayMode = Test-AnyLogMatch $attachLogs "using Win32 console relay (terminal transport|fallback)"
    $relayIdentify = Test-AnyLogMatch $logs "IDENTIFY_WIN32_TERMINAL"
    $inputCredit = Test-AnyLogMatch $attachLogs "Win32 input credit"
    $directOutput = Test-AnyLogMatch $attachLogs "using direct Win32 terminal output|IDENTIFY_WIN32_STDOUT duplicated|IDENTIFY_WIN32_STDIN duplicated"
    $outputAbort = Test-AnyLogMatch $logs "Win32 output abort|dropping [0-9]+ pending bytes|simulating Win32 console relay output loss"
    $transportLost = Test-AnyLogMatch $logs "relay transport lost|simulating Win32 console relay transport loss|transport-lost"
    $closePending = Test-AnyLogMatch $logs "Win32 relay close pending"
    $inputPauseCount = Get-LogMatchCount $attachLogs "console input paused \((credit exhausted|[0-9]+ reserved bytes pending send)"
    $inputResumeCount = Get-LogMatchCount $attachLogs "console input resumed"
    $inputReturnedCount = Get-LogMatchCount $logs "Win32 input credit returned"
    $inputPeakReserved = Get-LogMaximumCapture $attachLogs "console input peak reserved ([0-9]+) bytes, peak reader buffered [0-9]+ bytes"
    $inputPeakBuffered = Get-LogMaximumCapture $attachLogs "console input peak reserved [0-9]+ bytes, peak reader buffered ([0-9]+) bytes"
    $outputProgressCount = Get-LogMatchCount $attachLogs "client_win32_output_progress: progressed"
    $redrawDeferredCount = Get-LogMatchCount $logs "redraw deferred"
    $waitingForRedrawCount = Get-LogMatchCount $logs "waiting for redraw, [0-9]+ bytes left"
    $statusRedrawCount = Get-LogMatchCount $logs "redraw status"
    $statusRedraw = $statusRedrawCount -ge 1
    $win32ThresholdDeferredCount = Get-LogMatchCount $logs "Win32 output bytes >"
    $utf8SplitCarryLogged = Test-AnyLogMatch $attachLogs `
        "preserving [0-9]+ trailing UTF-8 bytes|resuming with [0-9]+ carried UTF-8 bytes"
    $invalidUtf8Failure = Test-AnyLogMatch $attachLogs "MultiByteToWideChar failed"
    $relayMouseState = Test-AnyLogMatch $attachLogs "relay mouse mode 0x"
    $mouseSgrLogged = Test-AnyLogMatch $logs "mouse input"
    $mousePaneBinding = Test-AnyLogMatch $logs "key MouseDown1Pane: send-keys -M|writing key 0x400000100 \\(MouseDown1Pane\\) to %"
    $ctrlJTranslated = Test-AnyLogMatch $attachLogs "translated Ctrl-J key record to LF"
    $ctrlJPaneWrite = Test-AnyLogMatch $logs "writing key .*C-j.* to %"
    $mouseSequencePress = $false
    if ($ExerciseMouse -and $mouseResultBytes.Length -ne 0) {
        $mouseSequencePress =
            $mouseResultText.Contains(([string][char]27) + "[<0;")
    }
    $mouseSyntheticInjectionObserved = $mouseCapturedVisible -or
        $mouseSgrLogged -or $mousePaneBinding -or $mouseSequencePress
    $ctrlJByteCaptured = $ctrlJResultBytes -contains [byte]10
    $resizeClientLog = $false
    $resizeServerLog = $false
    if ($ExerciseResizeBacklog -and $resizeTarget -ne $null) {
        $resizeClientLog = Test-AnyLogMatch $attachLogs (
            "console size is now $($resizeTarget.Width)x$($resizeTarget.Height)"
        )
        $resizeServerLog = Test-AnyLogMatch $logs (
            "server_client_win32_resize: client-$attachPid now " +
            "$($resizeTarget.Width)x$($resizeTarget.Height)"
        )
    }
    $failures = @(Get-LogMatches $logs "rejected|ReadFile failed|WriteFile failed|output error")

    Write-Host ""
    Write-Host "Smoke summary:"
    Write-Host "  attach exit code: $attachCode"
    Write-Host "  relay terminal mode selected: $relayMode"
    Write-Host "  relay terminal identify: $relayIdentify"
    Write-Host "  input credit observed: $inputCredit"
    Write-Host "  direct handle path used: $directOutput"
    if ($ExerciseInputCredit) {
        Write-Host "  input pause events: $inputPauseCount"
        Write-Host "  input resume events: $inputResumeCount"
        Write-Host "  returned credit events: $inputReturnedCount"
        Write-Host "  peak reserved bytes: $inputPeakReserved"
        Write-Host "  peak reader buffered bytes: $inputPeakBuffered"
    }
    if ($ExerciseCtrlJ) {
        Write-Host "  Ctrl-J helper ready: $ctrlJReadyVisible"
        Write-Host "  Ctrl-J key record translated: $ctrlJTranslated"
        Write-Host "  Ctrl-J pane write logged: $ctrlJPaneWrite"
        Write-Host "  Ctrl-J captured marker visible: $ctrlJCapturedVisible"
        Write-Host "  Ctrl-J LF byte captured: $ctrlJByteCaptured"
    }
    if ($ExerciseUtf8Split) {
        Write-Host "  UTF-8 split marker visible: $utf8SplitVisible"
        Write-Host "  UTF-8 carry logged: $utf8SplitCarryLogged"
    }
    if ($ExerciseInvalidUtf8) {
        Write-Host "  invalid UTF-8 prefix visible: $invalidUtf8PrefixVisible"
        Write-Host "  invalid UTF-8 failure logged: $invalidUtf8Failure"
        Write-Host "  output abort observed: $outputAbort"
        Write-Host "  transport lost observed: $transportLost"
    }
    if ($ExerciseMouse) {
        Write-Host "  mouse ready marker visible: $mouseReadyVisible"
        Write-Host "  mouse captured marker visible: $mouseCapturedVisible"
        Write-Host "  pane mouse flags: $mousePaneFlags"
        Write-Host "  relay mouse state logged: $relayMouseState"
        Write-Host "  relay SGR mouse logged: $mouseSgrLogged"
        Write-Host "  pane mouse binding logged: $mousePaneBinding"
        Write-Host "  pane mouse press captured: $mouseSequencePress"
        Write-Host "  synthetic mouse injection observed: $mouseSyntheticInjectionObserved"
        if (-not $mouseSyntheticInjectionObserved) {
            Write-Host "  note: synthetic CONIN$ mouse records did not surface as VT bytes on this host; direct ReadFile mouse generation remains a manual or desktop-harness check."
        }
    }
    if ($SimulateTransportLost) {
        Write-Host "  transport lost observed: $transportLost"
    }
    if ($ExerciseDetachBacklog) {
        Write-Host "  close pending observed: $closePending"
    }
    if ($ExerciseOutputProgress) {
        Write-Host "  output progress events: $outputProgressCount"
        Write-Host "  redraw deferred events: $redrawDeferredCount"
        Write-Host "  Win32 threshold deferral events: $win32ThresholdDeferredCount"
        Write-Host "  waiting-for-redraw events: $waitingForRedrawCount"
        Write-Host "  status redraw events: $statusRedrawCount"
        Write-Host "  light-phase progress events: $outputProgressSmallProgressCount"
        Write-Host "  light-phase status redraw events: $outputProgressSmallStatusRedrawCount"
        Write-Host "  light-phase Win32 threshold deferrals: $outputProgressSmallThresholdDeferredCount"
        Write-Host "  heavy-phase progress events: $outputProgressLargeProgressCount"
        Write-Host "  heavy-phase status redraw events: $outputProgressLargeStatusRedrawCount"
        Write-Host "  heavy-phase Win32 threshold deferrals: $outputProgressLargeThresholdDeferredCount"
    }
    if ($ExerciseResizeBacklog) {
        if ($resizeTarget -ne $null) {
            Write-Host "  resize target: $($resizeTarget.Width)x$($resizeTarget.Height)"
        }
        if ($resizeConsoleObserved -ne $null) {
            Write-Host "  native console resize observed: $($resizeConsoleObserved.Width)x$($resizeConsoleObserved.Height)"
        } else {
            Write-Host "  native console resize observed: <none>"
        }
        if ($resizeObserved -ne $null) {
            Write-Host "  resize observed by server query: $($resizeObserved.Width)x$($resizeObserved.Height)"
        } else {
            Write-Host "  resize observed by server query: <none>"
        }
        Write-Host "  client resize log observed: $resizeClientLog"
        Write-Host "  server resize log observed: $resizeServerLog"
        Write-Host "  output progress events: $outputProgressCount"
        Write-Host "  redraw deferred events: $redrawDeferredCount"
    }
    if ($SimulateOutputLoss) {
        Write-Host "  output abort observed: $outputAbort"
    }
    Write-Host "  logs: $root"

    if ($failures.Count -ne 0) {
        Write-Host ""
        Write-Host "Failure lines:"
        $failures | ForEach-Object { Write-Host "  $_" }
    }

    if ($ExerciseMouse) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and
            $mouseReadyVisible -and $relayMouseState -and
            $failures.Count -eq 0
    } elseif ($ExerciseInputCredit) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $inputPauseCount -ge 1 -and
            $inputResumeCount -ge 2 -and $inputReturnedCount -ge 2 -and
            $inputPeakReserved -ge 0 -and $inputPeakReserved -le 65536 -and
            $inputPeakBuffered -ge 0 -and $inputPeakBuffered -le 65536 -and
            $failures.Count -eq 0
    } elseif ($ExerciseCtrlJ) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $ctrlJReadyVisible -and
            $ctrlJTranslated -and $ctrlJPaneWrite -and
            $failures.Count -eq 0
    } elseif ($ExerciseUtf8Split) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $utf8SplitVisible -and
            $utf8SplitCarryLogged -and
            -not $invalidUtf8Failure -and $failures.Count -eq 0
    } elseif ($ExerciseInvalidUtf8) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and
            $invalidUtf8PrefixVisible -and
            $invalidUtf8Failure -and -not $outputAbort -and
            -not $transportLost -and $failures.Count -eq 0
    } elseif ($SimulateOutputLoss) {
        $passed = $attachCode -ne 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $outputAbort -and
            $failures.Count -eq 0
    } elseif ($SimulateTransportLost) {
        $passed = $attachCode -ne 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $transportLost -and
            $failures.Count -eq 0
    } elseif ($ExerciseDetachBacklog) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $closePending -and
            $outputProgressCount -ge 1 -and $failures.Count -eq 0
    } elseif ($ExerciseOutputProgress) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
        $inputCredit -and -not $directOutput -and
            $outputProgressCount -ge 2 -and $redrawDeferredCount -ge 1 -and
            $win32ThresholdDeferredCount -ge 1 -and
            $waitingForRedrawCount -ge 1 -and $statusRedraw -and
            $outputProgressSmallProgressCount -ge 1 -and
            $outputProgressSmallStatusRedrawCount -ge 1 -and
            $outputProgressSmallThresholdDeferredCount -eq 0 -and
            $outputProgressLargeProgressCount -ge 1 -and
            $outputProgressLargeStatusRedrawCount -ge 1 -and
            $outputProgressLargeThresholdDeferredCount -ge 1 -and
            $failures.Count -eq 0
    } elseif ($ExerciseResizeBacklog) {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $resizeTarget -ne $null -and
            $resizeConsoleObserved -ne $null -and
            $resizeConsoleObserved.Width -eq $resizeTarget.Width -and
            $resizeConsoleObserved.Height -eq $resizeTarget.Height -and
            $resizeClientLog -and $resizeServerLog -and
            $outputProgressCount -ge 1 -and $redrawDeferredCount -ge 1 -and
            $failures.Count -eq 0
    } else {
        $passed = $attachCode -eq 0 -and $relayMode -and $relayIdentify -and
            $inputCredit -and -not $directOutput -and $failures.Count -eq 0
    }
    if ($passed) {
        Write-Host ""
        if ($ExerciseMouse) {
            Write-Host "Native-console relay mouse smoke passed."
        } elseif ($ExerciseInputCredit) {
            Write-Host "Native-console relay input-credit smoke passed."
        } elseif ($ExerciseCtrlJ) {
            Write-Host "Native-console relay Ctrl-J smoke passed."
        } elseif ($ExerciseUtf8Split) {
            Write-Host "Native-console relay UTF-8 split smoke passed."
        } elseif ($ExerciseInvalidUtf8) {
            Write-Host "Native-console relay invalid UTF-8 smoke passed."
        } elseif ($SimulateOutputLoss) {
            Write-Host "Native-console relay output-loss smoke passed."
        } elseif ($SimulateTransportLost) {
            Write-Host "Native-console relay transport-loss smoke passed."
        } elseif ($ExerciseDetachBacklog) {
            Write-Host "Native-console relay detach-backlog smoke passed."
        } elseif ($ExerciseOutputProgress) {
            Write-Host "Native-console relay output-progress smoke passed."
        } elseif ($ExerciseResizeBacklog) {
            Write-Host "Native-console relay resize-backlog smoke passed."
        } else {
            Write-Host "Native-console relay smoke passed."
        }
        Write-SmokeResult -Path $ResultPath -Result ([ordered]@{
            scenario = Get-SmokeScenarioName
            passed = $true
            attachExitCode = $attachCode
            relayMode = $relayMode
            relayIdentify = $relayIdentify
            inputCredit = $inputCredit
            directOutput = $directOutput
            outputAbort = $outputAbort
            transportLost = $transportLost
            closePending = $closePending
            inputPauseCount = $inputPauseCount
            inputResumeCount = $inputResumeCount
            inputReturnedCount = $inputReturnedCount
            inputPeakReserved = $inputPeakReserved
            inputPeakBuffered = $inputPeakBuffered
            ctrlJReadyVisible = $ctrlJReadyVisible
            ctrlJTranslated = $ctrlJTranslated
            ctrlJPaneWrite = $ctrlJPaneWrite
            ctrlJCapturedVisible = $ctrlJCapturedVisible
            ctrlJByteCaptured = $ctrlJByteCaptured
            outputProgressCount = $outputProgressCount
            redrawDeferredCount = $redrawDeferredCount
            waitingForRedrawCount = $waitingForRedrawCount
            statusRedrawCount = $statusRedrawCount
            outputProgressSmallProgressCount = $outputProgressSmallProgressCount
            outputProgressSmallStatusRedrawCount = $outputProgressSmallStatusRedrawCount
            outputProgressSmallThresholdDeferredCount = $outputProgressSmallThresholdDeferredCount
            outputProgressLargeProgressCount = $outputProgressLargeProgressCount
            outputProgressLargeStatusRedrawCount = $outputProgressLargeStatusRedrawCount
            outputProgressLargeThresholdDeferredCount = $outputProgressLargeThresholdDeferredCount
            utf8SplitVisible = $utf8SplitVisible
            utf8SplitCarryLogged = $utf8SplitCarryLogged
            invalidUtf8PrefixVisible = $invalidUtf8PrefixVisible
            invalidUtf8Failure = $invalidUtf8Failure
            mouseReadyVisible = $mouseReadyVisible
            mouseCapturedVisible = $mouseCapturedVisible
            mousePaneFlags = $mousePaneFlags
            relayMouseState = $relayMouseState
            mouseSgrLogged = $mouseSgrLogged
            mousePaneBinding = $mousePaneBinding
            mouseSequencePress = $mouseSequencePress
            mouseSyntheticInjectionObserved = $mouseSyntheticInjectionObserved
            resizeClientLog = $resizeClientLog
            resizeServerLog = $resizeServerLog
            resizeConsoleObserved = if ($resizeConsoleObserved -ne $null) {
                "{0}x{1}" -f $resizeConsoleObserved.Width, $resizeConsoleObserved.Height
            } else {
                $null
            }
            resizeTarget = if ($resizeTarget -ne $null) {
                "{0}x{1}" -f $resizeTarget.Width, $resizeTarget.Height
            } else {
                $null
            }
            resizeObserved = if ($resizeObserved -ne $null) {
                "{0}x{1}" -f $resizeObserved.Width, $resizeObserved.Height
            } else {
                $null
            }
            logs = $root
            failures = @($failures)
        })
        if ($RemoveLogsOnSuccess) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
        exit 0
    }

    Write-Host ""
    if ($ExerciseMouse) {
        Write-Host "Native-console relay mouse smoke failed."
    } elseif ($ExerciseInputCredit) {
        Write-Host "Native-console relay input-credit smoke failed."
    } elseif ($ExerciseCtrlJ) {
        Write-Host "Native-console relay Ctrl-J smoke failed."
    } elseif ($ExerciseUtf8Split) {
        Write-Host "Native-console relay UTF-8 split smoke failed."
    } elseif ($ExerciseInvalidUtf8) {
        Write-Host "Native-console relay invalid UTF-8 smoke failed."
    } elseif ($SimulateOutputLoss) {
        Write-Host "Native-console relay output-loss smoke failed."
    } elseif ($SimulateTransportLost) {
        Write-Host "Native-console relay transport-loss smoke failed."
    } elseif ($ExerciseDetachBacklog) {
        Write-Host "Native-console relay detach-backlog smoke failed."
    } elseif ($ExerciseOutputProgress) {
        Write-Host "Native-console relay output-progress smoke failed."
    } elseif ($ExerciseResizeBacklog) {
        Write-Host "Native-console relay resize-backlog smoke failed."
    } else {
        Write-Host "Native-console relay smoke failed."
    }
    Write-SmokeResult -Path $ResultPath -Result ([ordered]@{
        scenario = Get-SmokeScenarioName
        passed = $false
        attachExitCode = $attachCode
        relayMode = $relayMode
        relayIdentify = $relayIdentify
        inputCredit = $inputCredit
        directOutput = $directOutput
        outputAbort = $outputAbort
        transportLost = $transportLost
        closePending = $closePending
        inputPauseCount = $inputPauseCount
        inputResumeCount = $inputResumeCount
        inputReturnedCount = $inputReturnedCount
        inputPeakReserved = $inputPeakReserved
        inputPeakBuffered = $inputPeakBuffered
        ctrlJReadyVisible = $ctrlJReadyVisible
        ctrlJTranslated = $ctrlJTranslated
        ctrlJPaneWrite = $ctrlJPaneWrite
        ctrlJCapturedVisible = $ctrlJCapturedVisible
        ctrlJByteCaptured = $ctrlJByteCaptured
        outputProgressCount = $outputProgressCount
        redrawDeferredCount = $redrawDeferredCount
        waitingForRedrawCount = $waitingForRedrawCount
        statusRedrawCount = $statusRedrawCount
        outputProgressSmallProgressCount = $outputProgressSmallProgressCount
        outputProgressSmallStatusRedrawCount = $outputProgressSmallStatusRedrawCount
        outputProgressSmallThresholdDeferredCount = $outputProgressSmallThresholdDeferredCount
        outputProgressLargeProgressCount = $outputProgressLargeProgressCount
        outputProgressLargeStatusRedrawCount = $outputProgressLargeStatusRedrawCount
        outputProgressLargeThresholdDeferredCount = $outputProgressLargeThresholdDeferredCount
        utf8SplitVisible = $utf8SplitVisible
        utf8SplitCarryLogged = $utf8SplitCarryLogged
        invalidUtf8PrefixVisible = $invalidUtf8PrefixVisible
        invalidUtf8Failure = $invalidUtf8Failure
        mouseReadyVisible = $mouseReadyVisible
        mouseCapturedVisible = $mouseCapturedVisible
        mousePaneFlags = $mousePaneFlags
        relayMouseState = $relayMouseState
        mouseSgrLogged = $mouseSgrLogged
        mousePaneBinding = $mousePaneBinding
        mouseSequencePress = $mouseSequencePress
        mouseSyntheticInjectionObserved = $mouseSyntheticInjectionObserved
        resizeClientLog = $resizeClientLog
        resizeServerLog = $resizeServerLog
        resizeConsoleObserved = if ($resizeConsoleObserved -ne $null) {
            "{0}x{1}" -f $resizeConsoleObserved.Width, $resizeConsoleObserved.Height
        } else {
            $null
        }
        resizeTarget = if ($resizeTarget -ne $null) {
            "{0}x{1}" -f $resizeTarget.Width, $resizeTarget.Height
        } else {
            $null
        }
        resizeObserved = if ($resizeObserved -ne $null) {
            "{0}x{1}" -f $resizeObserved.Width, $resizeObserved.Height
        } else {
            $null
        }
        logs = $root
        failures = @($failures)
    })
    exit 1
} catch {
    Write-SmokeResult -Path $ResultPath -Result ([ordered]@{
        scenario = Get-SmokeScenarioName
        passed = $false
        error = $_.Exception.ToString()
        logs = if (Test-Path -LiteralPath $root) { $root } else { $null }
    })
    throw
} finally {
    if ($restoreConsoleSize -ne $null) {
        try {
            Set-ConsoleWindowSize -Width $restoreConsoleSize.Width `
                -Height $restoreConsoleSize.Height
        } catch {
            Write-Warning "Failed to restore console size to $($restoreConsoleSize.Width)x$($restoreConsoleSize.Height): $_"
        }
    }
    $env:TMUX = $savedEnv.TMUX
    $env:TMUX_WIN32_HANDLE_TTY = $savedEnv.TMUX_WIN32_HANDLE_TTY
    $env:TMUX_WIN32_CONSOLE_RELAY = $savedEnv.TMUX_WIN32_CONSOLE_RELAY
    $env:TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS = $savedEnv.TMUX_WIN32_CONSOLE_RELAY_TEST_OUTPUT_LOSS
    $env:TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST = $savedEnv.TMUX_WIN32_CONSOLE_RELAY_TEST_TRANSPORT_LOST
    $env:TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT = $savedEnv.TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT
    $env:TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8 = $savedEnv.TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8
}
