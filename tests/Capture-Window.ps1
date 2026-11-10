# Saves a screenshot of a Sessions window (for checking the UI by eye):
# the main window by default, or another by its title ("Sessions search").
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputPath, [string]$WindowTitle = 'Sessions')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SessionsCapture {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
}
'@
[SessionsCapture]::SetProcessDPIAware() | Out-Null
$handle = [SessionsCapture]::FindWindow('WinUIDesktopWin32WindowClass', $WindowTitle)
if ($handle -eq [IntPtr]::Zero) { throw "No window titled '$WindowTitle' is open." }
$rect = New-Object SessionsCapture+RECT
[SessionsCapture]::GetWindowRect($handle, [ref]$rect) | Out-Null
$width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
# PW_RENDERFULLCONTENT captures the window itself, even when other windows cover it.
$hdc = $graphics.GetHdc()
[SessionsCapture]::PrintWindow($handle, $hdc, 2) | Out-Null
$graphics.ReleaseHdc($hdc)
$bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()
Write-Output $OutputPath
