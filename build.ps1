param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    # Packages and build output. Defaults to D:\Build\Sessions because C: is nearly full.
    [string]$BuildRoot = $(if ($env:SESSIONS_BUILD_ROOT) { $env:SESSIONS_BUILD_ROOT } elseif (Test-Path 'D:\') { 'D:\Build\Sessions' } else { Join-Path $PSScriptRoot 'build' })
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot).TrimEnd('\') + '\'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio 2022 with Desktop development with C++ and Windows SDK 26100.' }
$packages = Join-Path $BuildRoot 'packages'
$nuget = Join-Path $BuildRoot 'tools\nuget.exe'
if (!(Test-Path $nuget)) { New-Item -ItemType Directory -Force (Split-Path $nuget) | Out-Null; Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/v6.14.0/nuget.exe' -OutFile $nuget }
& $nuget restore packages.config -PackagesDirectory $packages -Source 'https://api.nuget.org/v3/index.json' -NonInteractive
if ($LASTEXITCODE) { throw 'Package restore failed.' }
& (Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe') Sessions.vcxproj /m /v:minimal /p:Configuration=$Configuration /p:Platform=x64 "/p:SessionsBuildRoot=$BuildRoot"
if ($LASTEXITCODE) { throw 'C++ build failed.' }
Write-Output (Join-Path $BuildRoot "$Configuration\Sessions.exe")
