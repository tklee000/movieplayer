param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$InputFile,
    [int]$SeekCount = 80,
    [int]$DelayMilliseconds = 350,
    [string]$ScreenshotPath = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "MoviePlayer executable not found: $Executable"
}
if (-not (Test-Path -LiteralPath $InputFile -PathType Leaf)) {
    throw "Media file not found: $InputFile"
}
if ($SeekCount -lt 1 -or $DelayMilliseconds -lt 0) {
    throw 'SeekCount must be positive and DelayMilliseconds cannot be negative.'
}

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class MoviePlayerSeekTestNative {
    public delegate bool EnumProc(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr window, uint message,
                                            IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr window, EnumProc callback,
                                               IntPtr parameter);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window,
                                                       out uint processId);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder text,
                                           int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder text,
                                          int capacity);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out RECT rectangle);

    public static string Text(IntPtr window) {
        var text = new StringBuilder(1024);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }

    public static string ClassName(IntPtr window) {
        var text = new StringBuilder(128);
        GetClassName(window, text, text.Capacity);
        return text.ToString();
    }

    public static IntPtr[] TopWindows(uint processId) {
        var windows = new List<IntPtr>();
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == processId && IsWindowVisible(window)) {
                windows.Add(window);
            }
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }

    public static string ChildTexts(IntPtr window) {
        var texts = new List<string>();
        EnumChildWindows(window, (child, parameter) => {
            string text = Text(child);
            if (text.Length != 0) texts.Add(text);
            return true;
        }, IntPtr.Zero);
        return String.Join(" | ", texts);
    }
}
'@

function Save-WindowScreenshot([IntPtr]$Window, [string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    $directory = Split-Path -Parent $Path
    if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
    $rectangle = New-Object MoviePlayerSeekTestNative+RECT
    if (-not [MoviePlayerSeekTestNative]::GetWindowRect(
            $Window, [ref]$rectangle)) {
        throw 'GetWindowRect failed while saving the test screenshot.'
    }
    $width = $rectangle.Right - $rectangle.Left
    $height = $rectangle.Bottom - $rectangle.Top
    $bitmap = New-Object Drawing.Bitmap $width, $height
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($rectangle.Left, $rectangle.Top, 0, 0,
                                     $bitmap.Size)
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
}

$process = Start-Process -FilePath $Executable `
    -ArgumentList ('"' + $InputFile + '"') -PassThru
$failure = $null
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $mainWindow = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline -and
           $mainWindow -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        $mainWindow = $process.MainWindowHandle
    }
    if ($mainWindow -eq [IntPtr]::Zero) { throw 'MoviePlayer main window not found.' }

    $seekBar = [MoviePlayerSeekTestNative]::GetDlgItem($mainWindow, 2007)
    $timeLabel = [MoviePlayerSeekTestNative]::GetDlgItem($mainWindow, 2009)
    if ($seekBar -eq [IntPtr]::Zero) { throw 'MoviePlayer seek bar not found.' }
    Write-Host "PID=$($process.Id) DPI=$([MoviePlayerSeekTestNative]::GetDpiForWindow($mainWindow))"
    Start-Sleep -Seconds 2

    # Stable LCG sequence. Slider position 1658 (attempt 15) is a known
    # open-GOP CRA/RASL regression point in Memories of Matsuko.
    [long]$state = 2463534242
    for ($attempt = 1; $attempt -le $SeekCount; ++$attempt) {
        $state = (1103515245L * $state + 12345L) % 2147483648L
        $position = 500 + [int]($state % 9001L)
        [void][MoviePlayerSeekTestNative]::SendMessage(
            $seekBar, 1029, [IntPtr]1, [IntPtr]$position) # TBM_SETPOS
        $scrollParameter = [IntPtr](($position -shl 16) -bor 4) # TB_THUMBPOSITION
        [void][MoviePlayerSeekTestNative]::SendMessage(
            $mainWindow, 0x0114, $scrollParameter, $seekBar) # WM_HSCROLL
        Start-Sleep -Milliseconds $DelayMilliseconds

        $dialogs = @([MoviePlayerSeekTestNative]::TopWindows(
                [uint32]$process.Id) | Where-Object {
                $_ -ne $mainWindow -and
                [MoviePlayerSeekTestNative]::ClassName($_) -eq '#32770'
            })
        if ($dialogs.Count -ne 0) {
            $dialog = $dialogs[0]
            $failure = "attempt=$attempt position=$position title='$([MoviePlayerSeekTestNative]::Text($dialog))' text='$([MoviePlayerSeekTestNative]::ChildTexts($dialog))'"
            break
        }
        if (($attempt % 10) -eq 0) {
            Write-Host "attempt=$attempt position=$position time='$([MoviePlayerSeekTestNative]::Text($timeLabel))'"
        }
    }

    Start-Sleep -Milliseconds 800
    Save-WindowScreenshot $mainWindow $ScreenshotPath
    if ($failure) { throw "Random seek failed: $failure" }
    Write-Host "PASS seeks=$SeekCount finalTime='$([MoviePlayerSeekTestNative]::Text($timeLabel))' screenshot='$ScreenshotPath'"
} finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
