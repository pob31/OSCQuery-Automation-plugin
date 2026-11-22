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
# The macOS installer (2026-11-22): one .pkg from a staged tree (scripts/build-release.sh),
# signed, notarised and stapled when the environment says how. Shape and environment contract
# from WFS-DIY's Plugin/Installer/build-pkg.sh.
#
#   installer/macos/build-pkg.sh <stage-dir> <version> <output-dir>
#
# Signing, each optional; without them the bundles are sealed ad hoc and the .pkg is unsigned:
#   CODESIGN_IDENTITY_APP      "Developer ID Application: ..."  the bundles and the core library
#   CODESIGN_IDENTITY_INSTALL  "Developer ID Installer: ..."    the .pkg
# Notarisation runs when one of these is complete, and needs CODESIGN_IDENTITY_INSTALL:
#   NOTARY_API_KEY (path to the .p8) + NOTARY_API_KEY_ID + NOTARY_API_ISSUER   (CI)
#   NOTARY_APPLE_ID + NOTARY_PASSWORD + NOTARY_TEAM_ID
#   NOTARIZE_KEYCHAIN_PROFILE                                                 (a notarytool profile)
#
# Where things go, each format a choice in the Installer's Customize pane:
#   VST3  /Library/Audio/Plug-Ins/VST3/
#   AU    /Library/Audio/Plug-Ins/Components/
#   CLAP  /Library/Audio/Plug-Ins/CLAP/
#   core  /Library/Application Support/Pix et Bel/OSCQuery Automation/   always; the registry
#         loader's fixed path (src/core/registry/RegistryLoader.cpp), one copy for every format,
#         with the licence and notices beside it.
#
# PACKAGE IDENTIFIERS ARE FIXED FOR GOOD (2026-11-22): the Installer finds the previous
# version's receipts by them. io.github.pob31.oqa.pkg.{vst3,au,clap,core}.

set -euo pipefail

