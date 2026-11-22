# This file is part of the OSCQuery Automation Plugin —
# https://github.com/pob31/OSCQuery-Automation-plugin
#
# Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle
#
# The OSCQuery Automation Plugin is free software: you can redistribute it
# and/or modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version. It is distributed in the hope that it
# will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
# of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Windows release build, locally and in .github/workflows/release.yml alike (2026-11-22):
# configure the release-windows preset, build oqa_plugins (Release), stage what an installer
# ships, then compile the Inno Setup installer. Shape from WFS-DIY's Plugin/Scripts/build-all.ps1.
#
#   pwsh scripts/build-release.ps1                    build, stage, installer
#   pwsh scripts/build-release.ps1 -SkipInstaller     build and stage only
#   pwsh scripts/build-release.ps1 -Install           also copy into the per-user plugin folders
#   pwsh scripts/build-release.ps1 -SkipBuild ...     reuse the last build
#
# Produces:
#   build/stage/windows/             VST3/  CLAP/  Core/  Docs/   (the installer's input)
#   build/installer/OSCQuery-Automation-<version>-Windows-x64-Setup.exe
#
# -Install needs no administrator: the bundles go to %LOCALAPPDATA%\Programs\Common\VST3 and
# \CLAP, with the core library beside each, where the registry loader finds it by walking up
# from the plugin's own module. Both copies answer to one base name, so a VST3 and a CLAP in one
# host still share one registry. The installer instead puts one copy at the loader's fixed path,
# %CommonProgramFiles%\Pix et Bel\OSCQuery Automation.
#
# Needs: CMake (on PATH, or Visual Studio's own copy, found through vswhere), Visual Studio
# 2022 or later, Inno Setup 6 for the installer (winget install JRSoftware.InnoSetup).

param(
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [switch]$Install
)

$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Preset     = 'release-windows'
$BuildDir   = Join-Path $RepoRoot "build/$Preset"
$StageDir   = Join-Path $RepoRoot 'build/stage/windows'
$OutputDir  = Join-Path $RepoRoot 'build/installer'
$CoreName   = 'OSCQuery-Automation-Core.dll'
$ClapName   = 'OSCQuery Automation.clap'

function Find-Tool ([string]$name, [string[]]$fallbacks) {
    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($f in $fallbacks) { if ($f -and (Test-Path $f)) { return $f } }
    return $null
}

