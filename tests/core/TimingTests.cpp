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
    A DEVICE'S TIMING, PROCESS-WIDE (devplan Phase 6a-3; PRD, Transport:
    "a per-device setting the user ticks in the Monitor", "set once per rig"):
    set by hand from one instance (or the Monitor) it stands when another
    instance pushes its chunk, and both chunks carry it on their next save; a
    chunk's own timing is taken until a hand has spoken.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/StateChunk.h>
#include <core/registry/Registry.h>

#include <oscquery/HostInfo.h>

#include <string>

using namespace oqa::core;
using namespace oqa::oscquery;
using oqatest::Rig;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    const bool shutdownRegistered = (oqatest::registerShutdownHook ([] { Registry::shared().shutdown(); }), true);

    juce::var deviceStatus (const Instance& instance)
    {
        return juce::JSON::parse (juce::String (instance.statusJson())).getProperty ("devices", {})[0];
    }

    StateChunk saved (Instance& instance)
    {
        std::string error;
        auto chunk = StateChunk::fromJson (instance.saveState(), instance.slotCount(), error);
        REQUIRE_MESSAGE (chunk.has_value(), error);
        return *chunk;
    }
}

//==============================================================================
TEST_CASE ("timing: set by hand from one instance it stands when another pushes its chunk, and both save it")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });

    Instance a (api(), 8, "timing-a");
    Instance b (api(), 8, "timing-b");
    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (rig.device ("timing")), why));
    REQUIRE (b.bind (1, pan, 0, b.addDevice (rig.device ("timing")), why));
    a.connect();
    b.connect();
    rig.waitReady (a);

    /*  A chunk's word, nobody's hand yet. */
    CHECK (deviceStatus (a).getProperty ("timing_by_hand", true) == juce::var (false));
    CHECK (deviceStatus (a).getProperty ("honours_timetags", true) == juce::var (false));

    /*  One hand: the device honours timetags, 12.5 ms early. */
    a.setDeviceTiming (0, true, -12.5);
    CHECK (deviceStatus (a).getProperty ("timing_by_hand", false) == juce::var (true));
    CHECK (deviceStatus (b).getProperty ("honours_timetags", false) == juce::var (true));
    CHECK (static_cast<double> (deviceStatus (b).getProperty ("offset_ms", 0.0)) == doctest::Approx (-12.5));

    /*  The other instance pushes its chunk, whose device says the opposite:
        the hand stands. */
    REQUIRE (b.bind (2, gain, 0, 0, why));
    CHECK (deviceStatus (b).getProperty ("honours_timetags", false) == juce::var (true));
    CHECK (static_cast<double> (deviceStatus (a).getProperty ("offset_ms", 0.0)) == doctest::Approx (-12.5));

    /*  Both chunks carry it on their next save. */
    const auto chunkA = saved (a);
    const auto chunkB = saved (b);
    CHECK (chunkA.devices[0].honoursTimetags);
    CHECK (chunkA.devices[0].offsetMs == doctest::Approx (-12.5));
    CHECK (chunkB.devices[0].honoursTimetags);
    CHECK (chunkB.devices[0].offsetMs == doctest::Approx (-12.5));

    /*  Another hand, from the other side, replaces it. */
    b.setDeviceTiming (0, false, 3.0);
    CHECK (deviceStatus (a).getProperty ("honours_timetags", true) == juce::var (false));
    CHECK (static_cast<double> (deviceStatus (a).getProperty ("offset_ms", 0.0)) == doctest::Approx (3.0));
    CHECK (saved (a).devices[0].offsetMs == doctest::Approx (3.0));
}

TEST_CASE ("timing: through the registry's own call, as the Monitor sets it, with no instance of its own")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Instance a (api(), 8, "timing-monitor");
    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (rig.device ("timing-monitor")), why));
    a.connect();
    rig.waitReady (a);

    CHECK (Registry::shared().setDeviceTiming ("timing-monitor", "", true, 7.25));
    CHECK_FALSE (Registry::shared().setDeviceTiming ("no such device", "", true, 0.0));

    CHECK (deviceStatus (a).getProperty ("timing_by_hand", false) == juce::var (true));
    CHECK (static_cast<double> (deviceStatus (a).getProperty ("offset_ms", 0.0)) == doctest::Approx (7.25));
    CHECK (saved (a).devices[0].honoursTimetags);
    CHECK (saved (a).devices[0].offsetMs == doctest::Approx (7.25));
}

