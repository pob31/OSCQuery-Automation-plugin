#!/usr/bin/env bash
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
# The macOS and Linux release build, locally and in .github/workflows/release.yml alike
# (2026-11-22): configure the release-macos or release-linux preset, build oqa_plugins
# (Release), stage what an installer ships, then make the .pkg (macOS) or the tarball (Linux).
# Shape from WFS-DIY's Plugin/Scripts/build-all.sh.
#
#   scripts/build-release.sh                     build, stage, installer
#   scripts/build-release.sh --skip-installer    build and stage only
#   scripts/build-release.sh --install           also copy into the per-user plugin folders
#   scripts/build-release.sh --skip-build ...    reuse the last build
#
# Produces:
#   build/stage/<os>/      VST3/  AU/ (macOS)  CLAP/  Core/  Docs/   (the installer's input)
#   build/installer/OSCQuery-Automation-<version>-macOS.pkg
#   build/installer/OSCQuery-Automation-<version>-Linux-<arch>.tar.gz
#
# On macOS the staged bundles are sealed ad hoc (Phase 1: a bundle a host loads needs a
# signature over the whole bundle). installer/macos/build-pkg.sh signs for real and notarises
# when its environment names an identity and notary credentials; without them the .pkg is
# unsigned, for this machine or the farm.
#
# --install needs no sudo: ~/Library/Audio/Plug-Ins/{VST3,Components,CLAP} on macOS, ~/.vst3
# and ~/.clap on Linux, with the core library beside each, where the registry loader finds it
# by walking up from the plugin's own module (one base name, so one registry per host).
#
# Needs: cmake 3.22+, ninja, a C++20 compiler; on Linux the JUCE packages ci.yml lists.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_BASENAME="OSCQuery-Automation-Core"
CLAP_NAME="OSCQuery Automation.clap"

SKIP_BUILD=0
SKIP_INSTALLER=0
INSTALL=0
for arg in "$@"; do
    case "$arg" in
        --skip-build)     SKIP_BUILD=1 ;;
        --skip-installer) SKIP_INSTALLER=1 ;;
        --install)        INSTALL=1 ;;
        -h|--help)        sed -n '17,40p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

case "$(uname -s)" in
    Darwin) OS=macos; CORE_FILE="lib${CORE_BASENAME}.dylib"
            export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH" ;;   # the farm's non-interactive PATH
    Linux)  OS=linux; CORE_FILE="lib${CORE_BASENAME}.so" ;;
    *) echo "build-release.sh is for macOS and Linux; on Windows use build-release.ps1" >&2; exit 1 ;;
esac

PRESET="release-$OS"
BUILD_DIR="$REPO_ROOT/build/$PRESET"
STAGE_DIR="$REPO_ROOT/build/stage/$OS"
OUTPUT_DIR="$REPO_ROOT/build/installer"

# The version has one home, project() in the root CMakeLists.txt.
VERSION="$(sed -nE 's/^project\(oqa[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "could not read VERSION from project() in CMakeLists.txt" >&2; exit 1; }
echo "==> OSCQuery Automation $VERSION ($OS)"

if [ "$SKIP_BUILD" = 0 ]; then
    cd "$REPO_ROOT"
    echo "==> Configure ($PRESET)"
    cmake --preset "$PRESET"
    echo "==> Build oqa_plugins (Release)"
    cmake --build --preset "$PRESET" --parallel
fi

# --- Stage ------------------------------------------------------------------------------------

echo "==> Stage into $STAGE_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/VST3" "$STAGE_DIR/CLAP" "$STAGE_DIR/Core" "$STAGE_DIR/Docs"
[ "$OS" = macos ] && mkdir -p "$STAGE_DIR/AU"

