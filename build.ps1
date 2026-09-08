#Requires -Version 7.0
<#
.SYNOPSIS
    Configures and builds H4CN.asi (x86) with the Visual Studio toolchain.

.DESCRIPTION
    Wraps the three things that are easy to get wrong in this repo:
      1. The compiler must target x86 (heroes4.exe is 32-bit) and needs the VS
         developer environment for INCLUDE/LIB.
      2. `cmake` on PATH may not be usable: Strawberry Perl's cmake.exe dies with
         0xC000007B, so the VS-bundled cmake/ninja are prepended explicitly.
      3. Presets, not CMakeSettings.json, drive the configuration.

.PARAMETER Config
    Debug or Release. Selects the `x86-debug` / `x86-release` preset pair.

.PARAMETER DeployDir
    Game plugin directory, e.g. 'D:\Games\Heroes4\plugins'. Passed through as
    -DH4CN_DEPLOY_DIR so the build copies H4CN.asi there after linking. The Ultimate
    ASI Loader runs with LoadFromScriptsOnly=1, so plugins\ is the only scanned
    location - the game root is not.

.PARAMETER ModDir
    Heroes4GL mods directory, e.g. 'D:\Games\Heroes4\mods'. Passed through as
    -DH4CN_MOD_DIR so the build also copies the binary there as H4CN.mod, the
    extension Heroes4GL's mod loader scans for. Install one host copy only
    (plugins\H4CN.asi OR mods\H4CN.mod, not both).

.PARAMETER Clean
    Delete the preset's build tree before configuring.

.PARAMETER Test
    Also configure, build and run the host unit tests (wrap_tests console exe,
    pure line-breaking logic only - it is never part of the shipped .asi).

.EXAMPLE
    ./build.ps1 -Config Release
.EXAMPLE
    ./build.ps1 -Config Release -DeployDir 'D:\Games\Heroes4\plugins'
.EXAMPLE
    ./build.ps1 -Config Release -ModDir 'D:\Games\Heroes4\mods'
.EXAMPLE
    ./build.ps1 -Config Debug -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    [string]$DeployDir = '',

    [string]$ModDir = '',

    [switch]$Clean,

    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
# Sources, CMakeLists.txt and CMakePresets.json live in src/; the build tree stays
# in the repo-root out/ (see binaryDir '${sourceDir}/../out/build/${presetName}').
$src = Join-Path $root 'src'
if (-not (Test-Path -LiteralPath (Join-Path $src 'CMakePresets.json'))) {
    throw "CMakePresets.json not found under '$src'."
}

# --- Locate Visual Studio ---------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'; install Visual Studio 2022 or newer with the C++ workload."
}
$vs = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
if (-not $vs) {
    throw 'No Visual Studio installation with the MSVC C++ toolset was found.'
}

# --- Enter the x86 developer environment (cl/link + INCLUDE/LIB) ------------
$devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path -LiteralPath $devShell)) {
    throw "Microsoft.VisualStudio.DevShell.dll not found under '$vs'."
}
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x86 -host_arch=x64' | Out-Null
Set-Location -LiteralPath $root

# --- Prefer the VS-bundled cmake/ninja over anything already on PATH --------
$cmakeBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
foreach ($dir in @($cmakeBin, $ninjaBin)) {
    if (-not (Test-Path -LiteralPath $dir)) {
        throw "Missing '$dir'; install the 'C++ CMake tools for Windows' component."
    }
}
$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"

# --- Configure + build ------------------------------------------------------
# Presets are resolved from the directory containing CMakePresets.json (src/).
$preset = "x86-$($Config.ToLowerInvariant())"
$outDir = Join-Path $root "out\build\$preset"
if ($Clean) {
    Remove-Item -Recurse -Force -LiteralPath $outDir -ErrorAction SilentlyContinue
}

Push-Location -LiteralPath $src
try {
    $configureArgs = @('--preset', $preset)
    if ($DeployDir) {
        $configureArgs += "-DH4CN_DEPLOY_DIR=$DeployDir"
    }
    if ($ModDir) {
        $configureArgs += "-DH4CN_MOD_DIR=$ModDir"
    }
    if ($Test) {
        $configureArgs += '-DH4CN_BUILD_TESTS=ON'
    }

    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for preset '$preset' ($LASTEXITCODE)." }

    & cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed for preset '$preset' ($LASTEXITCODE)." }

    if ($Test) {
        & ctest --test-dir $outDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "wrap_tests failed ($LASTEXITCODE)." }
    }
} finally {
    Pop-Location
}

$asi = Join-Path $outDir 'H4CN.asi'
Write-Host "`nBuilt: $asi" -ForegroundColor Green
if ($DeployDir) {
    Write-Host "Deployed to: $DeployDir" -ForegroundColor Green
}
if ($ModDir) {
    Write-Host "Mod-deployed to: $ModDir" -ForegroundColor Green
}
