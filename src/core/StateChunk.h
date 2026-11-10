/*
    This file is part of the OSCQuery Automation Plugin —
    https://github.com/pob31/OSCQuery-Automation-plugin

    Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle

    The OSCQuery Automation Plugin is free software: you can redistribute it
    and/or modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the License,
    or (at your option) any later version. It is distributed in the hope that it
    will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
    Public License (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

/*
    THE STATE CHUNK (PRD, Session persistence: "All bindings live in the
    plugin's state chunk inside the DAW session. No external mapping file.")
    JSON, versioned, format-neutral: the same bytes come back from the VST3,
    the AU and later the CLAP.

      version      1, from this first commit
      devices[]    name, last_host, last_port, tree_hash, identity_path,
                   identity_value, honours_timetags, offset_ms, and
                   device_id when the device declares a DEVICE_ID (Phase
                   6x; additive, written only when set, version unchanged)
      slots[]      host_param_id, device_index, path, arg_index, type, range
                   {min, max}, scaling, direction, display_name,
                   range_overridden, and two the PRD's list has not: unit
                   (what the tree said, so the editor shows it before the
                   device is reached) and critical (the tree's CRITICAL, so
                   the reliable route is known before the tree is)
      values[]     every slot's host value, normalised 0-1, in slot order
                   (decided 2026-11-09, Phase 5b: a CLAP persists its own
                   parameter values, and clap-validator checks that a saved
                   state restores them; the JUCE shim writes them too, so a
                   chunk opens in any format with its values). Optional on
                   read: absent in every chunk saved before.

    ONLY BOUND SLOTS ARE WRITTEN. A slot absent from slots[] is unbound.

    EVERY VERSION EVER SHIPPED STAYS LOADABLE (devplan, cross-cutting): a
    fixture per version lives in tests/state/, and a reader per version lives
    here. A chunk newer than this plugin loads what this plugin understands and
    says so in `warnings`; nothing is refused for being from the future.

    UNKNOWN FIELDS ARE KEPT and written back, at the top level, per device and
    per slot, so a session saved by a newer plugin and reopened by this one
    loses nothing it did not understand.

    NUMBERS GO THROUGH juce::JSON, whose text is locale-independent
    (tests/LocaleTests.cpp); the suite runs this under fr_FR as well as C.

    THE EXPORT FILE IS THIS CHUNK (decided by the author 2026-11-09, Phase 5c;
    PRD "Export and import": "bindings export to a standalone JSON file so
    Go.dot, ossia or a second DAW can reuse them", "import can address slots
    by host_param_id"). No envelope, no second schema: what the editor's Export
    writes is saveState(), what Import reads is loadState(), and
    tools/convert-mapping.py writes the same from a ToscA or OSCar mapping. A
    chunk from a bigger variant loads what fits, a warning per slot left out.
*/

#include <core/Slot.h>

#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oqa::core
{
    /** One device the session refers to; slots point at it by index. */
    struct DeviceRef
    {
        std::string name;
        std::string lastHost;
        int lastPort = 0;
        std::string treeHash;

        std::optional<std::string> identityPath;
        std::optional<std::string> identityValue;

        /*  The device's DEVICE_ID, when it declares one (Phase 6x; the optional
            extension of docs/spec/extensions-draft-0.1.md). Written only when
            set, so a chunk saved before, or against a device that declares
            none, keeps its bytes. */
        std::optional<std::string> deviceId;

        bool honoursTimetags = false;
        double offsetMs = 0.0;

        /** Fields this version does not read, as saved. */
        juce::NamedValueSet extras;
    };

    //==============================================================================
    struct StateChunk
    {
        static constexpr int currentVersion = 1;

        /** The version read, or currentVersion for a chunk made here. */
        int version = currentVersion;

        std::vector<DeviceRef> devices;
        SlotTable slots;

        /** Every slot's host value, 0-1, in slot order; empty when the chunk carried none. */
        std::vector<float> values;

        /*  The number `$instance` stands for in this instance's paths (PRD,
            Binding model; decided 2026-11-09: user-set, default 1). Written
            only when it is not 1, so every chunk saved before keeps its bytes. */
        int instanceNumber = 1;

        /** Top-level fields this version does not read. */
        juce::NamedValueSet extras;

        /** Per-slot fields this version does not read, by host parameter ID. */
        std::map<int, juce::NamedValueSet> slotExtras;

        /** What the loader could not honour and chose a default for, in words. */
        std::vector<std::string> warnings;

        //==============================================================================
        explicit StateChunk (int slotCount);

        /*  Read a chunk for a table of `slotCount` slots. nullopt with the
            reason when the text is not a chunk at all; otherwise everything
            understood is in, and what was not is in `warnings`. A slot whose
            host_param_id is beyond slotCount, or whose device_index names no
            device, is dropped with a warning: the curve attached to that ID
            in the host is not this plugin's to lose, and the host keeps it. */
        static std::optional<StateChunk> fromJson (std::string_view json, int slotCount, std::string& error);
        static std::optional<StateChunk> fromVar (const juce::var& root, int slotCount, std::string& error);

        juce::var toVar() const;
        std::string toJson() const;
    };
}
