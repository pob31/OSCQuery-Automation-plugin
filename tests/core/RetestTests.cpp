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
    DEFECTS FOUND BY JULIEN BAYLE'S RETEST OF ec6725c (2026-11-11), one case
    each, by the number his report gives it:

      2.4   a server restarted on the same HTTP port with another OSC port
            was still sent to at the old one
      2.7   an ACCESS that changed across a restart was ignored
      2.8   with the incoming ring full, the newest value was the one lost
      2.12  a toggle was sent again at every host move, its state unchanged

    A device that restarts is a mock stopped and started on a port of the
    test's choosing, so its HTTP address stays and the rest is new.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace oqa::core;
using namespace oqa::oscquery;
using oqatest::waitUntil;
using oqatest::Rig;
using oqatest::bundlesReceived;
using oqatest::messagesIn;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    const bool shutdownRegistered = (oqatest::registerShutdownHook ([] { Registry::shared().shutdown(); }), true);

    void sleepMs (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }

    /** A port free a moment ago: a mock started and stopped. */
    int freePort()
    {
        oqatest::MockDevice probe;
        REQUIRE (probe.loadTree (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json"))));
        REQUIRE (probe.start());
        const int port = probe.httpPort();
        probe.stop();
        return port;
    }

    oqatest::MockDevice::Options onPort (int port)
    {
        oqatest::MockDevice::Options options;
        options.httpPort = port;
        return options;
    }

    /*  Stop the mock, leave it down for `downMs`, and start it again where it
        was; true once it serves there. Down for a second, as a server that
        restarts is, the client's reconnection fails at least once before it
        succeeds: another path through the library than a quick restart's. */
    bool restart (Rig& rig, int port, int downMs = 0)
    {
        rig.mock.stop();
        sleepMs (downMs);

        for (int tries = 0; tries < 50; ++tries)
        {
            if (rig.mock.start() && rig.mock.httpPort() == port)
                return true;

            rig.mock.stop();
            sleepMs (100);
        }

        return false;
    }

    bool isToggle (const Node& n)
    {
        return (n.type == "T" || n.type == "F") && n.bindableForSend() && ! n.critical;
    }

    std::string slotState (const Instance& instance, int id)
    {
        const auto status = juce::JSON::parse (juce::String (instance.statusJson()));

        if (const auto* slots = status.getProperty ("slots", {}).getArray())
            for (const auto& s : *slots)
                if (static_cast<int> (s.getProperty ("id", -1)) == id)
                    return s.getProperty ("state", "").toString().toStdString();

        return {};
    }

    int sent (const Rig& rig) { return messagesIn (bundlesReceived (rig.mock)); }
}

//==============================================================================
TEST_CASE ("retest 2.4: a device restarted on the same HTTP port with another OSC port is sent to at the new one")
{
    const int port = freePort();
    Rig rig (onPort (port));
    REQUIRE (rig.mock.httpPort() == port);
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "retest-osc-port");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("retest-osc-port")), why));
    one.connect();
    rig.waitReady (one);
    rig.waitHeard();

    float values[4] = { 0.3f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return sent (rig) >= 1; }));

    /*  The restart: the same HTTP port, an OSC port of the system's choosing. */
    const int oscBefore = rig.mock.oscPort();
    int oscAfter = oscBefore;

    for (int tries = 0; oscAfter == oscBefore && tries < 5; ++tries)
    {
        REQUIRE (restart (rig, port));
        oscAfter = rig.mock.oscPort();
    }

    REQUIRE (oscAfter != oscBefore);
    rig.mock.clearReceivedOsc();

    /*  The plugin finds it again by itself. */
    REQUIRE (waitUntil ([&] { return rig.mock.connectionCount() >= 1; }, 30000));
    REQUIRE (waitUntil ([&] { return static_cast<int> (Rig::deviceStatus (one).getProperty ("osc_port", 0)) == oscAfter; }, 30000));

    /*  Three moves, as the report makes: every one arrives. */
    for (int move = 1; move <= 3; ++move)
    {
        values[0] = 0.3f + 0.1f * static_cast<float> (move);
        one.processBlock (values, 4);
        REQUIRE (waitUntil ([&] { return sent (rig) >= move; }));
    }
}

