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
# The Linux install, shipped at the top of the tarball (2026-11-22).
#
#   ./install.sh --user      ~/.vst3 and ~/.clap, the core library beside each; no sudo
#   ./install.sh --system    /usr/local/lib/vst3 and /usr/lib/clap, the core library at
#                            /usr/local/lib/oscquery-automation/; sudo
#
# An optional second argument names the tree to install from (scripts/build-release.sh
# --install passes its stage); by default, the folder this script is in.
#
# The core library: the registry loader looks beside the plugin first (walking up from its own
# module), then at /usr/local/lib/oscquery-automation (src/core/registry/RegistryLoader.cpp).
# No per-user fixed path exists, so --user puts a copy beside each format; the loader asks the
# system for a library already loaded by that name first, so a VST3 and a CLAP in one host still
# share one registry. /usr/lib/clap and not /usr/local/lib/clap: the CLAP standard search path
# (clap/entry.h) has only the former.

set -euo pipefail

MODE="${1:-}"
FROM="${2:-$(cd "$(dirname "$0")" && pwd)}"
CORE_FILE="libOSCQuery-Automation-Core.so"
CLAP_NAME="OSCQuery Automation.clap"

if [ -z "$MODE" ] && [ -t 0 ]; then
    read -r -p "Install for this user only (~/.vst3, ~/.clap)? [Y/n] " answer
    case "${answer,,}" in n|no) MODE=--system ;; *) MODE=--user ;; esac
fi

case "$MODE" in
    --user)
        VST3_DIR="$HOME/.vst3"
        CLAP_DIR="$HOME/.clap"
        CORE_DIRS=("$VST3_DIR" "$CLAP_DIR")
        DOC_DIR=""
        SUDO="" ;;
    --system)
        VST3_DIR="/usr/local/lib/vst3"
        CLAP_DIR="/usr/lib/clap"
        CORE_DIRS=("/usr/local/lib/oscquery-automation")
        DOC_DIR="/usr/local/share/doc/oscquery-automation"
        SUDO="sudo"
        [ "$(id -u)" -eq 0 ] && SUDO="" ;;
    *)
        echo "usage: $0 --user | --system [from-dir]" >&2
        exit 2 ;;
esac

[ -f "$FROM/Core/$CORE_FILE" ] || { echo "no $CORE_FILE under $FROM/Core" >&2; exit 1; }

echo "==> Installing from $FROM"
echo "    VST3  $VST3_DIR"
echo "    CLAP  $CLAP_DIR"
echo "    core  ${CORE_DIRS[*]}"

$SUDO mkdir -p "$VST3_DIR" "$CLAP_DIR" "${CORE_DIRS[@]}"

shopt -s nullglob
for bundle in "$FROM"/VST3/*.vst3; do
    $SUDO rm -rf "$VST3_DIR/$(basename "$bundle")"
    $SUDO cp -R "$bundle" "$VST3_DIR/"
done
shopt -u nullglob

$SUDO install -m 0755 "$FROM/CLAP/$CLAP_NAME" "$CLAP_DIR/$CLAP_NAME"

for d in "${CORE_DIRS[@]}"; do
    $SUDO install -m 0755 "$FROM/Core/$CORE_FILE" "$d/$CORE_FILE"
done

if [ -n "$DOC_DIR" ] && [ -d "$FROM/Docs" ]; then
    $SUDO mkdir -p "$DOC_DIR"
    $SUDO cp "$FROM"/Docs/*.txt "$DOC_DIR/"
fi

echo
echo "Done. Rescan plugins in the host: OSCQuery Automation, by Pix et Bel."
echo "To uninstall: uninstall.sh $MODE"
