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

# The core first (devplan Phase 3), the editor's sources (an interface target), then the
# two shims that link both: JUCE's (VST3 and AU) and our own CLAP (Phase 5b).
add_subdirectory(core)
add_subdirectory(editor)
add_subdirectory(shims/juce)
add_subdirectory(shims/clap)

# EVERY BINARY AN INSTALLER SHIPS, in one target (2026-11-22): each JUCE variant in every
# format, the CLAP module and the shared core library. scripts/build-release.* build this and
# nothing else, so a variant added in shims/juce reaches the installers by itself.
get_property(_oqa_juce_plugins GLOBAL PROPERTY OQA_JUCE_PLUGIN_TARGETS)
add_custom_target(oqa_plugins)
add_dependencies(oqa_plugins ${_oqa_juce_plugins} oqa_clap oqa_core_shared)
unset(_oqa_juce_plugins)
