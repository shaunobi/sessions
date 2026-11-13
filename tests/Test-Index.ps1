# Builds a fresh index into a scratch database and checks that every source
# produced sessions and that search returns results. Never touches the
# installed database.
param(
    [string]$BuildRoot = $(if ($env:SESSIONS_BUILD_ROOT) { $env:SESSIONS_BUILD_ROOT } elseif (Test-Path 'D:\') { 'D:\Build\Sessions' } else { Join-Path $PSScriptRoot '..\build' }),
    [string]$Query = 'the'
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $BuildRoot 'Release\Sessions.exe'
if (!(Test-Path $exe)) { throw "Build first: $exe not found." }
$db = Join-Path $BuildRoot 'test-index.db'
Remove-Item "$db*" -ErrorAction SilentlyContinue
$output = Join-Path $BuildRoot 'test-index.txt'
$process = Start-Process $exe -ArgumentList '--index', '--db', "`"$db`"", '--search', "`"$Query`"" -RedirectStandardOutput $output -Wait -PassThru -NoNewWindow
$lines = Get-Content $output -Encoding UTF8 | Where-Object { $_ -notmatch '^Indexing \d+ of' }
$lines
$failures = @()
if ($process.ExitCode -ne 0) { $failures += "exit code $($process.ExitCode)" }
foreach ($tool in 'Claude app', 'Claude Code (terminal)', 'Codex app', 'Codex (terminal)') {
    $line = $lines | Where-Object { $_ -like "${tool}: *" }
    if (-not $line -or [int]($line -replace '.*: ', '') -le 0) { $failures += "no $tool sessions" }
}
if (-not ($lines | Where-Object { $_ -match '^search .*: [1-9]\d* sessions' })) { $failures += 'search returned nothing' }
Remove-Item "$db*" -ErrorAction SilentlyContinue
if ($failures) { throw ('FAILED: ' + ($failures -join '; ')) }
'PASSED'
