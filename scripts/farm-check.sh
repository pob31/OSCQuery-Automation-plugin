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
# The CI job's checks on a farm machine (macOS or Linux), for when a runner is slow or a
# branch is not pushed yet: configure `dev`, build Debug, ctest under C and fr_FR, build
# the Release plugin and the CLAP, pluginval 1.0.4 at strictness 5, clap-validator 0.4.1.
# Logs go next to the checkout.
#
#   Usage: scripts/farm-check.sh <checkout>
#
# Moving an unpushed branch there, as go.dot does:
#   git bundle create oqa.bundle main && scp oqa.bundle macmini:/tmp/
#   ssh macmini 'git clone -b main /tmp/oqa.bundle oqa && cd oqa && git submodule update --init --recursive'
set -uo pipefail
export PATH=/opt/homebrew/bin:$PATH
cd "$1" || exit 1
OS=$(uname -s)
step() { echo; echo "=== $1"; }

step "configure (dev)"
cmake --preset dev > ../farm-oqa-config.log 2>&1 || { tail -30 ../farm-oqa-config.log; exit 1; }
grep '^CMake Warning' ../farm-oqa-config.log | grep -v ThirdParty/ && echo "!! configure warnings outside ThirdParty"

step "build Debug"
cmake --build --preset dev-debug > ../farm-oqa-build.log 2>&1 || { grep -E "error|Error" ../farm-oqa-build.log | head -30; exit 1; }
echo "warnings in our code:"; grep -E "warning:" ../farm-oqa-build.log | grep -v ThirdParty/ | sort -u | head -20; echo "(end)"

step "ctest"
ctest --preset dev-debug || exit 1
./build/dev/tests/Debug/oqa_tests --oqa-locale=fr_FR.UTF-8 2>&1 | tail -3

step "build Release plugin and CLAP"
cmake --build --preset dev-release --target oqa_plugin_32_All oqa_plugin_monitor_All oqa_clap > ../farm-oqa-release.log 2>&1 || { grep -E "error|Error" ../farm-oqa-release.log | head -30; exit 1; }
ART=build/dev/src/shims/juce/oqa_plugin_32_artefacts/Release
ls "$ART"
VST="$ART/VST3/OSCQuery Automation 32.vst3"
MONITOR="build/dev/src/shims/juce/oqa_plugin_monitor_artefacts/Release/VST3/OSCQuery Automation Monitor.vst3"

# THE VALIDATORS RUN THE PLUGIN WITH DNS-SD OFF (2026-11-10): they load the real plugin, in a
# couple of hundred processes at once for clap-validator, and with its server announced over
# multicast DNS that flooded the network this machine is on (docs/bench/2026-11-10-mdns-flood.md).
# The server stays on; the settings are a copy, so the user's own file is not read or written.
cp tests/validators.registry.json ../farm-oqa-validators.registry.json
export OQA_SETTINGS_FILE="$(cd .. && pwd)/farm-oqa-validators.registry.json"

step "pluginval 1.0.4, strictness 5"
rm -rf ../pluginval && mkdir ../pluginval
if [ "$OS" = Darwin ]; then
  curl -sSL -o ../pluginval/pv.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_macOS.zip
  unzip -q ../pluginval/pv.zip -d ../pluginval
  PV=../pluginval/pluginval.app/Contents/MacOS/pluginval
  lipo -archs "$VST/Contents/MacOS/OSCQuery Automation 32"
else
  curl -sSL -o ../pluginval/pv.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip
  unzip -q ../pluginval/pv.zip -d ../pluginval
  PV=../pluginval/pluginval; chmod +x $PV
fi
EXTRA=""
if [ "$OS" != Darwin ] && [ -z "${DISPLAY:-}" ]; then
  if command -v xvfb-run >/dev/null; then PVRUN="xvfb-run -a"; else PVRUN=""; EXTRA="--skip-gui-tests"; echo "(no DISPLAY and no xvfb: --skip-gui-tests)"; fi
fi
${PVRUN:-} $PV --strictness-level 5 --validate-in-process --timeout-ms 300000 $EXTRA --validate "$VST" > ../farm-oqa-pluginval.log 2>&1
RC=$?
grep -E "FAILED|SUCCESS|!!!" ../farm-oqa-pluginval.log | tail -10
echo "pluginval exit=$RC"
[ $RC -eq 0 ] || exit $RC

step "pluginval on the Monitor (Phase 6c)"
${PVRUN:-} $PV --strictness-level 5 --validate-in-process --timeout-ms 300000 $EXTRA --validate "$MONITOR" > ../farm-oqa-pluginval-monitor.log 2>&1
RC=$?
grep -E "FAILED|SUCCESS|!!!" ../farm-oqa-pluginval-monitor.log | tail -10
echo "pluginval (Monitor) exit=$RC"
[ $RC -eq 0 ] || exit $RC

step "clap-validator 0.4.1"
CLAP="build/dev/src/shims/clap/oqa_clap_artefacts/Release/OSCQuery Automation.clap"
CVB=clap-validator-0.4.1-127-g152b982
rm -rf ../clap-validator && mkdir ../clap-validator
if [ "$OS" = Darwin ]; then CVZ="$CVB-macos-universal.zip"; else CVZ="$CVB-ubuntu-22.04.zip"; fi
curl -sSL -o ../clap-validator/cv.zip "https://github.com/free-audio/clap-validator/releases/download/0.4.1/$CVZ"
unzip -q ../clap-validator/cv.zip -d ../clap-validator
for t in ../clap-validator/*.tar.gz; do [ -f "$t" ] && tar -xzf "$t" -C ../clap-validator; done
CV=$(find ../clap-validator -type f -name 'clap-validator' | head -1); chmod +x "$CV"
"$CV" validate "$CLAP" > ../farm-oqa-clap-validator.log 2>&1
RC=$?
tail -5 ../farm-oqa-clap-validator.log
echo "clap-validator exit=$RC"
exit $RC