shopt -s nullglob
vst3_count=0
au_count=0
for artefacts in "$BUILD_DIR"/src/shims/juce/oqa_plugin_*_artefacts/Release; do
    for b in "$artefacts"/VST3/*.vst3; do
        cp -R "$b" "$STAGE_DIR/VST3/"; echo "    VST3  $(basename "$b")"; vst3_count=$((vst3_count + 1))
    done
    for b in "$artefacts"/AU/*.component; do
        cp -R "$b" "$STAGE_DIR/AU/";   echo "    AU    $(basename "$b")"; au_count=$((au_count + 1))
    done
done
shopt -u nullglob
[ "$vst3_count" -gt 0 ] || { echo "no VST3 bundle under $BUILD_DIR/src/shims/juce; build first" >&2; exit 1; }
if [ "$OS" = macos ] && [ "$au_count" -ne "$vst3_count" ]; then
    echo "$vst3_count VST3 bundles but $au_count AU" >&2; exit 1
fi

CLAP_SRC="$BUILD_DIR/src/shims/clap/oqa_clap_artefacts/Release/$CLAP_NAME"
[ -e "$CLAP_SRC" ] || { echo "no CLAP module at $CLAP_SRC; build first" >&2; exit 1; }
cp -R "$CLAP_SRC" "$STAGE_DIR/CLAP/"
echo "    CLAP  $CLAP_NAME"

CORE_SRC="$BUILD_DIR/src/core/core-shared/Release/$CORE_FILE"
[ -f "$CORE_SRC" ] || { echo "no core library at $CORE_SRC; build first" >&2; exit 1; }
cp "$CORE_SRC" "$STAGE_DIR/Core/"
echo "    Core  $CORE_FILE"

# GPLv3: the licence travels with the binaries.
cp "$REPO_ROOT/LICENSE"                "$STAGE_DIR/Docs/LICENSE.txt"
cp "$REPO_ROOT/README.md"              "$STAGE_DIR/Docs/README.txt"
cp "$REPO_ROOT/THIRD_PARTY_NOTICES.md" "$STAGE_DIR/Docs/THIRD_PARTY_NOTICES.txt"

if [ "$OS" = macos ]; then
    echo "==> Seal the staged bundles ad hoc"
    codesign --force --sign - "$STAGE_DIR/Core/$CORE_FILE"
    for b in "$STAGE_DIR"/VST3/*.vst3 "$STAGE_DIR"/AU/*.component "$STAGE_DIR/CLAP/$CLAP_NAME"; do
        codesign --force --deep --sign - "$b"
    done
fi

# --- Per-user install, for a host on this machine ---------------------------------------------

if [ "$INSTALL" = 1 ]; then
    if [ "$OS" = macos ]; then
        PLUGINS="$HOME/Library/Audio/Plug-Ins"
        echo "==> Install for this user: $PLUGINS/{VST3,Components,CLAP}"
        mkdir -p "$PLUGINS/VST3" "$PLUGINS/Components" "$PLUGINS/CLAP"
        for b in "$STAGE_DIR"/VST3/*.vst3;      do rm -rf "$PLUGINS/VST3/$(basename "$b")";       cp -R "$b" "$PLUGINS/VST3/"; done
        for b in "$STAGE_DIR"/AU/*.component;   do rm -rf "$PLUGINS/Components/$(basename "$b")"; cp -R "$b" "$PLUGINS/Components/"; done
        rm -rf "$PLUGINS/CLAP/$CLAP_NAME"
        cp -R "$STAGE_DIR/CLAP/$CLAP_NAME" "$PLUGINS/CLAP/"
        for d in VST3 Components CLAP; do cp "$STAGE_DIR/Core/$CORE_FILE" "$PLUGINS/$d/"; done
        # The AU registrar caches components; a new build is seen only once it restarts.
        killall -9 AudioComponentRegistrar 2>/dev/null || true
    else
        bash "$REPO_ROOT/installer/linux/install.sh" --user "$STAGE_DIR"
    fi
    echo "    Done; rescan plugins in the host."
fi

# --- Installer --------------------------------------------------------------------------------

if [ "$SKIP_INSTALLER" = 1 ]; then
    echo "Installer skipped."
    exit 0
fi

mkdir -p "$OUTPUT_DIR"
if [ "$OS" = macos ]; then
    bash "$REPO_ROOT/installer/macos/build-pkg.sh" "$STAGE_DIR" "$VERSION" "$OUTPUT_DIR"
else
    bash "$REPO_ROOT/installer/linux/build-tarball.sh" "$STAGE_DIR" "$VERSION" "$OUTPUT_DIR"
fi
