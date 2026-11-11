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
    THE REGISTRY AND THE SEND PATH (devplan Phase 3c), driven through the
    plugin side of it (core::Instance over the static API table) against the
    mock device on loopback: two instances make one bundle per block; a
    multi-argument node is one message; bundles split at the MTU; a CRITICAL
    leaf goes to the websocket; a TCP device gets framed bundles; the rate
    limiter holds; the time tag follows the setting; a device that is not
    there is unreachable and nothing is sent; the audio path allocates
    nothing; and the time from post to the wire is measured.

    NO ADDRESS IS TYPED: every node is found in the fixture by what it is.
    Device names are per case, because the registry in this process is one
    and keeps its devices.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>
#include <core/registry/RegistryApi.h>
#include <core/rt/RtCheck.h>
#include <core/send/TimeTags.h>

#include <oscquery/Device.h>
#include <oscquery/osc/Codec.h>
#include <oscquery/osc/Framing.h>
#include <oscquery/tree/Tree.h>

#include <support/TreeGen.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
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

    /*  No thread of ours alive when main returns: the registry's devices are
        stopped after the last case. */
    const bool shutdownRegistered = (oqatest::registerShutdownHook ([] { Registry::shared().shutdown(); }), true);
}

//==============================================================================
TEST_CASE ("registry: after shutdown() an instance registered anew is served again, resolver included")
{
    /*  A CLAP host deinits the binary after a scan and inits it again to use
        the plugins (Phase 5b): the registry it shut down then must confirm a
        device again, which is the resolver's work, so its thread restarts
        with the first instance registered. */
    Registry::shared().shutdown();

    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Instance a (api(), 8, "after-shutdown");
    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (rig.device ("after-shutdown")), why));
    a.connect();
    rig.waitReady (a);

    float values[8] = {};
    values[0] = 0.5f;
    a.processBlock (values, 8);
    CHECK (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= 1; }));
}

TEST_CASE ("registry: two instances on one device make one bundle per block, with both messages")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && ! n.unit.empty() && ! n.unit[0].empty(); });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.fullPath != gain.fullPath && n.argumentCount() == 1; });

    Instance a (api(), 8, "a");
    Instance b (api(), 8, "b");

    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (rig.device ("two-instances")), why));
    REQUIRE (b.bind (3, pan, 0, b.addDevice (rig.device ("two-instances")), why));

    a.connect();
    b.connect();
    CHECK (a.warnings().empty());
    rig.waitReady (a);
    rig.waitReady (b);

    /*  Both slots vouched for by the tree before the count starts: a move made
        while a leaf is still unverified is held until it is, then sent, and
        would land in a later block's bundle (seen on the Mac mini, 49 for 50). */
    const auto everySlotOk = [] (const Instance& instance)
    {
        const auto status = juce::JSON::parse (juce::String (instance.statusJson()));
        const auto* slots = status.getProperty ("slots", {}).getArray();

        if (slots == nullptr || slots->isEmpty())
            return false;

        for (const auto& s : *slots)
            if (s.getProperty ("state", "").toString() != "ok")
                return false;

        return true;
    };

    REQUIRE (waitUntil ([&] { return everySlotOk (a) && everySlotOk (b); }));

    /*  Nothing is sent at connection (decided 2026-11-11): a first block where the host stands still says nothing. */
    float valuesA[8] = {};
    float valuesB[8] = {};
    a.processBlock (valuesA, 8);
    b.processBlock (valuesB, 8);
    std::this_thread::sleep_for (std::chrono::milliseconds (60));
    CHECK (rig.mock.receivedOsc().empty());
    rig.mock.clearReceivedOsc();

    constexpr int blocks = 25;

    for (int block = 1; block <= blocks; ++block)
    {
        valuesA[0] = block / 100.0f;
        valuesB[3] = 1.0f - block / 100.0f;
        a.processBlock (valuesA, 8);
        b.processBlock (valuesB, 8);
        std::this_thread::sleep_for (std::chrono::milliseconds (3));     // a block period, roughly
    }

    /*  Wait for the last block's gain to land, then look. On this machine
        every block is its own bundle; a loaded one (a CI runner) may merge
        two blocks into one bundle, the later value of a slot superseding the
        earlier, so fewer bundles and fewer messages, never more bundles than
        blocks and always two messages per bundle. */
    const auto lastGain = a.chunk().slots[0].toDevice (blocks / 100.0f);

    REQUIRE (waitUntil ([&]
    {
        for (const auto& bundle : bundlesReceived (rig.mock))
            for (const auto& m : bundle.elements)
                if (m.address == gain.fullPath && std::abs (m.args[0].asDouble() - lastGain) < 1e-4)
                    return true;

        return false;
    }));

    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    const auto bundles = bundlesReceived (rig.mock);

    MESSAGE (blocks << " blocks from two instances arrived as " << bundles.size() << " bundles holding "
             << messagesIn (bundles) << " messages");

    /*  A loaded machine (a shared CI runner) may also split a block: the two
        instances' posts straddling the gather deadline make two bundles of one
        message. Exact here and on the Mac mini; the shape holds anywhere. */
    CHECK (bundles.size() >= blocks / 2);
    CHECK (bundles.size() <= blocks + blocks / 4);

    for (const auto& bundle : bundles)
    {
        CHECK (bundle.elements.size() >= 1);
        CHECK (bundle.elements.size() <= 2);
    }

    if (bundles.size() == blocks)
        CHECK (messagesIn (bundles) == 2 * blocks);
    else
        MESSAGE ("not one bundle per block on this machine: " << bundles.size() << " bundles for " << blocks << " blocks");

    for (const auto& bundle : bundles)
    {
        REQUIRE (bundle.isBundle());
        CHECK (bundle.time == immediateTimeTag);

        for (const auto& m : bundle.elements)
        {
            CHECK ((m.address == gain.fullPath || m.address == pan.fullPath));
            REQUIRE (m.args.size() == 1);
            CHECK (m.args[0].isFloat32());
        }
    }

    /*  The last gain value, scaled by the slot: 0.25 of the way across its range. */
    const auto& last = bundles.back();
    const auto& slot = a.chunk().slots[0];
    bool sawGain = false;

    for (const auto& m : last.elements)
        if (m.address == gain.fullPath)
        {
            sawGain = true;
            CHECK (m.args[0].asDouble() == doctest::Approx (slot.toDevice (blocks / 100.0f)).epsilon (1e-4));
        }

    CHECK (sawGain);

    a.disconnect();
    b.disconnect();
}

