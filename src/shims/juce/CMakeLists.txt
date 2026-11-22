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

# The JUCE shim: one AudioProcessor, built as VST3 everywhere and AU on macOS.
#
# PLUGIN IDENTITY (decided 2026-11-04). The VST3 class ID and the AU type, subtype and
# manufacturer are derived from the manufacturer code, the plugin code and
# JUCE_VST3_CAN_REPLACE_VST2 (cmake/OqaThirdParty.cmake). Once a session has saved the
# plugin, none of the three may change, or the session no longer finds it.
#
#   Company "Pix et Bel", manufacturer code PxBl, as WFS-DIY.
#   Plugin codes Q008 Q016 Q032 Q064 Q128 for the slot-count variants, QMon for the
#   Monitor (zero slots, Phase 6c). Phase 0 builds Q032 only; Phase 7 adds the other four,
#   one line each.

set(OQA_PLUGIN_COMPANY       "Pix et Bel")
set(OQA_PLUGIN_MANUFACTURER  "PxBl")
set(OQA_PLUGIN_BUNDLE_PREFIX "io.github.pob31.oqa")

set(OQA_PLUGIN_FORMATS VST3)
if(APPLE)
    list(APPEND OQA_PLUGIN_FORMATS AU)
endif()

# One variant: target oqa_plugin_<tag>, the product named as given; `slots` is 8..128, or 0 for
# the Monitor.
function(oqa_add_plugin_variant slots code tag product description)
    set(target oqa_plugin_${tag})

    juce_add_plugin(${target}
        COMPANY_NAME                "${OQA_PLUGIN_COMPANY}"
        PLUGIN_MANUFACTURER_CODE    "${OQA_PLUGIN_MANUFACTURER}"
        PLUGIN_CODE                 "${code}"
        BUNDLE_ID                   "${OQA_PLUGIN_BUNDLE_PREFIX}.${code}"
        PRODUCT_NAME                "${product}"
        DESCRIPTION                 "${description}"
        VERSION                     "${PROJECT_VERSION}"
        FORMATS                     ${OQA_PLUGIN_FORMATS}
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            FALSE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD     FALSE
        VST3_CATEGORIES             "Fx" "Network"
        AU_MAIN_TYPE                "kAudioUnitType_Effect")

    target_sources(${target} PRIVATE
        PluginProcessor.h
        PluginProcessor.cpp
        SlotParameter.h
        SlotParameter.cpp
        EditorWindow.h
        EditorWindow.cpp

        # The core's and the client library's object files go INTO the plugin's shared-code
        # archive rather than beside it: GNU ld scans an archive once, in order, and the
        # format targets put the shared-code archive (where the plugin's JUCE modules are)
        # before any other, so a separate liboqa_oscquery.a could never find juce::SHA256 or
        # the websocket client. Members of one archive resolve each other whatever the order.
        $<TARGET_OBJECTS:oqa_core>
        $<TARGET_OBJECTS:oqa_oscquery>)

    target_compile_definitions(${target} PRIVATE
        OQA_SLOT_COUNT=${slots})

    # For oqa_plugins (src/CMakeLists.txt), what the installers build: a variant added here
    # reaches them with no script edited.
    set_property(GLOBAL APPEND PROPERTY OQA_JUCE_PLUGIN_TARGETS ${target}_All)

    # The core and the client library, and the JUCE modules they need beyond what
    # juce_audio_processors brings (juce_simpleweb for the websocket, cryptography for the
    # tree hash, data_structures for the recents store). The plugin compiles its own JUCE
    # through juce_add_plugin; oqa::core and oqa::oscquery carry headers and switches only.
    target_link_libraries(${target}
        PRIVATE
            oqa::core
            oqa::oscquery
            oqa::editor
            juce::juce_audio_processors
            juce::juce_cryptography
            juce::juce_data_structures
            juce_simpleweb
            oqa::juce_config
            oqa::warnings
            juce::juce_recommended_config_flags)

    # The shared core library beside the plugin bundle after every build, where the
    # loader looks first (walking up from the plugin's own module), so a dev build shares a
    # registry as an installed one will. WFS-DIY's dev builds never found their bridge.
    foreach(format IN LISTS OQA_PLUGIN_FORMATS)
        set(format_target ${target}_${format})
        if(TARGET ${format_target})
            add_dependencies(${format_target} oqa_core_shared)
            # The binary sits in <bundle>/Contents/<arch>/; three up is the folder holding
            # the bundle, VST3 and AU alike.
            add_custom_command(TARGET ${format_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:oqa_core_shared>"
                        "$<TARGET_FILE_DIR:${format_target}>/../../../"
                COMMENT "Copying the shared core library beside the ${format} bundle")
        endif()
    endforeach()
endfunction()

oqa_add_plugin_variant(32 Q032 32 "OSCQuery Automation 32" "Automation lanes bound to OSCQuery parameters")
oqa_add_plugin_variant(0 QMon monitor "OSCQuery Automation Monitor" "The registry's view: devices, every slot across instances, rates, errors, settings")
