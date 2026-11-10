# Opens the quick search (as a tray click would), optionally types a query,
# captures the screen area around the search panel, then closes the overlay.
[CmdletBinding()]
param([string]$Query = '', [Parameter(Mandatory = $true)][string]$OutputPath, [int]$WaitMilliseconds = 2500)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SessionsQuick {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
}
'@
[SessionsQuick]::SetProcessDPIAware() | Out-Null
& (Join-Path $PSScriptRoot 'Click-Tray.ps1') | Out-Null
Start-Sleep -Milliseconds 800
$handle = [SessionsQuick]::FindWindow('WinUIDesktopWin32WindowClass', 'Sessions search')
if ($handle -eq [IntPtr]::Zero) { throw 'The quick search did not open.' }
$root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
$condition = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::NameProperty), 'Find a conversation'
$box = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
if ($Query) { $box.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Query) }
Start-Sleep -Milliseconds $WaitMilliseconds
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen(0, 0, 0, 0, $bitmap.Size)
$bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()
& (Join-Path $PSScriptRoot 'Click-Tray.ps1') -Right | Out-Null   # opens the main window, which hides the overlay
Write-Output $OutputPath