[ $# -eq 3 ] || { echo "usage: $0 <stage-dir> <version> <output-dir>" >&2; exit 2; }
STAGE="$(cd "$1" && pwd)"
VERSION="$2"
mkdir -p "$3"
OUTPUT_DIR="$(cd "$3" && pwd)"

CORE_FILE="libOSCQuery-Automation-Core.dylib"
ID_PREFIX="io.github.pob31.oqa.pkg"
CORE_LOCATION="/Library/Application Support/Pix et Bel/OSCQuery Automation"
PKG_FINAL="$OUTPUT_DIR/OSCQuery-Automation-${VERSION}-macOS.pkg"

WORK="$OUTPUT_DIR/pkg-work"
rm -rf "$WORK"
mkdir -p "$WORK/roots/vst3" "$WORK/roots/au" "$WORK/roots/clap" "$WORK/roots/core" \
         "$WORK/pkgs" "$WORK/resources"

# A copy, so signing never touches the stage the next run starts from.
cp -R "$STAGE"/VST3/. "$WORK/roots/vst3/"
cp -R "$STAGE"/AU/.   "$WORK/roots/au/"
cp -R "$STAGE"/CLAP/. "$WORK/roots/clap/"
cp "$STAGE/Core/$CORE_FILE" "$WORK/roots/core/"
cp "$STAGE"/Docs/*.txt      "$WORK/roots/core/"
cp "$STAGE/Docs/LICENSE.txt" "$WORK/resources/"

# --- Sign: the library first, then each bundle -------------------------------------------------

IDENTITY="${CODESIGN_IDENTITY_APP:--}"
SIGN_OPTS=(--force --sign "$IDENTITY")
if [ "$IDENTITY" != "-" ]; then
    echo "==> Sign with: $IDENTITY"
    # Hardened runtime and a secure timestamp: what the notary service requires.
    SIGN_OPTS+=(--options runtime --timestamp)
else
    echo "==> No CODESIGN_IDENTITY_APP: sealing ad hoc, the .pkg will be unsigned"
fi

codesign "${SIGN_OPTS[@]}" "$WORK/roots/core/$CORE_FILE"
for b in "$WORK"/roots/vst3/*.vst3 "$WORK"/roots/au/*.component "$WORK"/roots/clap/*.clap; do
    codesign "${SIGN_OPTS[@]}" --deep "$b"
done
for b in "$WORK/roots/core/$CORE_FILE" "$WORK"/roots/vst3/*.vst3 "$WORK"/roots/au/*.component "$WORK"/roots/clap/*.clap; do
    codesign --verify --deep --strict "$b"
done

# --- One component package per destination -----------------------------------------------------

# pkgbuild marks every bundle it finds relocatable by default: the Installer would then
# "upgrade" a copy with the same bundle id wherever Spotlight saw one (a build tree, a
# per-user install) and never write /Library. A component plist turns that off.
component_pkg() {
    local name="$1" location="$2"
    local root="$WORK/roots/$name" plist="$WORK/$name.plist"
    local args=(--root "$root" --identifier "$ID_PREFIX.$name" --version "$VERSION"
                --install-location "$location")
    pkgbuild --analyze --root "$root" "$plist" >/dev/null
    local i=0
    while plutil -extract "$i" xml1 -o /dev/null "$plist" 2>/dev/null; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
        i=$((i + 1))
    done
    [ "$i" -gt 0 ] && args+=(--component-plist "$plist")
    pkgbuild "${args[@]}" "$WORK/pkgs/$ID_PREFIX.$name.pkg"
}

component_pkg vst3 "/Library/Audio/Plug-Ins/VST3"
component_pkg au   "/Library/Audio/Plug-Ins/Components"
component_pkg clap "/Library/Audio/Plug-Ins/CLAP"
component_pkg core "$CORE_LOCATION"

# --- The product: the licence shown, a choice per format, the core always -----------------------

# hostArchitectures: the binaries are universal; without it Apple silicon offers Rosetta.
cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>OSCQuery Automation $VERSION</title>
    <license file="LICENSE.txt"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <allowed-os-versions><os-version min="11.0"/></allowed-os-versions>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="clap"/>
        <line choice="core"/>
    </choices-outline>
    <choice id="vst3" title="VST3" description="For Reaper, Live, Bitwig, Nuendo and other VST3 hosts.">
        <pkg-ref id="$ID_PREFIX.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit" description="For Logic, GarageBand, Live and other AU hosts.">
        <pkg-ref id="$ID_PREFIX.au"/>
    </choice>
    <choice id="clap" title="CLAP" description="For Bitwig, Reaper and other CLAP hosts.">
        <pkg-ref id="$ID_PREFIX.clap"/>
    </choice>
    <choice id="core" title="Core library" visible="false" enabled="false" selected="true">
        <pkg-ref id="$ID_PREFIX.core"/>
    </choice>
    <pkg-ref id="$ID_PREFIX.vst3" version="$VERSION" onConclusion="none">$ID_PREFIX.vst3.pkg</pkg-ref>
    <pkg-ref id="$ID_PREFIX.au"   version="$VERSION" onConclusion="none">$ID_PREFIX.au.pkg</pkg-ref>
    <pkg-ref id="$ID_PREFIX.clap" version="$VERSION" onConclusion="none">$ID_PREFIX.clap.pkg</pkg-ref>
    <pkg-ref id="$ID_PREFIX.core" version="$VERSION" onConclusion="none">$ID_PREFIX.core.pkg</pkg-ref>
</installer-gui-script>
XML

rm -f "$PKG_FINAL"
PRODUCT_ARGS=(--distribution "$WORK/distribution.xml" --resources "$WORK/resources"
              --package-path "$WORK/pkgs")
if [ -n "${CODESIGN_IDENTITY_INSTALL:-}" ]; then
    echo "==> Sign the .pkg with: $CODESIGN_IDENTITY_INSTALL"
    PRODUCT_ARGS+=(--sign "$CODESIGN_IDENTITY_INSTALL" --timestamp)
fi
productbuild "${PRODUCT_ARGS[@]}" "$PKG_FINAL"

# --- Notarise, staple ------------------------------------------------------------------------

NOTARY_AUTH=()
if [ -n "${NOTARY_API_KEY:-}" ] && [ -n "${NOTARY_API_KEY_ID:-}" ] && [ -n "${NOTARY_API_ISSUER:-}" ]; then
    NOTARY_AUTH=(--key "$NOTARY_API_KEY" --key-id "$NOTARY_API_KEY_ID" --issuer "$NOTARY_API_ISSUER")
elif [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${NOTARY_PASSWORD:-}" ] && [ -n "${NOTARY_TEAM_ID:-}" ]; then
    NOTARY_AUTH=(--apple-id "$NOTARY_APPLE_ID" --password "$NOTARY_PASSWORD" --team-id "$NOTARY_TEAM_ID")
elif [ -n "${NOTARIZE_KEYCHAIN_PROFILE:-}" ]; then
    NOTARY_AUTH=(--keychain-profile "$NOTARIZE_KEYCHAIN_PROFILE")
fi

if [ "${#NOTARY_AUTH[@]}" -gt 0 ]; then
    if [ -z "${CODESIGN_IDENTITY_INSTALL:-}" ] || [ "$IDENTITY" = "-" ]; then
        echo "notarisation asked for, but the bundles or the .pkg are not signed with a Developer ID" >&2
        echo "(set CODESIGN_IDENTITY_APP and CODESIGN_IDENTITY_INSTALL; the notary refuses ad-hoc code)" >&2
        exit 1
    fi
    echo "==> Notarise (waits for Apple's answer)"
    # Into a file, then read: a grep -q in the pipe would close it early and pipefail would
    # report the accepted submission as a failure.
    xcrun notarytool submit "$PKG_FINAL" "${NOTARY_AUTH[@]}" --wait --output-format json \
        > "$WORK/notary.json" || true
    cat "$WORK/notary.json"
    if ! grep -q '"status" *: *"Accepted"' "$WORK/notary.json"; then
        SUBMISSION="$(sed -nE 's/.*"id" *: *"([^"]+)".*/\1/p' "$WORK/notary.json" | head -n1)"
        [ -n "$SUBMISSION" ] && xcrun notarytool log "$SUBMISSION" "${NOTARY_AUTH[@]}" >&2 || true
        echo "notarisation was not accepted" >&2
        exit 1
    fi
    xcrun stapler staple "$PKG_FINAL"
    xcrun stapler validate "$PKG_FINAL"
    spctl --assess --type install -vv "$PKG_FINAL"
fi

echo "==> Produced $PKG_FINAL"