//==============================================================================
//  Phase 6x-5: the optional CLOCK (docs/spec/extensions-draft-0.1.md). A device
//  that declares SCHEDULING says itself whether it honours time tags; a hand
//  wins over it, and it wins over a chunk.
namespace
{
    /** A mock serving CLOCK with this SCHEDULING, declared in EXTENSIONS or not. */
    oqatest::MockDevice::Options withClock (bool scheduling, bool declared = true)
    {
        ClockInfo clock;
        clock.source = "ptp";
        clock.domain = "0";
        clock.scheduling = scheduling;

        oqatest::MockDevice::Options options;
        auto* extras = new juce::DynamicObject();
        extras->setProperty ("CLOCK", clock.toVar());
        options.hostInfoExtras = juce::var (extras);

        if (declared)
            options.extensions.add ("CLOCK");

        return options;
    }

    std::string origin (const Instance& instance)
    {
        return deviceStatus (instance).getProperty ("timetags_origin", "").toString().toStdString();
    }

    bool honours (const Instance& instance)
    {
        return static_cast<bool> (deviceStatus (instance).getProperty ("honours_timetags", false));
    }

    bool confirmed (const Instance& instance)
    {
        return deviceStatus (instance).getProperty ("resolution", "").toString() == "confirmed";
    }
}

TEST_CASE ("clock: a device declaring SCHEDULING ticks honours timetags without a hand, the bundles carry a time, and the chunk saves it as its own word")
{
    Rig rig (withClock (true));
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Instance a (api(), 8, "clock-declared");
    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (rig.device ("clock-declared")), why));
    CHECK_FALSE (a.chunk().devices[0].honoursTimetags);
    a.connect();

    REQUIRE (oqatest::waitUntil ([&] { return origin (a) == "device"; }));
    CHECK (honours (a));
    CHECK (deviceStatus (a).getProperty ("timing_by_hand", true) == juce::var (false));

    const auto clock = deviceStatus (a).getProperty ("clock", {});
    CHECK (clock.getProperty ("source", "").toString() == "ptp");
    CHECK (clock.getProperty ("domain", "").toString() == "0");
    CHECK (clock.getProperty ("scheduling", false) == juce::var (true));

    /*  What goes out is tagged with a time, not "immediately". */
    rig.waitReady (a);
    float values[8] = { 0.4f };
    a.processBlock (values, 8);
    REQUIRE (oqatest::waitUntil ([&] { return ! oqatest::bundlesReceived (rig.mock).empty(); }));
    CHECK (oqatest::bundlesReceived (rig.mock).front().time.raw != 1);

    /*  Saved as the value, with no hand claimed for it. */
    const auto chunk = saved (a);
    CHECK (chunk.devices[0].honoursTimetags);
    CHECK_FALSE (chunk.devices[0].timetagsByHand);
    CHECK (a.saveState().find ("timetags_by_hand") == std::string::npos);

    /*  The chunk pushed again (a bind) says false still held in nobody's hand: the device stands. */
    const auto& pan = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });
    REQUIRE (a.bind (1, pan, 0, 0, why));
    CHECK (honours (a));
    CHECK (origin (a) == "device");
}

TEST_CASE ("clock: a hand wins over what the device declares, a chunk's push does not undo it, and it is saved as a hand's")
{
    Rig rig (withClock (true));
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });

    Instance a (api(), 8, "clock-hand-a");
    Instance b (api(), 8, "clock-hand-b");
    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (rig.device ("clock-hand")), why));
    REQUIRE (b.bind (1, pan, 0, b.addDevice (rig.device ("clock-hand")), why));
    a.connect();
    b.connect();

    {
        //  Failed once in a dozen full runs on the Mac mini, unexplained: the status is printed if it does again.
        const bool declared = oqatest::waitUntil ([&] { return origin (a) == "device" && honours (a); }, 20000);
        INFO (a.statusJson());
        REQUIRE (declared);
    }

    /*  Unticked by hand, with an offset: the device claims to schedule and the user knows better. */
    a.setDeviceTiming (0, false, 4.0);
    CHECK_FALSE (honours (b));
    CHECK (origin (b) == "hand");

    /*  The other instance pushes its chunk; the hand stands. */
    REQUIRE (b.bind (2, gain, 0, 0, why));
    CHECK_FALSE (honours (a));

    /*  Both save it, and say a hand set it. */
    CHECK_FALSE (saved (a).devices[0].honoursTimetags);
    CHECK (saved (a).devices[0].timetagsByHand);
    CHECK (saved (b).devices[0].timetagsByHand);
    CHECK (saved (b).devices[0].offsetMs == doctest::Approx (4.0));
}

