# Presses a button in a Sessions window by its automation name or text.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Name, [string]$WindowTitle = 'Sessions')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SessionsButton {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
}
'@
$handle = [SessionsButton]::FindWindow('WinUIDesktopWin32WindowClass', $WindowTitle)
if ($handle -eq [IntPtr]::Zero) { throw "No window titled '$WindowTitle'." }
$root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
$condition = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::NameProperty), $Name
$button = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
if (-not $button) { throw "No element named '$Name'." }
$button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
"pressed $Name"
