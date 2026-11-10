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

/*
    THE STATE CHUNK (devplan Phase 3b: "the PRD's JSON, version from the first
    commit, a loader that reads every past version (fixtures kept in
    tests/state/), unknown fields preserved on round-trip").

    The fixtures under tests/state/ are the one place this repository writes an
    OSC address down: a session saved against the generic mock device. Run
    under C and fr_FR, because ranges and offsets are decimals.
*/

#include <doctest/doctest.h>

#include "TestSupport.h"

#include <core/StateChunk.h>

#include <oscquery/tree/Tree.h>

#include <string>

using namespace oqa::core;

namespace
{
    juce::File stateFixture (const char* name)
    {
        return juce::File (juce::String (OQA_TEST_STATE_DIR)).getChildFile (name);
    }

    StateChunk load (const char* name, int slotCount)
    {
        std::string error;
        auto chunk = StateChunk::fromJson (stateFixture (name).loadFileAsString().toStdString(), slotCount, error);
        REQUIRE_MESSAGE (chunk.has_value(), error);
        return *chunk;
    }

    bool mentions (const std::vector<std::string>& warnings, const char* needle)
    {
        for (const auto& w : warnings)
            if (w.find (needle) != std::string::npos)
                return true;

        return false;
    }
}

//==============================================================================
TEST_CASE ("v1-generic: two devices and seven bound slots read back as saved")
{
    const auto chunk = load ("v1-generic.json", 32);

    CHECK (chunk.version == 1);
    CHECK (chunk.warnings.empty());

    REQUIRE (chunk.devices.size() == 2);

    const auto& first = chunk.devices[0];
    CHECK (first.name == "Generic Mock");
    CHECK (first.lastHost == "192.168.1.40");
    CHECK (first.lastPort == 5005);
    CHECK (first.treeHash.size() == 64);
    CHECK (first.identityPath == "/system/serial");
    CHECK (first.identityValue == "MOCK-0001");
    CHECK (first.honoursTimetags);
    CHECK (first.offsetMs == doctest::Approx (-12.5));

    const auto& second = chunk.devices[1];
    CHECK_FALSE (second.identityPath.has_value());
    CHECK_FALSE (second.identityValue.has_value());
    CHECK_FALSE (second.honoursTimetags);
    CHECK (second.offsetMs == 0.0);

    CHECK (chunk.slots.size() == 32);
    CHECK (chunk.slots.boundCount() == 7);

    SUBCASE ("a ranged float")
    {
        const auto& s = chunk.slots[0];
        CHECK (s.deviceIndex == 0);
        CHECK (s.type == 'f');
        CHECK (s.rangeMin == -60.0);
        CHECK (s.rangeMax == 12.0);
        CHECK_FALSE (s.rangeOverridden);
        CHECK (s.scaling == Scaling::linear);
        CHECK (s.direction == Direction::send);
        CHECK (s.displayName == "Ch 1 gain");
        CHECK (s.unit == "dB");
        CHECK (s.active());
    }

    SUBCASE ("a user range with the log curve, direction both")
    {
        const auto& s = chunk.slots[1];
        CHECK (s.rangeOverridden);
        CHECK (s.rangeMin == 20.0);
        CHECK (s.rangeMax == 20000.0);
        CHECK (s.scaling == Scaling::logarithmic);
        CHECK (s.direction == Direction::both);
        CHECK (s.toDevice (0.5f) == doctest::Approx (632.455532));
    }

    SUBCASE ("an fff on three consecutive slots, found again as a group")
    {
        const std::vector<int> expected { 4, 5, 6 };
        CHECK (chunk.slots.groupOf (5) == expected);
        CHECK (chunk.slots[4].argIndex == 0);
        CHECK (chunk.slots[6].argIndex == 2);
        CHECK (chunk.slots[6].rangeMax == 5.0);
        CHECK (chunk.slots[6].scaling == Scaling::exponential);
        CHECK (chunk.slots[4].path == chunk.slots[6].path);
    }

    SUBCASE ("a toggle with a null range is active")
    {
        const auto& s = chunk.slots[7];
        CHECK (s.type == 'T');
        CHECK_FALSE (s.rangeMin.has_value());
        CHECK (s.active());
    }

    SUBCASE ("an integer set to receive")
    {
        const auto& s = chunk.slots[2];
        CHECK (s.type == 'i');
        CHECK (s.direction == Direction::receive);
        CHECK (s.rangeMin == 0.0);
        CHECK (s.rangeMax == 127.0);
    }

    SUBCASE ("slots the fixture does not mention are unbound")
    {
        CHECK_FALSE (chunk.slots[3].bound());
        CHECK_FALSE (chunk.slots[31].bound());
    }
}

