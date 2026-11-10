param([ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$BuildRoot = $(if ($env:SESSIONS_BUILD_ROOT) { $env:SESSIONS_BUILD_ROOT } elseif (Test-Path 'D:\') { 'D:\Build\Sessions' } else { Join-Path $PSScriptRoot 'build' }))
$appPath = Join-Path $BuildRoot "$Configuration\Sessions.exe"
if (!(Test-Path $appPath)) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -BuildRoot $BuildRoot | Out-Null }
Start-Process -FilePath $appPath -WorkingDirectory (Split-Path $appPath)
