# Copies the Release build into the installed folder and adds Start menu and desktop shortcuts.
# The database (Sessions.db) in the installed folder is kept.
param(
    [string]$BuildRoot = $(if ($env:SESSIONS_BUILD_ROOT) { $env:SESSIONS_BUILD_ROOT } elseif (Test-Path 'D:\') { 'D:\Build\Sessions' } else { Join-Path $PSScriptRoot 'build' }),
    [string]$InstallFolder = 'D:\Apps\Sessions'
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $BuildRoot 'Release'
if (!(Test-Path (Join-Path $source 'Sessions.exe'))) { & (Join-Path $PSScriptRoot 'build.ps1') -BuildRoot $BuildRoot | Out-Null }
if (Get-Process Sessions -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallFolder*" }) { throw 'Close Sessions before installing.' }
New-Item -ItemType Directory -Force $InstallFolder | Out-Null
robocopy $source $InstallFolder /E /XF Sessions.db Sessions.db-wal Sessions.db-shm *.pdb *.ilk *.lib *.exp /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copy failed (robocopy exit $LASTEXITCODE)." }
$shell = New-Object -ComObject WScript.Shell
foreach ($folder in 'Programs', 'DesktopDirectory') {
    $shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath($folder)) 'Sessions.lnk'))
    $shortcut.TargetPath = Join-Path $InstallFolder 'Sessions.exe'
    $shortcut.WorkingDirectory = $InstallFolder
    $shortcut.Description = 'Find Claude and Codex sessions'
    $shortcut.Save()
}
Write-Output (Join-Path $InstallFolder 'Sessions.exe')
