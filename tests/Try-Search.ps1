# Types a query into the running Sessions window through UI Automation, waits
# for results and saves a screenshot. Used to check search end to end.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Query, [Parameter(Mandatory = $true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$process = Get-Process Sessions | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw 'No Sessions window is open.' }
$root = [System.Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
$condition = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::NameProperty), 'Search sessions'
$box = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
if (-not $box) { throw 'Search box not found.' }
$box.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Query)
Start-Sleep -Milliseconds 1500
& (Join-Path $PSScriptRoot 'Capture-Window.ps1') -OutputPath $OutputPath
