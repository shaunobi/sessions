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
# Embedding model for meaning search: BGE small English v1.5 (MIT licence), pinned to one revision.
$models = Join-Path $BuildRoot 'models'
New-Item -ItemType Directory -Force $models | Out-Null
$revision = '5c38ec7c405ec4b44b94cc5a9bb96e735b38267a'
$modelFiles = @{
    'bge-small-en-v1.5.onnx'      = "https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/$revision/onnx/model.onnx"
    'bge-small-en-v1.5.vocab.txt' = "https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/$revision/vocab.txt"
}
foreach ($name in $modelFiles.Keys) {
    $target = Join-Path $models $name
    if (!(Test-Path $target)) { Invoke-WebRequest $modelFiles[$name] -OutFile $target }
}
# Meaning model shipped beside the executable (Model folder): Snowflake Arctic Embed M v1.5
# (Apache 2.0), chosen by scoring candidates on real searches (qa-meaning). The built-in
# BGE small model above is the fallback when the folder is missing.
$arctic = Join-Path $models 'snowflake-arctic-embed-m-v1.5'
$arcticRevision = 'e58a8f756156a1293d763f17e3aae643474e9b8a'
New-Item -ItemType Directory -Force $arctic | Out-Null
foreach ($file in @(@('model.onnx', 'onnx/model.onnx'), @('vocab.txt', 'vocab.txt'))) {
    $target = Join-Path $arctic $file[0]
    if (!(Test-Path $target)) { Invoke-WebRequest "https://huggingface.co/Snowflake/snowflake-arctic-embed-m-v1.5/resolve/$arcticRevision/$($file[1])" -OutFile $target }
}
# Tool logos for the cards come from the apps installed on this PC (not committed).
if (!(Test-Path (Join-Path $PSScriptRoot 'assets\logos\chatgpt-white.png')) -or !(Test-Path (Join-Path $PSScriptRoot 'assets\logos\gemini.png'))) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'tools\ExtractLogos.ps1')
}
& $nuget restore packages.config -PackagesDirectory $packages -Source 'https://api.nuget.org/v3/index.json' -NonInteractive
if ($LASTEXITCODE) { throw 'Package restore failed.' }
& (Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe') Sessions.vcxproj /m /v:minimal /p:Configuration=$Configuration /p:Platform=x64 "/p:SessionsBuildRoot=$BuildRoot"
if ($LASTEXITCODE) { throw 'C++ build failed.' }
$modelOut = Join-Path $BuildRoot "$Configuration\Model"
New-Item -ItemType Directory -Force $modelOut | Out-Null
robocopy $arctic $modelOut model.onnx vocab.txt /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Could not copy the meaning model.' }
$global:LASTEXITCODE = 0
Set-Content (Join-Path $modelOut 'name.txt') 'snowflake-arctic-embed-m-v1.5' -Encoding ascii
Write-Output (Join-Path $BuildRoot "$Configuration\Sessions.exe")
