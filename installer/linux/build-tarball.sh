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
# The Linux tarball (2026-11-22): the staged tree (scripts/build-release.sh) as it is, with
# install.sh and uninstall.sh at its top. Shape from WFS-DIY's
# Plugin/Installer/build-tarball-linux.sh; the scripts are files here, not heredocs.
#
#   installer/linux/build-tarball.sh <stage-dir> <version> <output-dir>
#
# Produces <output-dir>/OSCQuery-Automation-<version>-Linux-<arch>.tar.gz:
#   OSCQuery-Automation-<version>-Linux-<arch>/
#       VST3/*.vst3  CLAP/OSCQuery Automation.clap  Core/libOSCQuery-Automation-Core.so
#       Docs/  install.sh  uninstall.sh  README.txt

set -euo pipefail

[ $# -eq 3 ] || { echo "usage: $0 <stage-dir> <version> <output-dir>" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE="$(cd "$1" && pwd)"
VERSION="$2"
mkdir -p "$3"
OUTPUT_DIR="$(cd "$3" && pwd)"

NAME="OSCQuery-Automation-${VERSION}-Linux-$(uname -m)"
WORK="$OUTPUT_DIR/$NAME"
TARBALL="$OUTPUT_DIR/$NAME.tar.gz"

rm -rf "$WORK" "$TARBALL"
mkdir -p "$WORK"
cp -R "$STAGE"/. "$WORK/"
install -m 0755 "$HERE/install.sh"   "$WORK/install.sh"
install -m 0755 "$HERE/uninstall.sh" "$WORK/uninstall.sh"
sed "s/@VERSION@/$VERSION/g; s/@ARCH@/$(uname -m)/g" "$HERE/README.txt" > "$WORK/README.txt"

# Owned by root in the archive, so a system install does not leave the builder's uid behind.
tar --owner=0 --group=0 -C "$OUTPUT_DIR" -czf "$TARBALL" "$NAME"
rm -rf "$WORK"

echo "==> Produced $TARBALL"