TEST_CASE ("registry: a multi-argument node is one message, arguments not automated from the tree's value")
{
    Rig rig;
    const auto& position = rig.leafWhere ([] (const Node& n) { return n.argumentCount() == 3; });

    Instance one (api(), 8, "fff");
    const int device = one.addDevice (rig.device ("fff"));

    std::string why;
    REQUIRE_MESSAGE (one.bindAll (2, position, device, why), why);
    one.connect();
    rig.waitReady (one);

    /*  Only the second argument moves; the others start where the device is,
        so the first block sends all three with x and z as the tree said. */
    float values[8] = {};
    values[2] = one.chunk().slots[2].toHost (position.argument (0).value->asDouble());
    values[3] = 0.75f;
    values[4] = one.chunk().slots[4].toHost (position.argument (2).value->asDouble());
    one.processBlock (values, 8);

    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedOsc().empty(); }));
    const auto bundles = bundlesReceived (rig.mock);
    REQUIRE (bundles.size() == 1);
    REQUIRE (bundles[0].elements.size() == 1);

    const auto& m = bundles[0].elements[0];
    CHECK (m.address == position.fullPath);
    REQUIRE (m.args.size() == 3);
    CHECK (m.args[0].asDouble() == doctest::Approx (position.argument (0).value->asDouble()).epsilon (1e-4));
    CHECK (m.args[1].asDouble() == doctest::Approx (one.chunk().slots[3].toDevice (0.75f)).epsilon (1e-4));
    CHECK (m.args[2].asDouble() == doctest::Approx (position.argument (2).value->asDouble()).epsilon (1e-4));

    /*  All three move in one block: still one message. */
    rig.mock.clearReceivedOsc();
    values[2] = 0.1f; values[3] = 0.2f; values[4] = 0.3f;
    one.processBlock (values, 8);

    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedOsc().empty(); }));
    const auto again = bundlesReceived (rig.mock);
    REQUIRE (again.size() == 1);
    REQUIRE (again[0].elements.size() == 1);
    CHECK (again[0].elements[0].args.size() == 3);
    CHECK (again[0].elements[0].args[0].asDouble() == doctest::Approx (one.chunk().slots[2].toDevice (0.1f)).epsilon (1e-4));
}

TEST_CASE ("registry: a block of many changes splits into bundles under the MTU")
{
    Rig rig ({}, oqatest::generateTree (64, 8));

    std::vector<const Node*> leaves;

    for (const auto* leaf : rig.tree.leaves())
        if (leaf->type == "f" && leaf->bindableForSend())
            leaves.push_back (leaf);

    REQUIRE (leaves.size() >= 32);

    Instance one (api(), 32, "mtu");
    const int device = one.addDevice (rig.device ("mtu"));
    std::string why;

    for (int slot = 0; slot < 32; ++slot)
        REQUIRE_MESSAGE (one.bind (slot, *leaves[static_cast<std::size_t> (slot)], 0, device, why), why);

    auto settings = Registry::shared().settings();
    settings.mtu = 200;
    Registry::shared().setSettings (settings);

    one.connect();
    rig.waitReady (one);

    float values[32];

    //  Every slot moved from where it stood (nought), so every one is sent.
    for (int i = 0; i < 32; ++i)
        values[i] = (i + 1) / 33.0f;

    one.processBlock (values, 32);

    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= 32; }));
    const auto bundles = bundlesReceived (rig.mock);

    MESSAGE ("32 messages at an MTU of 200 bytes arrived as " << bundles.size() << " bundles");
    CHECK (bundles.size() > 1);
    CHECK (messagesIn (bundles) == 32);

    for (const auto& datagram : rig.mock.receivedOsc())
        CHECK (datagram.size() <= 200);

    settings.mtu = 1400;
    Registry::shared().setSettings (settings);
}

TEST_CASE ("registry: a CRITICAL leaf on a UDP device goes over the websocket, not the wire")
{
    Rig rig;
    const auto& standby = rig.leafWhere ([] (const Node& n) { return n.critical && n.bindableForSend(); });

    Instance one (api(), 4, "critical");
    const int device = one.addDevice (rig.device ("critical"));
    std::string why;
    REQUIRE_MESSAGE (one.bind (1, standby, 0, device, why), why);
    one.connect();
    rig.waitReady (one);

    REQUIRE (waitUntil ([&] { return rig.mock.connectionCount() >= 1; }));

    float values[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);

    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedWebsocketOsc().empty(); }));
    std::this_thread::sleep_for (std::chrono::milliseconds (30));

    const auto frames = rig.mock.receivedWebsocketOsc();
    REQUIRE (frames.size() == 1);
    const auto decoded = osc::decode (frames[0].data(), frames[0].size());
    REQUIRE (decoded.ok);
    CHECK_FALSE (decoded.packet.isBundle());
    CHECK (decoded.packet.address == standby.fullPath);
    REQUIRE (decoded.packet.args.size() == 1);
    CHECK (decoded.packet.args[0].isBool());
    CHECK (decoded.packet.args[0].getBool());

    CHECK (rig.mock.receivedOsc().empty());
}

TEST_CASE ("registry: a device whose OSC_TRANSPORT is TCP receives SLIP-framed bundles on one connection")
{
    oqatest::MockDevice::Options options;
    options.oscTransport = "TCP";
    Rig rig (options);

    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "tcp");
    const int device = one.addDevice (rig.device ("tcp"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);

    float values[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);

    REQUIRE (waitUntil ([&] { return rig.mock.receivedOscStream().size() > 16; }));
    std::this_thread::sleep_for (std::chrono::milliseconds (20));

    osc::Deframer deframer (osc::Framing::slip);
    deframer.feed (rig.mock.receivedOscStream());

    const auto packet = deframer.next();
    REQUIRE (packet.has_value());
    const auto decoded = osc::decode (packet->data(), packet->size());
    REQUIRE (decoded.ok);
    REQUIRE (decoded.packet.isBundle());
    REQUIRE (decoded.packet.elements.size() == 1);
    CHECK (decoded.packet.elements[0].address == gain.fullPath);
    CHECK (decoded.packet.elements[0].args[0].asDouble() == doctest::Approx (one.chunk().slots[0].toDevice (0.5f)).epsilon (1e-4));
}

TEST_CASE ("registry: the rate limiter holds a path to its rate and the last value still arrives")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "limited");
    const int device = one.addDevice (rig.device ("limited"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));

    auto settings = Registry::shared().settings();
    settings.defaultRateHz = 20.0;       // one every 50 ms
    Registry::shared().setSettings (settings);

    one.connect();
    rig.waitReady (one);

    float values[4] = {};
    one.processBlock (values, 4);                        // where the host stands: nothing is sent for it
    rig.mock.clearReceivedOsc();

    /*  Forty changes about a millisecond apart: at 20 Hz, one per 50 ms
        window that the forty took, and the first. A loaded machine (a CI
        runner) stretches the forty, so the allowance is read off the clock. */
    const auto started = std::chrono::steady_clock::now();

    for (int i = 1; i <= 40; ++i)
    {
        values[0] = i / 40.0f;
        one.processBlock (values, 4);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    const auto tookMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - started).count();
    const auto allowed = static_cast<std::size_t> (tookMs / 50.0) + 2;

    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    const auto soon = messagesIn (bundlesReceived (rig.mock));
    INFO ("the forty changes took " << tookMs << " ms");
    CHECK (soon <= allowed);

    /*  Then the held value, the last one, comes through. */
    REQUIRE (waitUntil ([&]
    {
        const auto bundles = bundlesReceived (rig.mock);
        return ! bundles.empty() && bundles.back().elements.size() == 1
            && std::abs (bundles.back().elements[0].args[0].asDouble() - one.chunk().slots[0].toDevice (1.0f)) < 1e-3;
    }));

    MESSAGE ("40 changes in " << tookMs << " ms at 20 Hz arrived as " << messagesIn (bundlesReceived (rig.mock)) << " messages");
    CHECK (messagesIn (bundlesReceived (rig.mock)) <= allowed + 1);

    settings.defaultRateHz = 0.0;
    Registry::shared().setSettings (settings);
}

TEST_CASE ("registry: a device that honours time tags gets NTP now plus its offset; the rest get immediate")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "timetags");
    const int device = one.addDevice (rig.device ("timetags"));
    one.setDeviceTiming (device, true, -12.5);
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);

    const auto before = unixNanosNow();
    float values[4] = { 0.3f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);

    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedOsc().empty(); }));
    const auto after = unixNanosNow();

    const auto bundles = bundlesReceived (rig.mock);
    REQUIRE (bundles.size() == 1);
    CHECK (bundles[0].time != immediateTimeTag);

    const auto tagged = unixNanosFromNtp (bundles[0].time);
    const auto offset = -12'500'000LL;
    CHECK (tagged >= before + offset - 1'000'000LL);
    CHECK (tagged <= after + offset + 1'000'000LL);

    /*  Off again: immediate. */
    one.setDeviceTiming (device, false, 0.0);
    rig.mock.clearReceivedOsc();
    values[0] = 0.6f;
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedOsc().empty(); }));
    CHECK (bundlesReceived (rig.mock)[0].time == immediateTimeTag);
}

TEST_CASE ("registry: a device that is not there is unreachable, nothing is sent, and pointing it right fixes every instance")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    /*  A port nothing answers on: a mock started and stopped. */
    int deadPort = 0;
    {
        oqatest::MockDevice dead;
        REQUIRE (dead.loadTree (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json"))));
        REQUIRE (dead.start());
        deadPort = dead.httpPort();
        dead.stop();
    }

    DeviceRef ref;
    ref.name = "moved-device";
    ref.lastHost = "127.0.0.1";
    ref.lastPort = deadPort;

    Instance a (api(), 4, "moved-a");
    Instance b (api(), 4, "moved-b");
    std::string why;
    REQUIRE (a.bind (0, gain, 0, a.addDevice (ref), why));
    REQUIRE (b.bind (0, gain, 0, b.addDevice (ref), why));
    a.connect();
    b.connect();

    REQUIRE (waitUntil ([&]
    {
        const auto status = juce::JSON::parse (juce::String (a.statusJson()));
        const auto* devices = status.getProperty ("devices", {}).getArray();
        return devices != nullptr && ! devices->isEmpty()
            && (*devices)[0].getProperty ("state", "").toString() == "unreachable";
    }));

    float values[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
    a.processBlock (values, 4);
    b.processBlock (values, 4);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    {
        const auto status = juce::JSON::parse (juce::String (a.statusJson()));
        const auto& d = (*status.getProperty ("devices", {}).getArray())[0];
        CHECK_FALSE (static_cast<bool> (d.getProperty ("sender_ready", true)));
        CHECK (static_cast<int> (d.getProperty ("unsent_no_route", 0)) >= 2);
        CHECK (static_cast<int> (d.getProperty ("bundles_sent", 0)) == 0);
    }

    /*  Pointed once, every instance follows. */
    REQUIRE (Registry::shared().setDeviceAddress ("moved-device", "", "127.0.0.1", rig.mock.httpPort()));
    rig.waitReady (a);
    rig.waitReady (b);

    values[0] = 0.25f;
    a.processBlock (values, 4);
    values[0] = 0.75f;
    b.processBlock (values, 4);

    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= 2; }));
}

TEST_CASE ("registry: the audio path allocates nothing, with and without the direct send")
{
    if (! rt::isCounting())
    {
        MESSAGE ("not counting allocations in this executable");
        return;
    }

    Rig rig;
    const auto& position = rig.leafWhere ([] (const Node& n) { return n.argumentCount() == 3; });
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 8, "rt");
    const int device = one.addDevice (rig.device ("rt"));
    std::string why;
    REQUIRE (one.bindAll (0, position, device, why));
    REQUIRE (one.bind (5, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);

    float values[8] = {};
    one.processBlock (values, 8);        // the first block, everything
    std::this_thread::sleep_for (std::chrono::milliseconds (20));

    for (bool direct : { false, true })
    {
        auto settings = Registry::shared().settings();
        settings.directSend = direct;
        Registry::shared().setSettings (settings);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

        rt::resetCounts();

        {
            rt::ScopedRealtimeCheck region;

            for (int block = 0; block < 500; ++block)
            {
                values[0] = (block % 100) / 100.0f;
                values[1] = 1.0f - values[0];
                values[5] = (block % 7) / 7.0f;
                one.processBlock (values, 8);
            }
        }

        INFO ("direct send " << (direct ? "on" : "off"));
        CHECK (rt::violations() == 0);
        CHECK (rt::regions() == 1);
    }

    auto settings = Registry::shared().settings();
    settings.directSend = false;
    Registry::shared().setSettings (settings);

    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    MESSAGE ("1000 blocks posted; the mock received " << rig.mock.receivedOsc().size() << " datagrams");
}

TEST_CASE ("registry: from the post to the wire on loopback, measured")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "latency");
    const int device = one.addDevice (rig.device ("latency"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);

    float values[4] = {};
    one.processBlock (values, 4);                        // where the host stands: nothing is sent for it
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    rig.mock.clearReceivedOsc();

    constexpr int blocks = 200;
    std::vector<std::chrono::steady_clock::time_point> posted;
    posted.reserve (blocks);

    for (int block = 1; block <= blocks; ++block)
    {
        values[0] = block / 300.0f;
        posted.push_back (std::chrono::steady_clock::now());
        one.processBlock (values, 4);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= blocks; }));
    const auto arrived = rig.mock.receivedOscTimes();

    if (arrived.size() != blocks)
    {
        /*  Two blocks landed in one bundle somewhere: the pairing below would
            lie. The count is the finding; the latency waits for a quieter run. */
        MESSAGE (blocks << " blocks arrived as " << arrived.size() << " bundles; latency not paired this run");
        return;
    }

    std::vector<double> latencyUs;

    for (int i = 0; i < blocks; ++i)
        latencyUs.push_back (std::chrono::duration<double, std::micro> (arrived[static_cast<std::size_t> (i)] - posted[static_cast<std::size_t> (i)]).count());

    std::sort (latencyUs.begin(), latencyUs.end());
    const auto median = latencyUs[blocks / 2];
    const auto p99 = latencyUs[blocks * 99 / 100];

    MESSAGE ("post to the mock's socket on loopback, " << blocks << " blocks: median " << median
             << " us, p99 " << p99 << " us, max " << latencyUs.back() << " us (gather deadline "
             << Registry::shared().settings().gatherDeadlineUs << " us; includes the receiver's own wake)");

    /*  The PRD's target is under a millisecond to the wire; the gather deadline
        is half of it and the receiving thread's wake is in this number too. */
    CHECK (median < 2000.0);
}

//==============================================================================
/*  AGAINST A LIVE DEVICE, when OQA_LIVE_DEVICE names one as host:port (the
    author's WFS-DIY: 127.0.0.1:5005). Not a CI case: the device is not there.
    The first three writable, ranged, per-channel float leaves of its tree
    (a numeric segment in the path), whatever they are called, are bound to
    three slots, driven for twenty blocks, read back over HTTP, and put back.
    No address is typed; the record goes in docs/bench. */
TEST_CASE ("registry: a live device, when OQA_LIVE_DEVICE names one")
{
    const auto target = juce::SystemStats::getEnvironmentVariable ("OQA_LIVE_DEVICE", {});

    if (target.isEmpty())
    {
        MESSAGE ("OQA_LIVE_DEVICE not set; skipped");
        return;
    }

    const auto host = target.upToLastOccurrenceOf (":", false, false).toStdString();
    const int port = target.fromLastOccurrenceOf (":", false, false).getIntValue();
    REQUIRE (port > 0);

    /*  The tree, straight from the device, to choose leaves by what they are. */
    oqa::oscquery::Device probe (oqa::oscquery::Device::Options { host, port });
    REQUIRE_MESSAGE (probe.probe(), probe.lastError());
    REQUIRE_MESSAGE (probe.fetchTree(), probe.lastError());
    const auto tree = probe.tree();
    const auto info = probe.hostInfo();
    REQUIRE (info.has_value());

    std::vector<const Node*> chosen;

    for (const auto* leaf : tree.leaves())
    {
        if (! isRangedFloat (*leaf) || leaf->argumentCount() != 1)
            continue;

        const auto path = juce::String (leaf->fullPath);
        bool numeric = false;

        for (const auto& segment : juce::StringArray::fromTokens (path, "/", {}))
            numeric = numeric || (segment.isNotEmpty() && segment.containsOnly ("0123456789"));

        if (numeric)
            chosen.push_back (leaf);

        if (chosen.size() == 3)
            break;
    }

    REQUIRE (chosen.size() == 3);

    std::vector<double> before;

    for (const auto* leaf : chosen)
    {
        const auto value = probe.readValue (leaf->fullPath);
        REQUIRE (value.has_value());
        REQUIRE (! value->empty());
        before.push_back ((*value)[0].asDouble());
    }

    Instance one (api(), 4, "live");
    DeviceRef ref;
    ref.name = info->name;
    ref.lastHost = host;
    ref.lastPort = port;
    const int device = one.addDevice (ref);

    std::string why;

    for (int i = 0; i < 3; ++i)
        REQUIRE_MESSAGE (one.bind (i, *chosen[static_cast<std::size_t> (i)], 0, device, why), why);

    one.connect();

    REQUIRE (waitUntil ([&]
    {
        const auto status = juce::JSON::parse (juce::String (one.statusJson()));
        const auto* devices = status.getProperty ("devices", {}).getArray();
        return devices != nullptr && ! devices->isEmpty() && static_cast<bool> ((*devices)[0].getProperty ("sender_ready", false));
    }));

    /*  Twenty blocks, each slot on its own ramp, ending at 0.37, 0.61 and 0.84 of its range. */
    float values[4] = {};
    const float ends[3] = { 0.37f, 0.61f, 0.84f };

    for (int block = 1; block <= 20; ++block)
    {
        for (int i = 0; i < 3; ++i)
            values[i] = ends[i] * block / 20.0f;

        one.processBlock (values, 4);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    for (int i = 0; i < 3; ++i)
    {
        const auto* leaf = chosen[static_cast<std::size_t> (i)];
        const auto value = probe.readValue (leaf->fullPath);
        REQUIRE (value.has_value());
        const auto expected = one.chunk().slots[i].toDevice (ends[i]);
        INFO (leaf->fullPath << " expected " << expected << " read " << (*value)[0].asDouble());
        CHECK ((*value)[0].asDouble() == doctest::Approx (expected).epsilon (1e-3));
        MESSAGE ("live: " << leaf->fullPath << " = " << (*value)[0].asDouble() << " (sent " << expected << ")");
    }

    const auto status = juce::JSON::parse (juce::String (one.statusJson()));
    const auto& d = (*status.getProperty ("devices", {}).getArray())[0];
    MESSAGE ("live: " << info->name << " at " << d.getProperty ("resolved", "").toString().toStdString()
             << ", " << static_cast<int> (d.getProperty ("bundles_sent", 0)) << " bundles, "
             << static_cast<int> (d.getProperty ("messages_sent", 0)) << " messages, "
             << static_cast<int> (d.getProperty ("datagrams_dropped", 0)) << " dropped");

    /*  Put the device back as it was, through the same path. */
    for (int i = 0; i < 3; ++i)
        values[i] = one.chunk().slots[i].toHost (before[static_cast<std::size_t> (i)]);

    one.processBlock (values, 4);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    for (int i = 0; i < 3; ++i)
    {
        const auto value = probe.readValue (chosen[static_cast<std::size_t> (i)]->fullPath);
        REQUIRE (value.has_value());
        CHECK ((*value)[0].asDouble() == doctest::Approx (before[static_cast<std::size_t> (i)]).epsilon (1e-3));
    }

    one.disconnect();
}
