# Lists the UI Automation elements of a Sessions window: a quick way to check
# what a window shows without a screenshot.
[CmdletBinding()]
param([string]$WindowTitle = 'Sessions search', [int]$First = 25)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SessionsInspect {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
}
'@
$handle = [SessionsInspect]::FindWindow('WinUIDesktopWin32WindowClass', $WindowTitle)
if ($handle -eq [IntPtr]::Zero) { throw "No window titled '$WindowTitle'." }
"visible: $([SessionsInspect]::IsWindowVisible($handle))"
$root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
$all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
"elements: $($all.Count)"
$all | Select-Object -First $First | ForEach-Object {
    '{0,-28} {1,-50} {2}' -f $_.Current.ControlType.ProgrammaticName, (($_.Current.Name -replace '\s+', ' ') + (' ' * 50)).Substring(0, 50).TrimEnd(), $_.Current.BoundingRectangle
}
