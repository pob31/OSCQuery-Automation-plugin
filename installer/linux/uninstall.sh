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
# Removes what install.sh put in place, nothing else (2026-11-22). The per-user settings
# file and recents are the user's and stay.
#
#   ./uninstall.sh --user | --system

set -euo pipefail

CORE_FILE="libOSCQuery-Automation-Core.so"
CLAP_NAME="OSCQuery Automation.clap"

case "${1:-}" in
    --user)
        VST3_DIR="$HOME/.vst3"; CLAP_DIR="$HOME/.clap"
        CORE_DIRS=("$VST3_DIR" "$CLAP_DIR"); DOC_DIR=""; SUDO="" ;;
    --system)
        VST3_DIR="/usr/local/lib/vst3"; CLAP_DIR="/usr/lib/clap"
        CORE_DIRS=("/usr/local/lib/oscquery-automation")
        DOC_DIR="/usr/local/share/doc/oscquery-automation"; SUDO="sudo"
        [ "$(id -u)" -eq 0 ] && SUDO="" ;;
    *)
        echo "usage: $0 --user | --system" >&2
        exit 2 ;;
esac

shopt -s nullglob
for bundle in "$VST3_DIR"/"OSCQuery Automation"*.vst3; do
    $SUDO rm -rf "$bundle"
done
shopt -u nullglob
$SUDO rm -f "$CLAP_DIR/$CLAP_NAME"
for d in "${CORE_DIRS[@]}"; do
    $SUDO rm -f "$d/$CORE_FILE"
done
if [ "$1" = --system ]; then
    $SUDO rmdir "/usr/local/lib/oscquery-automation" 2>/dev/null || true
    $SUDO rm -rf "$DOC_DIR"
fi

echo "Uninstalled OSCQuery Automation ($1)."