TEST_CASE ("a chunk round-trips to the same JSON, bound slots only, in host parameter order")
{
    const auto chunk = load ("v1-generic.json", 32);
    const auto once = chunk.toJson();

    std::string error;
    const auto again = StateChunk::fromJson (once, 32, error);
    REQUIRE_MESSAGE (again.has_value(), error);
    CHECK (again->warnings.empty());
    CHECK (again->toJson() == once);

    /*  The fixture lists slot 2 last; the writer puts it in ID order. */
    const auto var = juce::JSON::parse (once);
    const auto* slots = var.getProperty ("slots", {}).getArray();
    REQUIRE (slots != nullptr);
    REQUIRE (slots->size() == 7);
    CHECK (static_cast<int> ((*slots)[2].getProperty ("host_param_id", -1)) == 2);
    CHECK (static_cast<int> ((*slots)[6].getProperty ("host_param_id", -1)) == 7);

    /*  Decimals survive the locale: under fr_FR the text still reads -12.5. */
    CHECK (once.find ("-12.5") != std::string::npos);
    CHECK (once.find ("12,5") == std::string::npos);
}

TEST_CASE ("v1-extras: unknown fields are kept and written back; what cannot be placed is a warning")
{
    const auto chunk = load ("v1-extras.json", 8);

    SUBCASE ("top-level, device and slot extras survive the round trip")
    {
        CHECK (chunk.extras.contains ("saved_by"));
        CHECK (chunk.extras.contains ("colour_theme"));
        CHECK (chunk.devices[0].extras.contains ("firmware"));
        CHECK (chunk.devices[0].extras.contains ("pinned"));

        REQUIRE (chunk.slotExtras.count (3) == 1);
        CHECK (chunk.slotExtras.at (3).contains ("colour"));
        CHECK (static_cast<int> (chunk.slotExtras.at (3)["smoothing_ms"]) == 40);

        const auto var = juce::JSON::parse (chunk.toJson());
        CHECK (var.getProperty ("saved_by", {}).toString() == "a future plugin, 9.9.9");
        CHECK (var.getProperty ("colour_theme", {}).getProperty ("accent", {}).toString() == "#ff8800");

        const auto* devices = var.getProperty ("devices", {}).getArray();
        REQUIRE (devices != nullptr);
        CHECK ((*devices)[0].getProperty ("firmware", {}).toString() == "2.4.1");

        const auto* slots = var.getProperty ("slots", {}).getArray();
        REQUIRE (slots != nullptr);
        bool found = false;

        for (const auto& s : *slots)
            if (static_cast<int> (s.getProperty ("host_param_id", -1)) == 3)
            {
                found = true;
                CHECK (s.getProperty ("colour", {}).toString() == "#00ccff");
            }

        CHECK (found);
    }

    SUBCASE ("an unknown scaling reads as linear and says so")
    {
        CHECK (chunk.slots[1].bound());
        CHECK (chunk.slots[1].scaling == Scaling::linear);
        CHECK (mentions (chunk.warnings, "s-curve"));
    }

    SUBCASE ("a slot beyond the table is dropped and the host keeps its curve")
    {
        CHECK (mentions (chunk.warnings, "slot 12"));
        CHECK (chunk.slots.boundCount() == 2);
    }

    SUBCASE ("a slot naming a device the chunk does not list is dropped")
    {
        CHECK_FALSE (chunk.slots[2].bound());
        CHECK (mentions (chunk.warnings, "device 4"));
    }

    SUBCASE ("an integer offset reads as a double")
    {
        CHECK (chunk.devices[0].offsetMs == 3.0);
    }
}

TEST_CASE ("v1-empty: a session with nothing bound loads and writes back bare")
{
    const auto chunk = load ("v1-empty.json", 16);
    CHECK (chunk.devices.empty());
    CHECK (chunk.slots.boundCount() == 0);
    CHECK (chunk.warnings.empty());

    const auto var = juce::JSON::parse (chunk.toJson());
    CHECK (static_cast<int> (var.getProperty ("version", 0)) == 1);
    REQUIRE (var.getProperty ("slots", {}).getArray() != nullptr);
    CHECK (var.getProperty ("slots", {}).getArray()->isEmpty());
}