TEST_CASE ("clock: a session whose timetags a hand set stands over the device on reload; one that only carries a value gives way to it")
{
    /*  Saved by a hand in an earlier run: unticked, against a device that claims to schedule. */
    {
        Rig rig (withClock (true));
        auto ref = rig.device ("clock-reload-hand");
        ref.honoursTimetags = false;
        ref.timetagsByHand = true;
        ref.offsetMs = -2.0;

        Instance a (api(), 8, "clock-reload-hand");
        std::string why;
        REQUIRE (a.bind (0, rig.leafWhere (isRangedFloat), 0, a.addDevice (ref), why));
        a.connect();

        REQUIRE (oqatest::waitUntil ([&] { return confirmed (a) && ! deviceStatus (a).getProperty ("clock", {}).isVoid(); }));
        CHECK_FALSE (honours (a));
        CHECK (origin (a) == "saved_hand");
        CHECK (saved (a).devices[0].timetagsByHand);
        CHECK (saved (a).devices[0].offsetMs == doctest::Approx (-2.0));
    }

    /*  A chunk that says true with no hand behind it, a device that says it does not schedule. */
    {
        Rig rig (withClock (false));
        auto ref = rig.device ("clock-reload-value");
        ref.honoursTimetags = true;
        ref.offsetMs = 6.5;

        Instance a (api(), 8, "clock-reload-value");
        std::string why;
        REQUIRE (a.bind (0, rig.leafWhere (isRangedFloat), 0, a.addDevice (ref), why));
        a.connect();

        REQUIRE (oqatest::waitUntil ([&] { return origin (a) == "device"; }));
        CHECK_FALSE (honours (a));

        //  The offset is the session's: a device never sets it.
        CHECK (static_cast<double> (deviceStatus (a).getProperty ("offset_ms", 0.0)) == doctest::Approx (6.5));
        CHECK_FALSE (saved (a).devices[0].honoursTimetags);
        CHECK (saved (a).devices[0].offsetMs == doctest::Approx (6.5));
    }
}

TEST_CASE ("clock: a CLOCK that EXTENSIONS does not declare changes nothing, and a device with none is as before")
{
    /*  Served, not declared. */
    {
        Rig rig (withClock (true, false));
        Instance a (api(), 8, "clock-undeclared");
        std::string why;
        REQUIRE (a.bind (0, rig.leafWhere (isRangedFloat), 0, a.addDevice (rig.device ("clock-undeclared")), why));
        a.connect();

        REQUIRE (oqatest::waitUntil ([&] { return confirmed (a); }));
        CHECK_FALSE (honours (a));
        CHECK (origin (a) == "chunk");
        CHECK (deviceStatus (a).getProperty ("clock", "x").isVoid());
    }

    /*  No CLOCK at all: the chunk's word, and the immediate tag on the wire. */
    {
        Rig rig;
        Instance a (api(), 8, "clock-none");
        std::string why;
        REQUIRE (a.bind (0, rig.leafWhere (isRangedFloat), 0, a.addDevice (rig.device ("clock-none")), why));
        a.connect();

        REQUIRE (oqatest::waitUntil ([&] { return confirmed (a); }));
        CHECK (origin (a) == "chunk");
        CHECK (deviceStatus (a).getProperty ("clock", "x").isVoid());

        rig.waitReady (a);
        float values[8] = { 0.4f };
        a.processBlock (values, 8);
        REQUIRE (oqatest::waitUntil ([&] { return ! oqatest::bundlesReceived (rig.mock).empty(); }));
        CHECK (oqatest::bundlesReceived (rig.mock).front().time.raw == 1);

        const auto json = a.saveState();
        CHECK (json.find ("timetags_by_hand") == std::string::npos);
        CHECK (json.find ("device_id") == std::string::npos);
    }
}
