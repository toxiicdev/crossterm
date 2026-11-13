# Builds CrossTerm on Windows (Release, MSVC), runs the tests and assembles a portable folder with windeployqt.
#
# Dependencies:
#   - Visual Studio 2022 or later with "Desktop development with C++" (includes CMake)
#   - Qt 6.5+ for MSVC 64-bit (Qt online installer)
#   - Optional: VcXsrv files in <output>\xserver for the integrated X server (see README)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Qt C:\Qt\6.8.3\msvc2022_64
# Parameters: -Qt <Qt msvc folder> (or set CMAKE_PREFIX_PATH), -Build <build dir>, -Output <portable folder>
param(
    [string]$Qt = $env:CMAKE_PREFIX_PATH,
    [string]$Build = "build",
    [string]$Output = "dist"
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not $Qt -or -not (Test-Path "$Qt\bin\windeployqt.exe")) {
    throw "Qt for MSVC not found: pass -Qt C:\Qt\<version>\msvc2022_64 or set CMAKE_PREFIX_PATH."
}

# CMake from PATH, otherwise the one shipped with Visual Studio.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
$ctest = Join-Path (Split-Path $cmake) "ctest.exe"

& $cmake -S . -B $Build -DCMAKE_PREFIX_PATH="$Qt"
if ($LASTEXITCODE) { throw "CMake configure failed." }
& $cmake --build $Build --config Release --parallel
if ($LASTEXITCODE) { throw "Build failed." }

$env:PATH = "$Qt\bin;$env:PATH"
$env:QT_QPA_PLATFORM = "offscreen"
& $ctest --test-dir $Build -C Release --output-on-failure
if ($LASTEXITCODE) { Write-Warning "Some tests failed (see above)." }
Remove-Item Env:QT_QPA_PLATFORM

New-Item -ItemType Directory -Force $Output | Out-Null
Copy-Item "$Build\Release\CrossTerm.exe" $Output -Force
& "$Qt\bin\windeployqt.exe" --release --no-translations "$Output\CrossTerm.exe"
if ($LASTEXITCODE) { throw "windeployqt failed." }
Write-Host "Built: $Output\CrossTerm.exe"
