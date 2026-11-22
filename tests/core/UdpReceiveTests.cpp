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
    RECORD FROM DEVICES WITHOUT LISTEN (devplan Phase 4b; PRD: "the plugin
    opens a UDP listener and shows its port in the editor; the user enters that
    port in the device's own configuration"). The mock in HTTP-only mode is
    told the registry's port, as a user would tell a device, and its values
    arrive exactly as over LISTEN; two such devices on one machine are told
    apart by path, and what both hold goes to both, counted; a device whose
    websocket streams has its UDP copies ignored; the setting moves the port.

    NO ADDRESS IS TYPED: every node is found in the fixture by what it is.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>

#include <oscquery/tree/Tree.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace oqa::core;
using namespace oqa::oscquery;
using oqatest::waitUntil;
using oqatest::Rig;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    using Received = std::vector<std::pair<int, float>>;

    int poll (Instance& instance, Received& into)
    {
        return instance.pollReceived ([&] (int slot, float value) { into.emplace_back (slot, value); });
    }

    bool isReadOnlyFloat (const Node& n)
    {
        const auto a = n.argument (0);
        return n.type == "f" && ! n.bindableForSend() && n.bindableForRecord() && a.range.has_value() && a.range->hasMinMax();
    }

    juce::var topStatus (const Instance& instance)
    {
        return juce::JSON::parse (juce::String (instance.statusJson()));
    }

    int receivePort (const Instance& instance)
    {
        return static_cast<int> (topStatus (instance).getProperty ("receive_port", 0));
    }

    juce::int64 counter (const Instance& instance, const char* name, int deviceIndex = 0)
    {
        return static_cast<juce::int64> (Rig::deviceStatus (instance, deviceIndex).getProperty (name, 0));
    }

    double unitsAt (const Node& leaf, double fraction)
    {
        const auto range = *leaf.argument (0).range;    // a copy: argument() returns by value
        return *range.min + fraction * (*range.max - *range.min);
    }

    /** A device without websocket, and honest about it: no LISTEN, no PATH_* in EXTENSIONS. */
    oqatest::MockDevice::Options httpOnly()
    {
        oqatest::MockDevice::Options options;
        options.websocket = false;
        options.extensions = { "ACCESS", "VALUE", "RANGE", "DESCRIPTION", "TYPE", "UNIT", "CLIPMODE", "CRITICAL" };
        return options;
    }

    /** Wait until the mock is reachable from the instance and its tree is in: no websocket to wait for. */
    void waitTree (const Rig& rig, const Instance& instance, int deviceIndex = 0)
    {
        rig.waitReady (instance, deviceIndex);
        REQUIRE (waitUntil ([&] { return Rig::deviceStatus (instance, deviceIndex).getProperty ("listen", "").toString() == "unsupported"; }));
    }
}

