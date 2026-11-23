# Configure, build and run unit tests inside a VS developer environment.
# Usage: scripts\build.ps1 [-Config Release|Debug] [-NoTest]
param([ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release', [switch]$NoTest)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH += ";$installer"  # VsDevCmd calls vswhere by name
$vswhere = "$installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ x64 toolset found.' }
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$build = Join-Path $root "build\$Config"
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $build
if ($LASTEXITCODE) { throw 'build failed' }
if (-not $NoTest) {
    ctest --test-dir $build --output-on-failure -LE integration  # integration tests open windows; run them explicitly
    if ($LASTEXITCODE) { throw 'tests failed' }
}
