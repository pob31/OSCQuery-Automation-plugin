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
    RECORD: THE RECEIVE PATH (devplan Phase 4a), driven through core::Instance
    over the static API table against the mock device on loopback. Every bound
    path is LISTENed; a value the device streams arrives at the instance scaled
    to 0-1 by its slot; an fff's arguments no slot drives follow the device;
    a value echoed inside the loop guard's window is dropped and counted; 32
    slots in both directions follow a device streaming at 100 Hz with nothing
    missed and nothing echoed back; a receiving slot without a range says so;
    the audio path still allocates nothing; a saved chunk with receiving slots
    records through the registry.

    NO ADDRESS IS TYPED: every node is found in the fixture by what it is.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>
#include <core/rt/RtCheck.h>

#include <oscquery/tree/Tree.h>

#include <support/TreeGen.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <thread>
#include <utility>
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

    using Received = std::vector<std::pair<int, float>>;

    /** Everything waiting for `instance`, appended to `into`. */
    int poll (Instance& instance, Received& into)
    {
        return instance.pollReceived ([&] (int slot, float value) { into.emplace_back (slot, value); });
    }

    bool isReadOnlyFloat (const Node& n)
    {
        const auto a = n.argument (0);
        return n.type == "f" && ! n.bindableForSend() && n.bindableForRecord() && a.range.has_value() && a.range->hasMinMax();
    }

    bool isToggle (const Node& n)
    {
        return (n.type == "T" || n.type == "F") && n.bindableForSend() && ! n.critical;
    }

    bool isUnrangedFloat (const Node& n)
    {
        return n.type == "f" && n.bindableForSend() && n.argument (0).range.has_value() == false;
    }

    juce::int64 counter (const Instance& instance, const char* name, int deviceIndex = 0)
    {
        return static_cast<juce::int64> (Rig::deviceStatus (instance, deviceIndex).getProperty (name, 0));
    }

    void sleepMs (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }
}

//==============================================================================
TEST_CASE ("receive: every bound path is LISTENed, sending ones included; an unbound one is IGNOREd")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);
    const auto& load = rig.leafWhere (isReadOnlyFloat);

    Instance one (api(), 4, "listen");
    const int device = one.addDevice (rig.device ("listen"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    REQUIRE (one.bind (1, load, 0, device, why));
    CHECK (one.chunk().slots[0].direction == Direction::send);
    CHECK (one.chunk().slots[1].direction == Direction::receive);     // a read-only leaf binds for record

    one.connect();
    rig.waitListening (one);

    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (gain.fullPath) == 1 && rig.mock.listenerCount (load.fullPath) == 1; }));

    one.unbind (1);
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (load.fullPath) == 0; }));
    CHECK (rig.mock.listenerCount (gain.fullPath) == 1);
}

TEST_CASE ("receive: a value the device streams arrives scaled to 0-1 by its slot; a toggle as 0 or 1; a log range by its curve")
{
    Rig rig;
    const auto& load = rig.leafWhere (isReadOnlyFloat);
    const auto& mute = rig.leafWhere (isToggle);
    const auto& trim = rig.leafWhere (isUnrangedFloat);

    Instance one (api(), 4, "scaled");
    const int device = one.addDevice (rig.device ("scaled"));
    std::string why;
    REQUIRE (one.bind (0, load, 0, device, why));
    REQUIRE (one.bind (1, mute, 0, device, why));
    one.setDirection (1, Direction::receive);
    REQUIRE (one.bind (2, trim, 0, device, why));
    one.setRange (2, 20.0, 20000.0);
    one.setScaling (2, Scaling::logarithmic);
    one.setDirection (2, Direction::receive);

    std::atomic<int> wakes { 0 };
    one.setReceiveWake ([&] { ++wakes; });
    CHECK (one.canReceive());

    one.connect();
    CHECK (one.warnings().empty());
    rig.waitListening (one);
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (load.fullPath) == 1 && rig.mock.listenerCount (mute.fullPath) == 1
                                  && rig.mock.listenerCount (trim.fullPath) == 1; }));

    /*  On LISTEN the device's current value arrives once: a receiving slot
        starts where the device is. */
    rig.settleListen (one, 3);
    Received got;
    CHECK (poll (one, got) == 0);
    REQUIRE (one.lastReceived (0).has_value());
    CHECK (*one.lastReceived (0) == doctest::Approx (one.chunk().slots[0].toHost (load.argument (0).value->asDouble())).epsilon (1e-4));

    const auto& loadRange = *load.argument (0).range;
    const double loadUnits = *loadRange.min + 0.375 * (*loadRange.max - *loadRange.min);
    REQUIRE (rig.mock.setValue (load.fullPath, { loadUnits }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    REQUIRE (got.size() == 1);
    CHECK (got[0].first == 0);
    CHECK (got[0].second == doctest::Approx (0.375f).epsilon (1e-4));
    CHECK (one.lastReceived (0).value_or (-1.0f) == doctest::Approx (0.375f).epsilon (1e-4));

    got.clear();
    REQUIRE (rig.mock.setValue (mute.fullPath, { true }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].first == 1);
    CHECK (got[0].second == 1.0f);

    got.clear();
    REQUIRE (rig.mock.setValue (mute.fullPath, { false }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].second == 0.0f);

    got.clear();
    REQUIRE (rig.mock.setValue (trim.fullPath, { 2000.0 }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].first == 2);
    CHECK (got[0].second == doctest::Approx (one.chunk().slots[2].toHost (2000.0)).epsilon (1e-4));
    CHECK (got[0].second == doctest::Approx (std::log (2000.0 / 20.0) / std::log (1000.0)).epsilon (1e-3));

    CHECK (wakes.load() >= 4);
    CHECK (counter (one, "values_received") >= 4);
    CHECK (counter (one, "loop_guarded") == 0);
}

TEST_CASE ("receive: an fff's arguments no slot drives follow what the device streams")
{
    Rig rig;
    const auto& position = rig.leafWhere ([] (const Node& n) { return n.argumentCount() == 3; });

    Instance one (api(), 8, "fff-follow");
    const int device = one.addDevice (rig.device ("fff-follow"));
    std::string why;
    REQUIRE_MESSAGE (one.bindAll (2, position, device, why), why);

    /*  Only the second argument stays bound: the message still carries three. */
    one.unbind (2);
    one.unbind (4);
    one.connect();
    rig.waitReady (one);

    /*  The bindings reached the registry before the tree did: the plan is
        rebuilt when the tree lands, whole this time. */
    REQUIRE (waitUntil ([&]
    {
        return static_cast<int> (juce::JSON::parse (juce::String (one.statusJson())).getProperty ("plans", 0)) >= 2;
    }));
    rig.settleListen (one, 1);

    float values[8] = {};
    values[3] = 0.5f;
    one.processBlock (values, 8);
    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedOsc().empty(); }));

    {
        const auto first = bundlesReceived (rig.mock);
        REQUIRE (first.size() == 1);
        REQUIRE (first[0].elements.size() == 1);
        REQUIRE (first[0].elements[0].args.size() == 3);
        CHECK (first[0].elements[0].args[0].asDouble() == doctest::Approx (position.argument (0).value->asDouble()).epsilon (1e-4));
        CHECK (first[0].elements[0].args[2].asDouble() == doctest::Approx (position.argument (2).value->asDouble()).epsilon (1e-4));
    }

    rig.mock.clearReceivedOsc();
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (position.fullPath) == 1; }));

    /*  The device moves all three; nothing is delivered (no slot receives), but
        the two arguments no slot drives are remembered. */
    const auto before = counter (one, "values_received");
    const auto r0 = *position.argument (0).range;
    const auto r2 = *position.argument (2).range;
    const double x = *r0.min + 0.8 * (*r0.max - *r0.min);
    const double z = *r2.min + 0.3 * (*r2.max - *r2.min);
    REQUIRE (rig.mock.setValue (position.fullPath, { x, 0.0, z }));
    REQUIRE (waitUntil ([&] { return counter (one, "values_received") > before; }));

    Received got;
    CHECK (poll (one, got) == 0);

    values[3] = 0.9f;
    one.processBlock (values, 8);
    REQUIRE (waitUntil ([&] { return ! rig.mock.receivedOsc().empty(); }));

    const auto after = bundlesReceived (rig.mock);
    REQUIRE (after.size() == 1);
    REQUIRE (after[0].elements.size() == 1);
    const auto& m = after[0].elements[0];
    REQUIRE (m.args.size() == 3);
    CHECK (m.args[0].asDouble() == doctest::Approx (x).epsilon (1e-4));
    CHECK (m.args[1].asDouble() == doctest::Approx (one.chunk().slots[3].toDevice (0.9f)).epsilon (1e-4));
    CHECK (m.args[2].asDouble() == doctest::Approx (z).epsilon (1e-4));
}

TEST_CASE ("receive: a value echoed within the guard window is dropped and counted; one echoed after it arrives")
{
    const auto& settings = Registry::shared().settings();
    REQUIRE (settings.loopGuardMs == doctest::Approx (50.0));

    SUBCASE ("an echo 10 ms after the send is the plugin's own value coming back")
    {
        oqatest::MockDevice::Options options;
        options.echoDelayMs = 10;
        Rig rig (options);
        const auto& gain = rig.leafWhere (isRangedFloat);

        Instance one (api(), 4, "guard-near");
        const int device = one.addDevice (rig.device ("guard-near"));
        std::string why;
        REQUIRE (one.bind (0, gain, 0, device, why));
        one.setDirection (0, Direction::both);
        one.connect();
        rig.waitReady (one);
        REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (gain.fullPath) == 1; }));
        rig.settleListen (one, 1);

        /*  The first block posts everything; let its echo come and go. */
        float values[4] = {};
        one.processBlock (values, 4);
        sleepMs (150);
        Received got;
        poll (one, got);
        got.clear();
        const auto guardedBefore = counter (one, "loop_guarded");

        values[0] = 0.4f;
        one.processBlock (values, 4);
        REQUIRE (waitUntil ([&] { return counter (one, "loop_guarded") > guardedBefore; }));
        sleepMs (50);

        CHECK (poll (one, got) == 0);
        CHECK (counter (one, "values_received") >= 1);
    }

    SUBCASE ("an echo 150 ms after the send is the device moving, and is delivered; the host handing it back sends nothing")
    {
        oqatest::MockDevice::Options options;
        options.echoDelayMs = 150;
        Rig rig (options);
        const auto& gain = rig.leafWhere (isRangedFloat);

        Instance one (api(), 4, "guard-far");
        one.setHostHoldMs (0);              // the loop guard alone: the host's hold on a slot it just moved is ReceiveHoldTests' subject
        const int device = one.addDevice (rig.device ("guard-far"));
        std::string why;
        REQUIRE (one.bind (0, gain, 0, device, why));
        one.setDirection (0, Direction::both);
        one.connect();
        rig.waitReady (one);
        REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (gain.fullPath) == 1; }));
        rig.settleListen (one, 1);

        float values[4] = {};
        one.processBlock (values, 4);
        sleepMs (300);
        Received got;
        poll (one, got);
        got.clear();
        const auto guardedBefore = counter (one, "loop_guarded");

        values[0] = 0.6f;
        one.processBlock (values, 4);
        REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }, 2000));
        REQUIRE (got.size() == 1);
        CHECK (got[0].first == 0);
        CHECK (got[0].second == doctest::Approx (0.6f).epsilon (1e-4));
        CHECK (counter (one, "loop_guarded") == guardedBefore);

        /*  What the host hands back is what the device said: not a move. */
        const auto datagrams = rig.mock.receivedOsc().size();
        values[0] = got[0].second;
        one.processBlock (values, 4);
        sleepMs (60);
        CHECK (rig.mock.receivedOsc().size() == datagrams);

        /*  A real move is. */
        values[0] = 0.61f;
        one.processBlock (values, 4);
        REQUIRE (waitUntil ([&] { return rig.mock.receivedOsc().size() > datagrams; }));
    }
}

TEST_CASE ("receive: 32 slots in both directions follow a device streaming at 100 Hz: none missed, none echoed back")
{
    Rig rig ({}, oqatest::generateTree (64, 8));

    std::vector<const Node*> leaves;

    for (const auto* leaf : rig.tree.leaves())
        if (leaf->type == "f" && leaf->bindableForSend())
            leaves.push_back (leaf);

    REQUIRE (leaves.size() >= 32);
    leaves.resize (32);

    Instance one (api(), 32, "stream32");
    const int device = one.addDevice (rig.device ("stream32"));
    std::string why;

    for (int slot = 0; slot < 32; ++slot)
    {
        REQUIRE_MESSAGE (one.bind (slot, *leaves[static_cast<std::size_t> (slot)], 0, device, why), why);
        one.setDirection (slot, Direction::both);
    }

    one.connect();
    rig.waitReady (one);
    rig.waitListening (one);
    REQUIRE (waitUntil ([&]
    {
        for (const auto* leaf : leaves)
            if (rig.mock.listenerCount (leaf->fullPath) != 1)
                return false;

        return true;
    }));
    rig.settleListen (one, 32);

    float values[32] = {};
    one.processBlock (values, 32);                       // where the host stands: nothing is sent for it
    sleepMs (50);
    Received got;
    poll (one, got);
    got.clear();
    const auto datagramsBefore = rig.mock.receivedOsc().size();

    constexpr int rounds = 200;
    std::map<int, std::vector<float>> perSlot;

    const auto collect = [&]
    {
        poll (one, got);

        for (const auto& [slot, value] : got)
            perSlot[slot].push_back (value);

        got.clear();
    };

    for (int round = 1; round <= rounds; ++round)
    {
        std::vector<std::pair<juce::String, juce::Array<juce::var>>> bundle;
        const float normalised = static_cast<float> (round) / rounds;

        for (int slot = 0; slot < 32; ++slot)
            bundle.emplace_back (juce::String (leaves[static_cast<std::size_t> (slot)]->fullPath),
                                 juce::Array<juce::var> { one.chunk().slots[slot].toDevice (normalised) });

        REQUIRE (rig.mock.setValueBundle (bundle));
        sleepMs (10);
        collect();
    }

    REQUIRE (waitUntil ([&]
    {
        collect();
        std::size_t total = 0;

        for (const auto& [slot, seq] : perSlot)
            total += seq.size();

        return total >= static_cast<std::size_t> (32 * rounds);
    }));

    REQUIRE (perSlot.size() == 32);

    for (const auto& [slot, seq] : perSlot)
    {
        INFO ("slot " << slot);
        REQUIRE (seq.size() == static_cast<std::size_t> (rounds));

        for (int i = 0; i < rounds; ++i)
            CHECK (seq[static_cast<std::size_t> (i)] == doctest::Approx (static_cast<float> (i + 1) / rounds).epsilon (1e-4));
    }

    /*  The device moved; the host hands the same values back: not a move, nothing sent. */
    CHECK (rig.mock.receivedOsc().size() == datagramsBefore);

    for (int slot = 0; slot < 32; ++slot)
        values[slot] = one.lastReceived (slot).value_or (0.0f);

    one.processBlock (values, 32);
    sleepMs (60);
    CHECK (rig.mock.receivedOsc().size() == datagramsBefore);
    CHECK (counter (one, "loop_guarded") == 0);

    values[5] = 0.123f;
    one.processBlock (values, 32);
    REQUIRE (waitUntil ([&] { return rig.mock.receivedOsc().size() > datagramsBefore; }));
    const auto moved = bundlesReceived (rig.mock);
    CHECK (messagesIn (moved) == messagesIn (std::vector<osc::Packet> (moved.begin(), moved.begin() + static_cast<std::ptrdiff_t> (datagramsBefore))) + 1);
}

TEST_CASE ("receive: a receiving slot without a range neither sends nor receives, and the warning says so")
{
    Rig rig;
    const auto& trim = rig.leafWhere (isUnrangedFloat);

    Instance one (api(), 4, "norange");
    const int device = one.addDevice (rig.device ("norange"));
    std::string why;
    REQUIRE (one.bind (0, trim, 0, device, why));
    one.setDirection (0, Direction::receive);
    one.connect();
    rig.waitReady (one);

    bool said = false;

    for (const auto& w : one.warnings())
        said = said || w.find ("neither sends nor receives") != std::string::npos;

    CHECK (said);
    CHECK (rig.mock.listenerCount (trim.fullPath) == 0);

    REQUIRE (rig.mock.setValue (trim.fullPath, { 1.0 }));
    sleepMs (100);
    Received got;
    CHECK (poll (one, got) == 0);
}

TEST_CASE ("receive: two instances receiving on one path each get the value")
{
    Rig rig;
    const auto& load = rig.leafWhere (isReadOnlyFloat);

    Instance a (api(), 4, "twice-a");
    Instance b (api(), 4, "twice-b");
    std::string why;
    REQUIRE (a.bind (0, load, 0, a.addDevice (rig.device ("twice")), why));
    REQUIRE (b.bind (2, load, 0, b.addDevice (rig.device ("twice")), why));
    a.connect();
    b.connect();
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (load.fullPath) == 1; }));    // one websocket, two instances
    rig.settleListen (a, 1);
    b.pollReceived ({});

    const auto& range = *load.argument (0).range;
    REQUIRE (rig.mock.setValue (load.fullPath, { *range.min + 0.5 * (*range.max - *range.min) }));

    Received gotA, gotB;
    REQUIRE (waitUntil ([&] { poll (a, gotA); poll (b, gotB); return ! gotA.empty() && ! gotB.empty(); }));
    CHECK (gotA[0].first == 0);
    CHECK (gotB[0].first == 2);
    CHECK (gotA[0].second == doctest::Approx (0.5f).epsilon (1e-4));
    CHECK (gotB[0].second == doctest::Approx (0.5f).epsilon (1e-4));
}

TEST_CASE ("receive: the audio path allocates nothing with receiving slots in the plan")
{
    if (! rt::isCounting())
    {
        MESSAGE ("not counting allocations in this executable");
        return;
    }

    Rig rig;
    const auto& position = rig.leafWhere ([] (const Node& n) { return n.argumentCount() == 3; });
    const auto& gain = rig.leafWhere (isRangedFloat);
    const auto& load = rig.leafWhere (isReadOnlyFloat);

    Instance one (api(), 8, "rt-receive");
    const int device = one.addDevice (rig.device ("rt-receive"));
    std::string why;
    REQUIRE (one.bindAll (0, position, device, why));
    one.setDirection (1, Direction::both);
    REQUIRE (one.bind (5, gain, 0, device, why));
    one.setDirection (5, Direction::both);
    REQUIRE (one.bind (6, load, 0, device, why));
    one.connect();
    rig.waitReady (one);
    rig.settleListen (one, 3);

    float values[8] = {};
    one.processBlock (values, 8);
    sleepMs (20);

    const auto& range = *load.argument (0).range;
    REQUIRE (rig.mock.setValue (load.fullPath, { *range.min + 0.25 * (*range.max - *range.min) }));
    Received got;
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));

    rt::resetCounts();

    {
        rt::ScopedRealtimeCheck region;

        for (int block = 0; block < 500; ++block)
        {
            values[0] = (block % 100) / 100.0f;
            values[1] = 1.0f - values[0];
            values[5] = (block % 7) / 7.0f;
            values[6] = got[0].second;              // the host handing back what was received
            one.processBlock (values, 8);
        }
    }

    CHECK (rt::violations() == 0);
    CHECK (rt::regions() == 1);
}

TEST_CASE ("receive: a saved chunk with receiving slots records through the registry (tests/state/v1-receive.json)")
{
    Rig rig;
    rig.mock.setName ("Generic Mock");             // the fixture's device, as its NAME says (5b)

    /*  The fixture was saved against another address; the session moved. */
    auto saved = juce::JSON::parse (juce::File (juce::String (OQA_TEST_STATE_DIR)).getChildFile ("v1-receive.json"));
    REQUIRE (saved.isObject());
    auto* device = saved["devices"].getArray()->getReference (0).getDynamicObject();
    device->setProperty ("last_host", "127.0.0.1");
    device->setProperty ("last_port", rig.mock.httpPort());

    Instance one (api(), 8, "chunk-receive");
    std::string error;
    REQUIRE_MESSAGE (one.loadState (juce::JSON::toString (saved).toStdString(), error), error);
    REQUIRE (one.chunk().slots[0].direction == Direction::receive);
    REQUIRE (one.chunk().slots[1].direction == Direction::both);
    REQUIRE (one.chunk().slots[2].direction == Direction::send);

    one.connect();
    CHECK (one.warnings().empty());
    rig.waitReady (one);
    rig.waitListening (one);

    const auto& receiving = one.chunk().slots[0];
    const auto& both = one.chunk().slots[1];
    const auto& sending = one.chunk().slots[2];
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (juce::String (receiving.path)) == 1
                                  && rig.mock.listenerCount (juce::String (both.path)) == 1
                                  && rig.mock.listenerCount (juce::String (sending.path)) == 1; }));
    rig.settleListen (one, 3);

    Received got;
    REQUIRE (rig.mock.setValue (juce::String (receiving.path), { receiving.toDevice (0.25f) }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].first == 0);
    CHECK (got[0].second == doctest::Approx (0.25f).epsilon (1e-4));

    got.clear();
    REQUIRE (rig.mock.setValue (juce::String (both.path), { both.toDevice (0.75f) }));
    REQUIRE (waitUntil ([&] { poll (one, got); return ! got.empty(); }));
    CHECK (got[0].first == 1);
    CHECK (got[0].second == doctest::Approx (0.75f).epsilon (1e-4));

    /*  The sending slot's path is LISTENed too, but nothing is delivered for it. */
    got.clear();
    REQUIRE (rig.mock.setValue (juce::String (sending.path), { sending.toDevice (0.5f) }));
    sleepMs (100);
    CHECK (poll (one, got) == 0);
}

//==============================================================================
//  Julien Bayle's retest of ec6725c, 2.2: what the device last sent is not a
//  value the host may never send again.
TEST_CASE ("receive: the host moving back to the value the device last sent is sent; a toggle the device left off can be switched on and off again")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);
    const auto& mute = rig.leafWhere (isToggle);

    Instance one (api(), 4, "back-to-received");
    const int device = one.addDevice (rig.device ("back-to-received"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    REQUIRE (one.bind (1, mute, 0, device, why));
    one.setDirection (0, Direction::both);
    one.setDirection (1, Direction::both);
    one.connect();
    rig.waitReady (one);
    rig.settleListen (one, 2);

    /*  A first move of the float, to have something sent; let it land and
        the host's hold on the slot run out. */
    one.setHostHoldMs (50);
    float values[4] = { 0.25f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= 1; }));
    sleepMs (120);
    Received got;
    poll (one, got);
    got.clear();
    rig.mock.clearReceivedOsc();

    const auto sent = [&] { return messagesIn (bundlesReceived (rig.mock)); };

    /*  The device moves the float; the host follows and hands it back: not a move. */
    const auto range = *gain.argument (0).range;
    const float deviceValue = static_cast<float> (*range.min + 0.5 * (*range.max - *range.min));
    REQUIRE (rig.mock.setValue (gain.fullPath, { static_cast<double> (deviceValue) }));
    REQUIRE (waitUntil ([&] { return poll (one, got) > 0; }));
    const float followed = got.back().second;
    CHECK (followed == doctest::Approx (0.5f).epsilon (0.01));
    values[0] = followed;
    one.processBlock (values, 4);
    sleepMs (80);
    CHECK (sent() == 0);

    /*  Away from it: sent. */
    values[0] = 0.8f;
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return sent() == 1; }));

    /*  And back to exactly what the device had sent: sent too. The device holds 0.8 until it is. */
    sleepMs (80);                                       // past the loop guard, so nothing here is its doing
    values[0] = followed;
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return sent() == 2; }));

    /*  The toggle, which the device says is off. */
    rig.mock.clearReceivedOsc();
    got.clear();
    REQUIRE (rig.mock.setValue (mute.fullPath, { false }));
    sleepMs (120);
    poll (one, got);

    for (int round = 1; round <= 3; ++round)
    {
        values[1] = 1.0f;
        one.processBlock (values, 4);
        REQUIRE (waitUntil ([&] { return sent() == 2 * round - 1; }));
        sleepMs (80);

        values[1] = 0.0f;                               // off again: the value the device last sent
        one.processBlock (values, 4);
        REQUIRE (waitUntil ([&] { return sent() == 2 * round; }));
        sleepMs (80);
    }
}
