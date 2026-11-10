[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$IntermediateDirectory,
    [ValidatePattern('^[A-Za-z0-9_.-]+$')][string]$IndexName = 'Sessions'
)

$ErrorActionPreference = 'Stop'
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$workRoot = [System.IO.Path]::GetFullPath($IntermediateDirectory)
$inputRoot = Join-Path $workRoot 'input'
New-Item -ItemType Directory -Path $inputRoot -Force | Out-Null

# The component indexes carry WinUI templates and localized strings. Without a
# merged application PRI, an unpackaged executable cannot resolve ms-appx URIs.
$componentNames = @(
    'Microsoft.UI.Xaml.Controls.pri',
    'Microsoft.UI.pri',
    'Microsoft.WindowsAppRuntime.pri'
)
foreach ($componentName in $componentNames) {
    $componentPath = Join-Path $outputRoot $componentName
    if (-not (Test-Path -LiteralPath $componentPath -PathType Leaf)) {
        throw "Required Windows App SDK resource index is missing: $componentPath"
    }
    Copy-Item -LiteralPath $componentPath -Destination (Join-Path $inputRoot $componentName) -Force
}

$toolCandidates = @()
if ($env:WindowsSdkVerBinPath) {
    $toolCandidates += Join-Path $env:WindowsSdkVerBinPath 'x64\makepri.exe'
}
$sdkBinRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
if (Test-Path -LiteralPath $sdkBinRoot) {
    $toolCandidates += Get-ChildItem -LiteralPath $sdkBinRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\makepri.exe' }
}
$makePriPath = $toolCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $makePriPath) {
    throw 'MakePRI was not found. Install the Windows SDK resource tools.'
}

$configPath = Join-Path $workRoot 'priconfig.xml'
$generatedPath = Join-Path $workRoot 'resources.pri'
$logPath = Join-Path $workRoot 'makepri.log'
# Omitting the packaging section keeps every language/scale in one local PRI.
@'
<?xml version="1.0" encoding="UTF-8"?>
<resources targetOsVersion="10.0.0" majorVersion="1">
  <index root="\" startIndexAt="\">
    <default>
      <qualifier name="Language" value="en-US"/>
      <qualifier name="Contrast" value="standard"/>
      <qualifier name="Scale" value="100"/>
      <qualifier name="HomeRegion" value="001"/>
      <qualifier name="TargetSize" value="256"/>
      <qualifier name="LayoutDirection" value="LTR"/>
      <qualifier name="Theme" value="dark"/>
      <qualifier name="AlternateForm" value=""/>
      <qualifier name="DXFeatureLevel" value="DX9"/>
      <qualifier name="Configuration" value=""/>
      <qualifier name="DeviceFamily" value="Universal"/>
      <qualifier name="Custom" value=""/>
    </default>
    <indexer-config type="folder" foldernameAsQualifier="true" filenameAsQualifier="true" qualifierDelimiter="."/>
    <indexer-config type="PRI"/>
  </index>
</resources>
'@ | Set-Content -LiteralPath $configPath -Encoding UTF8

$toolArguments = @('new', '/pr', $inputRoot, '/cf', $configPath,
    '/of', $generatedPath, '/in', $IndexName, '/o')
$toolOutput = & $makePriPath @toolArguments 2>&1
$toolExitCode = $LASTEXITCODE
$toolOutput | Set-Content -LiteralPath $logPath -Encoding Unicode
if ($toolExitCode -ne 0 -or -not (Test-Path -LiteralPath $generatedPath -PathType Leaf)) {
    throw "MakePRI failed with exit code $toolExitCode. See $logPath"
}
try {
    Copy-Item -LiteralPath $generatedPath -Destination (Join-Path $outputRoot 'resources.pri') -Force
} catch {
    throw "Could not replace resources.pri. Close Sessions before rebuilding. The generated resource index is preserved at $generatedPath."
}
Write-Host "Generated native WinUI resources.pri ($IndexName)."
