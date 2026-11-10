# Simulates a click on the Sessions tray icon (left by default, -Right for
# right click) by posting the notification the shell would send.
[CmdletBinding()]
param([switch]$Right)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SessionsTray {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int processId);
}
'@
$window = [SessionsTray]::FindWindow('SessionsTrayWindow', [NullString]::Value)
if ($window -eq [IntPtr]::Zero) { throw 'Sessions is not running.' }
[SessionsTray]::AllowSetForegroundWindow(-1) | Out-Null
$event = if ($Right) { 0x007B } else { 0x0400 }   # WM_CONTEXTMENU or NIN_SELECT
[SessionsTray]::PostMessage($window, 0x8000 + 41, [IntPtr]1, [IntPtr]$event) | Out-Null
'posted'