//==============================================================================
TEST_CASE ("udp: a device without LISTEN records through the registry's UDP port once told it")
{
    Rig rig (httpOnly());
    const auto& load = rig.leafWhere (isReadOnlyFloat);

    Instance one (api(), 4, "udp-one");
    const int device = one.addDevice (rig.device ("udp-one"));
    std::string why;
    REQUIRE (one.bind (0, load, 0, device, why));
    CHECK (one.chunk().slots[0].direction == Direction::receive);
    one.connect();
    waitTree (rig, one);

    /*  The port the editor shows, with this host's addresses beside it. */
    const int port = receivePort (one);
    REQUIRE (port > 0);
    MESSAGE ("the registry receives on UDP " << port << "; this host's addresses: "
             << juce::JSON::toString (topStatus (one).getProperty ("receive_addresses", {}), true));

    /*  Nothing yet: the device has not been told. */
    REQUIRE (rig.mock.setValue (load.fullPath, { unitsAt (load, 0.1) }));
    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    Received got;
    CHECK (poll (one, got) == 0);
    CHECK (counter (one, "udp_received") == 0);

    /*  Told, as a user types the port into the device. */
    rig.mock.setFeedbackTarget ("127.0.0.1", port);
    REQUIRE (rig.mock.setValue (load.fullPath, { unitsAt (load, 0.375) }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    REQUIRE (got.size() == 1);
    CHECK (got[0].first == 0);
    CHECK (got[0].second == doctest::Approx (0.375f).epsilon (1e-4));
    CHECK (counter (one, "udp_received") == 1);
    CHECK (counter (one, "values_received") == 1);
    CHECK (counter (one, "udp_ignored") == 0);
    CHECK (static_cast<int> (topStatus (one).getProperty ("udp_unrouted", -1)) == 0);

    /*  A bundle from the device, as setValueBundle sends one. */
    got.clear();
    REQUIRE (rig.mock.setValueBundle ({ { load.fullPath, { unitsAt (load, 0.5) } } }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].second == doctest::Approx (0.5f).epsilon (1e-4));
}

TEST_CASE ("udp: two devices without LISTEN on one machine: a value goes to the device whose path it names, or to both when both hold it")
{
    Rig first (httpOnly());
    Rig second (httpOnly());
    const auto& load = first.leafWhere (isReadOnlyFloat);
    const auto& gain = first.leafWhere (isRangedFloat);

    Instance a (api(), 4, "udp-two-a");
    Instance b (api(), 4, "udp-two-b");
    std::string why;
    REQUIRE (a.bind (0, load, 0, a.addDevice (first.device ("udp-two-first")), why));
    REQUIRE (b.bind (0, gain, 0, b.addDevice (second.device ("udp-two-second")), why));
    b.setDirection (0, Direction::receive);
    a.connect();
    b.connect();
    waitTree (first, a);
    waitTree (second, b);

    const int port = receivePort (a);
    REQUIRE (port > 0);
    CHECK (receivePort (b) == port);              // one receiver per process
    first.mock.setFeedbackTarget ("127.0.0.1", port);
    second.mock.setFeedbackTarget ("127.0.0.1", port);

    Received gotA, gotB;
    REQUIRE (first.mock.setValue (load.fullPath, { unitsAt (load, 0.2) }));
    REQUIRE (waitUntil ([&] { poll (a, gotA); return ! gotA.empty(); }));
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    CHECK (poll (b, gotB) == 0);
    CHECK (gotA[0].second == doctest::Approx (0.2f).epsilon (1e-4));

    gotA.clear();
    REQUIRE (second.mock.setValue (gain.fullPath, { unitsAt (gain, 0.6) }));
    REQUIRE (waitUntil ([&] { poll (b, gotB); return ! gotB.empty(); }));
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    CHECK (poll (a, gotA) == 0);
    CHECK (gotB[0].second == doctest::Approx (0.6f).epsilon (1e-4));
    CHECK (static_cast<int> (topStatus (a).getProperty ("udp_ambiguous", -1)) == 0);

    /*  Both devices hold the same path (they serve the same fixture, as two
        units of one product would) and both are on this machine: the sender's
        address tells nothing apart, so both get it, and it is counted. */
    REQUIRE (b.bind (1, load, 0, 0, why));
    gotA.clear();
    gotB.clear();
    REQUIRE (first.mock.setValue (load.fullPath, { unitsAt (load, 0.9) }));
    REQUIRE (waitUntil ([&] { poll (a, gotA); poll (b, gotB); return ! gotA.empty() && ! gotB.empty(); }));
    CHECK (gotA[0].first == 0);
    CHECK (gotB[0].first == 1);
    CHECK (gotA[0].second == doctest::Approx (0.9f).epsilon (1e-4));
    CHECK (gotB[0].second == doctest::Approx (0.9f).epsilon (1e-4));
    CHECK (static_cast<int> (topStatus (a).getProperty ("udp_ambiguous", 0)) >= 1);
}

TEST_CASE ("udp: a device whose websocket streams has its UDP copies ignored, LISTEN being its way")
{
    Rig rig;
    const auto& load = rig.leafWhere (isReadOnlyFloat);

    Instance one (api(), 4, "udp-listening");
    const int device = one.addDevice (rig.device ("udp-listening"));
    std::string why;
    REQUIRE (one.bind (0, load, 0, device, why));
    one.connect();
    rig.waitReady (one);
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (load.fullPath) == 1; }));
    rig.settleListen (one, 1);

    const int port = receivePort (one);
    REQUIRE (port > 0);
    rig.mock.setFeedbackTarget ("127.0.0.1", port);

    /*  Over the websocket and over UDP at once: delivered once. */
    Received got;
    REQUIRE (rig.mock.setValue (load.fullPath, { unitsAt (load, 0.3) }));
    REQUIRE (waitUntil ([&] { return counter (one, "udp_ignored") >= 1; }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    std::this_thread::sleep_for (std::chrono::milliseconds (60));
    poll (one, got);
    CHECK (got.size() == 1);
    CHECK (got[0].second == doctest::Approx (0.3f).epsilon (1e-4));
    CHECK (counter (one, "udp_received") == 0);
}

TEST_CASE ("udp: the receive_port setting moves the receiver; 0 gives it an ephemeral port again")
{
    Rig rig (httpOnly());
    const auto& load = rig.leafWhere (isReadOnlyFloat);

    Instance one (api(), 4, "udp-port");
    const int device = one.addDevice (rig.device ("udp-port"));
    std::string why;
    REQUIRE (one.bind (0, load, 0, device, why));
    one.connect();
    waitTree (rig, one);

    const int ephemeral = receivePort (one);
    REQUIRE (ephemeral > 0);

    /*  A port known to be free: one the system just handed out and took back. */
    int chosen = 0;
    {
        juce::DatagramSocket probe;
        REQUIRE (probe.bindToPort (0));
        chosen = probe.getBoundPort();
    }
    REQUIRE (chosen > 0);

    auto settings = Registry::shared().settings();
    settings.receivePort = chosen;
    Registry::shared().setSettings (settings);
    CHECK (receivePort (one) == chosen);
    CHECK (static_cast<int> (topStatus (one)["settings"].getProperty ("receive_port", 0)) == chosen);

    /*  And it works there. */
    rig.mock.setFeedbackTarget ("127.0.0.1", chosen);
    Received got;
    REQUIRE (rig.mock.setValue (load.fullPath, { unitsAt (load, 0.7) }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].second == doctest::Approx (0.7f).epsilon (1e-4));

    settings.receivePort = 0;
    Registry::shared().setSettings (settings);
    const int again = receivePort (one);
    CHECK (again > 0);
    CHECK (again != chosen);
}