TEST_CASE ("retest 2.7: an ACCESS the device takes away across a restart makes the slot unauthorised, and nothing more is sent")
{
    const int port = freePort();
    Rig rig (onPort (port));
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "retest-access");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("retest-access")), why));
    one.connect();
    rig.waitReady (one);
    rig.waitHeard();
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));

    float values[4] = { 0.3f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return sent (rig) >= 1; }));

    /*  The device now lets nobody write (every leaf reads ACCESS 1 to this
        client), and restarts, down for a second and a half as in the report. */
    rig.mock.setAllowList (juce::StringArray { "192.0.2.99" });
    REQUIRE (restart (rig, port, 1500));
    rig.mock.clearReceivedOsc();

    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "unauthorised"; }, 30000));

    values[0] = 0.7f;
    one.processBlock (values, 4);
    sleepMs (300);
    CHECK (sent (rig) == 0);
    CHECK (rig.mock.receivedWebsocketOsc().empty());

    /*  Given back, across another restart: live again. */
    rig.mock.setAllowList (std::nullopt);
    REQUIRE (restart (rig, port, 1500));
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }, 30000));
}

TEST_CASE ("retest 2.8: when more values arrive than the ring holds before anyone reads them, the last one read is the newest")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);
    const auto range = *gain.argument (0).range;

    Instance one (api(), 4, "retest-ring");
    std::string why;
    REQUIRE (one.bind (0, gain, 0, one.addDevice (rig.device ("retest-ring")), why));
    one.setDirection (0, Direction::receive);
    one.connect();
    rig.waitReady (one);
    rig.settleListen (one, 1);

    /*  Six thousand values, nobody polling: the ring holds 4096. The last is
        the top of the range; the ones before it climb to nine tenths. */
    constexpr int count = 6000;
    const auto before = static_cast<juce::int64> (Rig::deviceStatus (one).getProperty ("values_received", 0));

    for (int i = 0; i < count; ++i)
    {
        const double fraction = i == count - 1 ? 1.0 : 0.9 * static_cast<double> (i) / static_cast<double> (count);
        REQUIRE (rig.mock.setValue (gain.fullPath, { *range.min + fraction * (*range.max - *range.min) }));
    }

    REQUIRE (waitUntil ([&]
    {
        return static_cast<juce::int64> (Rig::deviceStatus (one).getProperty ("values_received", 0)) - before >= count;
    }, 60000));

    int polled = 0;
    float last = -1.0f;
    polled = one.pollReceived ([&] (int, float value) { last = value; });

    CHECK (polled > 4000);                      // the ring's worth, and what was held for after it
    CHECK (polled < count);                     // not everything: the ring did overflow
    CHECK (last == doctest::Approx (1.0f));     // and the newest came last

    //  Nothing is left behind for a later poll to bring back.
    CHECK (one.pollReceived ([] (int, float) {}) == 0);

    /*  The next value, with room in the ring, arrives as ever. */
    REQUIRE (rig.mock.setValue (gain.fullPath, { *range.min + 0.25 * (*range.max - *range.min) }));
    REQUIRE (waitUntil ([&] { return one.pollReceived ([&] (int, float value) { last = value; }) > 0; }));
    CHECK (last == doctest::Approx (0.25f).epsilon (0.01));
}

TEST_CASE ("retest 2.12: a toggle is sent when its state changes, not at every host move; and again after the device changed it itself")
{
    Rig rig;
    const auto& mute = rig.leafWhere (isToggle);

    Instance one (api(), 4, "retest-toggle");
    std::string why;
    REQUIRE (one.bind (0, mute, 0, one.addDevice (rig.device ("retest-toggle")), why));
    one.setDirection (0, Direction::both);
    one.connect();
    rig.waitReady (one);
    rig.settleListen (one, 1);

    const auto post = [&] (float value)
    {
        float values[4] = { value, 0.0f, 0.0f, 0.0f };
        one.processBlock (values, 4);
        sleepMs (120);                          // a flush, and past the loop guard
        one.pollReceived ([] (int, float) {});
    };

    /*  The first block posts everything: off, once. */
    post (0.0f);
    REQUIRE (waitUntil ([&] { return sent (rig) == 1; }));

    /*  Three moves below the half: the state has not changed. */
    post (0.2f);
    post (0.4f);
    post (0.49f);
    CHECK (sent (rig) == 1);

    /*  Across the half: on, once; and further up says nothing more. */
    post (0.6f);
    REQUIRE (waitUntil ([&] { return sent (rig) == 2; }));
    post (0.9f);
    post (1.0f);
    CHECK (sent (rig) == 2);

    post (0.1f);
    REQUIRE (waitUntil ([&] { return sent (rig) == 3; }));

    /*  The device switches it on itself. The host, following, hands that back
        (not a move); then moves to off: the device holds on, so off is sent. */
    REQUIRE (rig.mock.setValue (mute.fullPath, { true }));
    REQUIRE (waitUntil ([&] { return one.pollReceived ([] (int, float) {}) > 0; }));
    post (1.0f);
    CHECK (sent (rig) == 3);
    post (0.3f);
    REQUIRE (waitUntil ([&] { return sent (rig) == 4; }));
}
