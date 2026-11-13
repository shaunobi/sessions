# Copies the Release build into the installed folder and adds Start menu and desktop shortcuts.
# The database (Sessions.db) in the installed folder is kept.
param(
    [string]$BuildRoot = $(if ($env:SESSIONS_BUILD_ROOT) { $env:SESSIONS_BUILD_ROOT } elseif (Test-Path 'D:\') { 'D:\Build\Sessions' } else { Join-Path $PSScriptRoot 'build' }),
    [string]$InstallFolder = 'D:\Apps\Sessions'
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $BuildRoot 'Release'
if (!(Test-Path (Join-Path $source 'Sessions.exe'))) { & (Join-Path $PSScriptRoot 'build.ps1') -BuildRoot $BuildRoot | Out-Null }
$running = Get-Process Sessions -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallFolder*" }
if ($running) {
    # Ask it to close (it saves its window position and shuts the web browser down cleanly).
    # Killing it outright can damage the web sign-in profile.
    $running | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    $running | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue
    if (Get-Process Sessions -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$InstallFolder*" }) {
        throw 'Sessions is still running (it may be minimized to the tray). Quit it, then install again.'
    }
}
New-Item -ItemType Directory -Force $InstallFolder | Out-Null
# Mirror: files from older builds are removed. The database and the Browser folder (web sign-ins)
# are never touched: mirroring once deleted Browser on every install, signing you out of every site.
robocopy $source $InstallFolder /MIR /XD Browser Browser2 /XF Sessions.db Sessions.db-wal Sessions.db-shm *.pdb *.ilk *.lib *.exp /NFL /NDL /NJH /NJS /NP | Out-Null
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