function Find-VsCMake {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $found = & $vswhere -latest -prerelease -products * `
        -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    return ($found | Select-Object -First 1)
}

function Invoke-Native ([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed with exit code $LASTEXITCODE" }
}

# The version has one home, project() in the root CMakeLists.txt.
$Version = [regex]::Match((Get-Content -Raw (Join-Path $RepoRoot 'CMakeLists.txt')),
    'project\(oqa\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if (-not $Version) { throw 'Could not read VERSION from project() in CMakeLists.txt' }
Write-Host "==> OSCQuery Automation $Version"

if (-not $SkipBuild) {
    $cmake = Find-Tool 'cmake' @((Find-VsCMake))
    if (-not $cmake) { throw 'CMake not found on PATH nor in a Visual Studio installation' }

    Push-Location $RepoRoot
    try {
        Write-Host "==> Configure ($Preset)"
        Invoke-Native $cmake @('--preset', $Preset)
        Write-Host '==> Build oqa_plugins (Release)'
        Invoke-Native $cmake @('--build', '--preset', $Preset, '--parallel')
    } finally {
        Pop-Location
    }
}

# --- Stage ------------------------------------------------------------------------------------

Write-Host "==> Stage into $StageDir"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
foreach ($d in 'VST3', 'CLAP', 'Core', 'Docs') {
    New-Item -ItemType Directory -Force (Join-Path $StageDir $d) | Out-Null
}

$vst3 = @(Get-ChildItem -Directory (Join-Path $BuildDir 'src/shims/juce') -Filter 'oqa_plugin_*_artefacts' |
    ForEach-Object { Get-ChildItem -Directory (Join-Path $_.FullName 'Release/VST3') -Filter '*.vst3' -ErrorAction SilentlyContinue })
if ($vst3.Count -eq 0) { throw "No VST3 bundle under $BuildDir/src/shims/juce; build first" }
foreach ($b in $vst3) {
    Copy-Item -Recurse $b.FullName (Join-Path $StageDir 'VST3')
    Write-Host "    VST3  $($b.Name)"
}

$clap = Join-Path $BuildDir "src/shims/clap/oqa_clap_artefacts/Release/$ClapName"
if (-not (Test-Path $clap)) { throw "No CLAP module at $clap; build first" }
Copy-Item $clap (Join-Path $StageDir 'CLAP')
Write-Host "    CLAP  $ClapName"

$core = Join-Path $BuildDir "src/core/core-shared/Release/$CoreName"
if (-not (Test-Path $core)) { throw "No core library at $core; build first" }
Copy-Item $core (Join-Path $StageDir 'Core')
Write-Host "    Core  $CoreName"

# GPLv3: the licence travels with the binaries. .txt, so Notepad opens them on a double-click.
Copy-Item (Join-Path $RepoRoot 'LICENSE')                (Join-Path $StageDir 'Docs/LICENSE.txt')
Copy-Item (Join-Path $RepoRoot 'README.md')              (Join-Path $StageDir 'Docs/README.txt')
Copy-Item (Join-Path $RepoRoot 'THIRD_PARTY_NOTICES.md') (Join-Path $StageDir 'Docs/THIRD_PARTY_NOTICES.txt')

# --- Per-user install, for a host on this machine ---------------------------------------------

if ($Install) {
    $common   = Join-Path $env:LOCALAPPDATA 'Programs\Common'
    $vst3Dest = Join-Path $common 'VST3'
    $clapDest = Join-Path $common 'CLAP'
    Write-Host "==> Install for this user: $vst3Dest, $clapDest"
    New-Item -ItemType Directory -Force $vst3Dest, $clapDest | Out-Null
    try {
        foreach ($b in Get-ChildItem -Directory (Join-Path $StageDir 'VST3')) {
            $target = Join-Path $vst3Dest $b.Name
            if (Test-Path $target) { Remove-Item -Recurse -Force $target }
            Copy-Item -Recurse $b.FullName $vst3Dest
        }
        Copy-Item -Force (Join-Path $StageDir "CLAP/$ClapName") $clapDest
        Copy-Item -Force (Join-Path $StageDir "Core/$CoreName") $vst3Dest
        Copy-Item -Force (Join-Path $StageDir "Core/$CoreName") $clapDest
    } catch {
        throw "Install failed (is a host still holding the plugin open?): $_"
    }
    Write-Host '    Done; rescan plugins in the host.'
}

# --- Installer --------------------------------------------------------------------------------

if ($SkipInstaller) {
    Write-Host 'Installer skipped.'
    exit 0
}

$iscc = Find-Tool 'iscc' @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))
if (-not $iscc) { throw 'ISCC.exe not found: install Inno Setup 6, or pass -SkipInstaller' }

New-Item -ItemType Directory -Force $OutputDir | Out-Null
Write-Host '==> Inno Setup'
Invoke-Native $iscc @('/Q',
    "/DAppVersion=$Version",
    "/DStageDir=$StageDir",
    "/DOutputDir=$OutputDir",
    "/DRepoRoot=$RepoRoot",
    (Join-Path $RepoRoot 'installer/windows/OSCQuery-Automation.iss'))

$exe = Join-Path $OutputDir "OSCQuery-Automation-$Version-Windows-x64-Setup.exe"
if (-not (Test-Path $exe)) { throw "Inno Setup ran but $exe is missing" }
Write-Host "==> Produced $exe"