TEST_CASE ("v1-receive: three slots, one each way, read back as saved")
{
    const auto chunk = load ("v1-receive.json", 8);
    CHECK (chunk.warnings.empty());
    REQUIRE (chunk.devices.size() == 1);
    CHECK (chunk.slots.boundCount() == 3);
    CHECK (chunk.slots[0].direction == Direction::receive);
    CHECK (chunk.slots[0].receives());
    CHECK_FALSE (chunk.slots[0].sends());
    CHECK (chunk.slots[1].direction == Direction::both);
    CHECK (chunk.slots[1].sends());
    CHECK (chunk.slots[1].receives());
    CHECK (chunk.slots[2].direction == Direction::send);
    CHECK_FALSE (chunk.slots[2].receives());
    CHECK (chunk.slots[0].active());

    std::string error;
    const auto again = StateChunk::fromJson (chunk.toJson(), 8, error);
    REQUIRE_MESSAGE (again.has_value(), error);
    CHECK (again->slots[1].direction == Direction::both);
}

TEST_CASE ("v1-values: every slot's host value reads back, a longer list is cut to the table with a warning, a chunk without one has none")
{
    const auto chunk = load ("v1-values.json", 8);
    REQUIRE (chunk.values.size() == 8);
    CHECK (chunk.values[1] == doctest::Approx (0.75f));
    CHECK (chunk.values[3] == doctest::Approx (1.0f));
    CHECK (chunk.values[7] == doctest::Approx (0.125f));
    CHECK (chunk.warnings.empty());

    /*  The same file read for a smaller variant: what fits is kept. */
    const auto smaller = load ("v1-values.json", 4);
    REQUIRE (smaller.values.size() == 4);
    CHECK (smaller.values[3] == doctest::Approx (1.0f));
    CHECK (mentions (smaller.warnings, "values: 8 for a table of 4"));

    /*  Written back: the list as read, in order; and a chunk never given
        values writes none, so every chunk saved before this field is
        byte-identical on a round trip (the earlier cases). */
    const auto var = juce::JSON::parse (chunk.toJson());
    const auto* written = var.getProperty ("values", {}).getArray();
    REQUIRE (written != nullptr);
    REQUIRE (written->size() == 8);
    CHECK (static_cast<double> ((*written)[1]) == doctest::Approx (0.75));

    CHECK (load ("v1-receive.json", 8).values.empty());
    CHECK (! juce::JSON::parse (load ("v1-receive.json", 8).toJson()).hasProperty ("values"));
}

TEST_CASE ("a fresh chunk made from bound slots writes what the PRD lists")
{
    std::string error;
    auto tree = oqa::oscquery::Tree::parse (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json")), error);
    REQUIRE_MESSAGE (tree.has_value(), error);

    const oqa::oscquery::Node* fff = nullptr;

    for (const auto* leaf : tree->leaves())
        if (leaf->argumentCount() == 3)
            fff = leaf;

    REQUIRE (fff != nullptr);

    StateChunk chunk (8);
    DeviceRef device;
    device.name = "Generic Mock";
    device.lastHost = "127.0.0.1";
    device.lastPort = 5005;
    device.treeHash = tree->treeHash();
    chunk.devices.push_back (device);

    std::string why;
    REQUIRE_MESSAGE (chunk.slots.bindAll (2, *fff, 0, why), why);

    const auto var = juce::JSON::parse (chunk.toJson());
    const auto* slots = var.getProperty ("slots", {}).getArray();
    REQUIRE (slots != nullptr);
    REQUIRE (slots->size() == 3);

    const auto& s = (*slots)[0];

    for (const char* field : { "host_param_id", "device_index", "path", "arg_index", "type", "range",
                               "scaling", "direction", "display_name", "range_overridden", "unit", "critical" })
    {
        INFO ("field " << field);
        CHECK (s.hasProperty (field));
    }

    CHECK (s.getProperty ("path", {}).toString().toStdString() == fff->fullPath);
    CHECK (s.getProperty ("type", {}).toString() == "f");
    CHECK (static_cast<double> (s.getProperty ("range", {}).getProperty ("min", {})) == *fff->argument (0).range->min);

    const auto* devices = var.getProperty ("devices", {}).getArray();
    REQUIRE (devices != nullptr);
    CHECK ((*devices)[0].getProperty ("tree_hash", {}).toString().toStdString() == tree->treeHash());
}

TEST_CASE ("what is not a chunk is refused with a reason")
{
    std::string error;

    CHECK_FALSE (StateChunk::fromJson ("not json at all {", 8, error).has_value());
    CHECK_FALSE (error.empty());

    CHECK_FALSE (StateChunk::fromJson ("[1, 2, 3]", 8, error).has_value());
    CHECK (error.find ("object") != std::string::npos);

    CHECK_FALSE (StateChunk::fromJson ("{\"devices\": []}", 8, error).has_value());
    CHECK (error.find ("version") != std::string::npos);

    CHECK_FALSE (StateChunk::fromJson ("{\"version\": 0}", 8, error).has_value());
}

TEST_CASE ("a chunk from the future loads what this plugin understands and warns")
{
    std::string error;
    const auto chunk = StateChunk::fromJson (
        "{\"version\": 7, \"devices\": [{\"name\": \"X\", \"last_host\": \"h\", \"last_port\": 1, \"tree_hash\": \"\"}],"
        " \"slots\": [{\"host_param_id\": 0, \"device_index\": 0, \"path\": \"/a\", \"arg_index\": 0, \"type\": \"f\","
        " \"range\": {\"min\": 0, \"max\": 1}, \"scaling\": \"linear\", \"direction\": \"send\", \"display_name\": \"a\","
        " \"range_overridden\": false, \"hologram\": true}]}", 8, error);

    REQUIRE_MESSAGE (chunk.has_value(), error);
    CHECK (chunk->version == 7);
    CHECK (mentions (chunk->warnings, "newer"));
    CHECK (chunk->slots[0].bound());
    CHECK (chunk->slotExtras.at (0).contains ("hologram"));

    /*  Written back by this plugin, it is this plugin's version. */
    const auto var = juce::JSON::parse (chunk->toJson());
    CHECK (static_cast<int> (var.getProperty ("version", 0)) == StateChunk::currentVersion);
}

//==============================================================================
//  Phase 6x-3: the optional device_id, an additive field of version 1.
TEST_CASE ("v1-device-id: a device's DEVICE_ID is read, written back only where it was, and old chunks keep their bytes")
{
    const auto chunk = load ("v1-device-id.json", 8);
    CHECK (chunk.version == 1);
    CHECK (chunk.warnings.empty());
    REQUIRE (chunk.devices.size() == 2);

    CHECK (chunk.devices[0].deviceId == std::optional<std::string> ("6f1c2a9e-0b4d-4e2a-9f7c-3d5b8a1e2c47"));
    CHECK_FALSE (chunk.devices[1].deviceId.has_value());
    CHECK (chunk.devices[0].extras.isEmpty());          // a field this version reads is not an extra

    //  Round trip: the same JSON, the key on the first device and not on the second.
    const auto once = chunk.toJson();
    std::string error;
    const auto again = StateChunk::fromJson (once, 8, error);
    REQUIRE_MESSAGE (again.has_value(), error);
    CHECK (again->toJson() == once);

    const auto var = juce::JSON::parse (once);
    const auto* devices = var.getProperty ("devices", {}).getArray();
    REQUIRE (devices != nullptr);
    REQUIRE (devices->size() == 2);
    CHECK ((*devices)[0].getProperty ("device_id", "").toString() == "6f1c2a9e-0b4d-4e2a-9f7c-3d5b8a1e2c47");
    CHECK_FALSE ((*devices)[1].hasProperty ("device_id"));

    //  A chunk saved before this field existed says nothing of it when saved again.
    CHECK (load ("v1-generic.json", 32).toJson().find ("device_id") == std::string::npos);

    //  Not a string, or empty: not an identifier, and not kept as one.
    const auto odd = StateChunk::fromJson (R"({ "version": 1, "devices": [
        { "name": "A", "last_host": "h", "last_port": 1, "tree_hash": "", "device_id": 7, "honours_timetags": false, "offset_ms": 0 },
        { "name": "B", "last_host": "h", "last_port": 2, "tree_hash": "", "device_id": "", "honours_timetags": false, "offset_ms": 0 } ],
        "slots": [] })", 8, error);
    REQUIRE_MESSAGE (odd.has_value(), error);
    CHECK_FALSE (odd->devices[0].deviceId.has_value());
    CHECK_FALSE (odd->devices[1].deviceId.has_value());
    CHECK (odd->toJson().find ("device_id") == std::string::npos);
}
