# Release package: dist\MissionControl-<version>-x64.zip with the app, the default config and docs.
# The exe uses the static CRT, so no redistributable is needed. Build first: scripts\build.ps1
param([string]$Build = "$PSScriptRoot\..\build\Release", [string]$Out = "$PSScriptRoot\..\dist")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path (Resolve-Path $Build) 'MissionControl.exe'
$version = (Get-Item $exe).VersionInfo.FileVersion
if (-not $version) { throw 'MissionControl.exe has no version resource' }
$name = "MissionControl-$version-x64"
$stage = Join-Path $Out $name
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $root 'resources\config.default.json') $stage
Copy-Item (Join-Path $root 'README.md') $stage
@"
Mission Control for Windows $version

Run MissionControl.exe. It stays in the background with an icon in the notification area
(click: open the overview; right-click: Start with Windows, Quit).
  Win+Tab          open / close the overview (Ctrl+Alt+Space always works too)
  MissionControl.exe --autostart on|off   start with Windows (per-user Run entry)
  MissionControl.exe --quit          stop it
  MissionControl.exe --no-takeover   run without taking over Win+Tab

Settings: copy config.default.json to %LOCALAPPDATA%\MissionControl\config.json and edit it.
"integration": { "takeoverWinTab": false } gives Win+Tab back to Windows Task View.
Nothing in Windows settings or the registry is changed; quitting the app restores everything.
Log and crash dumps: %LOCALAPPDATA%\MissionControl\
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'QUICKSTART.txt')
$zip = Join-Path $Out "$name.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-Item $zip | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } }
