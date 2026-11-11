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
    THE PRD'S FAILURE-STATES TABLE (Access control and failure states), one
    case per row, each driven by the mock device and asserting the status key
    the editor reads for it (devplan Phase 5c: "all reachable and all driven by
    the mock device in tests"):

        Unreachable      reachable false, state "unreachable", last_seen_ms -1
        Unauthorised     the slot's state "unauthorised"
        Stale            the slot's state "missing"
        Range unknown    the slot's state "inactive", the warning
        Echo loop        loop_guarded counts
        Dropped sends    rate_limited counts

    Most of it is the word of earlier phases' code, gathered here so the table
    has one test. NO ADDRESS IS TYPED.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>

#include <chrono>
#include <string>
#include <thread>

using namespace oqa::core;
using namespace oqa::oscquery;
using oqatest::waitUntil;
using oqatest::Rig;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    void sleepMs (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }

    juce::var slotStatus (const Instance& instance, int id)
    {
        const auto status = juce::JSON::parse (juce::String (instance.statusJson()));

        if (const auto* slots = status.getProperty ("slots", {}).getArray())
            for (const auto& s : *slots)
                if (static_cast<int> (s.getProperty ("id", -1)) == id)
                    return s;

        return {};
    }

    std::string slotState (const Instance& instance, int id)
    {
        return slotStatus (instance, id).getProperty ("state", "").toString().toStdString();
    }

    juce::int64 counter (const Instance& instance, const char* name, int deviceIndex = 0)
    {
        return static_cast<juce::int64> (Rig::deviceStatus (instance, deviceIndex).getProperty (name, 0));
    }

    bool isUnrangedFloat (const Node& n)
    {
        return n.type == "f" && n.bindableForSend() && ! n.argument (0).range.has_value();
    }

    /** A registry setting changed for one case and put back after it. */
    struct ScopedSetting
    {
        ScopedSetting (Instance& instanceIn, const std::string& set, const std::string& restoreIn)
            : instance (instanceIn), restore (restoreIn)
        {
            std::string error;
            REQUIRE_MESSAGE (instance.setSettingsJson (set, error), error);
        }

        ~ScopedSetting()
        {
            std::string error;
            instance.setSettingsJson (restore, error);
        }

        Instance& instance;
        std::string restore;
    };
}

//==============================================================================
TEST_CASE ("failure states: unreachable - no HOST_INFO answer: the device row's word, never seen")
{
    int deadPort = 0;

    {
        oqatest::MockDevice dead;
        REQUIRE (dead.loadTree (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json"))));
        REQUIRE (dead.start());
        deadPort = dead.httpPort();
        dead.stop();
    }

    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);
    auto ref = rig.device ("failure-unreachable");
    ref.lastPort = deadPort;

    Instance one (api(), 4, "failure-unreachable");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (ref), why));
    one.connect();

    REQUIRE (waitUntil ([&] { return Rig::deviceStatus (one).getProperty ("state", "").toString() == "unreachable"; }));
    const auto d = Rig::deviceStatus (one);
    CHECK_FALSE (static_cast<bool> (d.getProperty ("reachable", true)));
    CHECK (static_cast<juce::int64> (d.getProperty ("last_seen_ms", 0)) == -1);
    CHECK_FALSE (static_cast<bool> (d.getProperty ("sender_ready", true)));
    CHECK (d.getProperty ("last_error", "").toString().isNotEmpty());
    CHECK (slotState (one, 0) == "pending");
}

TEST_CASE ("failure states: unauthorised - ACCESS excludes write for this client: the slot's word")
{
    oqatest::MockDevice::Options options;
    options.allowList = juce::StringArray { "203.0.113.1" };
    Rig rig (options);
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "failure-unauthorised");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("failure-unauthorised")), why));
    one.connect();
    rig.waitReady (one);

    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "unauthorised"; }));
    CHECK (slotStatus (one, 0).getProperty ("detail", "").toString().contains ("ACCESS"));
}

TEST_CASE ("failure states: stale - the path is gone after reopen: the slot's word, the old path kept")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "failure-stale");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("failure-stale")), why));
    one.connect();
    rig.waitReady (one);
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));

    rig.waitHeard();
    REQUIRE (rig.mock.remove (gain.fullPath));
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "missing"; }));
    CHECK (one.chunk().slots[0].path == gain.fullPath);
}

TEST_CASE ("failure states: range unknown - a leaf without RANGE: inactive until min and max are entered")
{
    Rig rig;
    const auto& trim = rig.leafWhere (isUnrangedFloat);

    Instance one (api(), 4, "failure-range");
    std::string why;
    REQUIRE (one.bind (0, trim, 0, one.addDevice (rig.device ("failure-range")), why));
    CHECK_FALSE (one.chunk().slots[0].active());
    one.connect();
    rig.waitReady (one);

    CHECK (slotState (one, 0) == "inactive");
    bool said = false;

    for (const auto& w : one.warnings())
        said = said || w.find ("no range") != std::string::npos;

    CHECK (said);

    one.setRange (0, 0.0, 10.0);
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));
}

TEST_CASE ("failure states: echo loop - the device echoes what it is sent within the guard: counted, dropped")
{
    oqatest::MockDevice::Options options;
    options.echoDelayMs = 10;
    Rig rig (options);
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "failure-echo");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("failure-echo")), why));
    one.setDirection (0, Direction::both);
    one.connect();
    rig.waitReady (one);
    rig.settleListen (one, 1);

    float values[4] = { 0.8f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return counter (one, "loop_guarded") >= 1; }));
    CHECK (one.pollReceived ({}) == 0);
}

TEST_CASE ("failure states: dropped sends - the rate limiter holding a path: counted per device")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "failure-dropped");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("failure-dropped")), why));

    const ScopedSetting limit (one, R"({"default_rate_hz": 20})", R"({"default_rate_hz": 0})");
    one.connect();
    rig.waitReady (one);

    for (int block = 0; block < 20; ++block)
    {
        float values[4] = { static_cast<float> (block + 1) / 40.0f, 0.0f, 0.0f, 0.0f };
        one.processBlock (values, 4);
        sleepMs (2);
    }

    REQUIRE (waitUntil ([&] { return counter (one, "rate_limited") >= 1; }));
}
