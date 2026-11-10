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
    STALE AND UNAUTHORISED SLOTS (devplan Phase 5a; PRD "Reopening a session"
    step 3 and the failure-states table), driven through core::Instance against
    the mock device on loopback. A bound path the device renames, removes or
    retypes shows stale in the instance's status, sends nothing and is LISTENed
    no more, its lane untouched; a path added back is live again; a chunk
    reopened against a tree missing one of its paths has that slot stale and
    the rest live; the chunk's tree_hash against the live tree is one warning
    line, cleared by a rebind's hash; ACCESS as reported to this client makes a
    sending slot unauthorised, which still receives.

    NO ADDRESS IS TYPED: every node is found in the fixture by what it is, and
    a moved path is spelled from the node's own.
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
using oqatest::bundlesReceived;
using oqatest::messagesIn;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    void sleepMs (int ms) { std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }

    /** The status entry of slot `id` in `instance`'s `slots`, or void. */
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

    std::vector<std::string> statusWarnings (const Instance& instance)
    {
        std::vector<std::string> out;
        const auto status = juce::JSON::parse (juce::String (instance.statusJson()));

        if (const auto* warnings = status.getProperty ("warnings", {}).getArray())
            for (const auto& w : *warnings)
                out.push_back (w.toString().toStdString());

        return out;
    }

    bool anyWarningContains (const Instance& instance, const std::string& text)
    {
        for (const auto& w : statusWarnings (instance))
            if (w.find (text) != std::string::npos)
                return true;

        return false;
    }

    /** `root` (a `GET /` tree) without the node at `path`; false if absent. */
    bool removeFromTree (juce::var& root, const std::string& path)
    {
        juce::var node = root;
        std::size_t start = 1;

        while (start <= path.size())
        {
            const auto end = path.find ('/', start);
            const auto segment = juce::String (path.substr (start, end == std::string::npos ? std::string::npos : end - start));
            auto* contents = node.getProperty ("CONTENTS", {}).getDynamicObject();

            if (contents == nullptr || ! contents->hasProperty (segment))
                return false;

            if (end == std::string::npos)
            {
                contents->removeProperty (segment);
                return true;
            }

            node = contents->getProperty (segment);
            start = end + 1;
        }

        return false;
    }
}

//==============================================================================
TEST_CASE ("stale: a path the device renames shows renamed with where it went; nothing is sent there or to the new path; the LISTEN is gone")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "stale-rename");
    const int device = one.addDevice (rig.device ("stale-rename"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);
    rig.waitListening (one);
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));
    CHECK (static_cast<juce::int64> (Rig::deviceStatus (one).getProperty ("last_seen_ms", -1)) >= 0);

    const auto moved = gain.fullPath + "-moved";
    REQUIRE (rig.mock.rename (gain.fullPath, moved));
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "renamed"; }));

    const auto s = slotStatus (one, 0);
    CHECK (s.getProperty ("renamed_to", "").toString().toStdString() == moved);
    CHECK (s.getProperty ("detail", "").toString().contains ("moved to"));

    /*  The lane keeps its binding; the registry sends nothing for it. */
    CHECK (one.chunk().slots[0].path == gain.fullPath);
    rig.mock.clearReceivedOsc();
    float values[4] = { 0.8f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    sleepMs (150);
    CHECK (rig.mock.receivedOsc().empty());
    CHECK (rig.mock.listenerCount (gain.fullPath) == 0);
    CHECK (rig.mock.listenerCount (moved) == 0);
}

TEST_CASE ("stale: a path the device removes shows missing; added back, the slot is live again and sends")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);
    const auto saved = rig.mock.node (gain.fullPath);
    REQUIRE (saved.isObject());

    Instance one (api(), 4, "stale-remove");
    const int device = one.addDevice (rig.device ("stale-remove"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);
    rig.waitListening (one);
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));

    REQUIRE (rig.mock.remove (gain.fullPath));
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "missing"; }));
    CHECK (slotStatus (one, 0).getProperty ("detail", "").toString().isNotEmpty());

    rig.mock.clearReceivedOsc();
    float values[4] = { 0.8f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    sleepMs (150);
    CHECK (rig.mock.receivedOsc().empty());

    /*  The device grows the path back (PATH_ADDED): ok again, LISTENed again, sent again. */
    REQUIRE (rig.mock.add (gain.fullPath, saved));
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));
    REQUIRE (waitUntil ([&] { return rig.mock.listenerCount (gain.fullPath) == 1; }));

    values[0] = 0.3f;
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= 1; }));
    const auto bundles = bundlesReceived (rig.mock);
    REQUIRE_FALSE (bundles.empty());
    const auto& message = bundles.back().isBundle() ? bundles.back().elements.back() : bundles.back();
    CHECK (message.address == gain.fullPath);
}

TEST_CASE ("stale: a path the device retypes shows retyped, naming both types")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    Instance one (api(), 4, "stale-retype");
    const int device = one.addDevice (rig.device ("stale-retype"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);
    rig.waitListening (one);                    // PATH_CHANGED goes to the websocket: sent before it is open, it is never heard
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok"; }));

    REQUIRE (rig.mock.retype (gain.fullPath, "i"));
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "retyped"; }));
    const auto detail = slotStatus (one, 0).getProperty ("detail", "").toString();
    CHECK (detail.contains ("'f'"));
    CHECK (detail.contains ("'i'"));

    rig.mock.clearReceivedOsc();
    float values[4] = { 0.8f, 0.0f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    sleepMs (150);
    CHECK (rig.mock.receivedOsc().empty());
}

TEST_CASE ("stale: a chunk reopened against a tree missing one of its paths has that slot stale and the others live")
{
    /*  Two ranged floats of the fixture, the second to go missing. */
    std::string parseError;
    const auto fixtureTree = Tree::parse (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json")), parseError);
    REQUIRE_MESSAGE (fixtureTree.has_value(), parseError);

    std::vector<const Node*> floats;

    for (const auto* leaf : fixtureTree->leaves())
        if (isRangedFloat (*leaf))
            floats.push_back (leaf);

    REQUIRE (floats.size() >= 2);
    const Node kept = *floats[0];
    const Node gone = *floats[1];

    std::string chunkJson;

    {
        Rig full;
        Instance bound (api(), 4, "stale-reopen-bind");
        const int device = bound.addDevice (full.device ("stale-reopen"));
        std::string why;
        REQUIRE (bound.bind (0, kept, 0, device, why));
        REQUIRE (bound.bind (1, gone, 0, device, why));
        chunkJson = bound.saveState();
    }

    /*  The device the session reopens against lost one path. */
    auto root = juce::JSON::parse (oqatest::fixture ("trees/generic-device.json"));
    REQUIRE (removeFromTree (root, gone.fullPath));
    Rig shrunk ({}, root);
    shrunk.mock.setName ("stale-reopen");          // the same device, as its NAME says (5b)

    auto saved = juce::JSON::parse (juce::String (chunkJson));
    auto* device = saved["devices"].getArray()->getReference (0).getDynamicObject();
    device->setProperty ("last_port", shrunk.mock.httpPort());

    Instance one (api(), 4, "stale-reopen");
    std::string error;
    REQUIRE_MESSAGE (one.loadState (juce::JSON::toString (saved).toStdString(), error), error);
    one.connect();
    shrunk.waitReady (one);
    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "ok" && slotState (one, 1) == "missing"; }));

    /*  Both lanes move; one message, for the live slot. */
    shrunk.mock.clearReceivedOsc();
    float values[4] = { 0.6f, 0.6f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (shrunk.mock)) >= 1; }));
    sleepMs (100);
    const auto bundles = bundlesReceived (shrunk.mock);
    CHECK (messagesIn (bundles) == 1);
    const auto& message = bundles.front().isBundle() ? bundles.front().elements.front() : bundles.front();
    CHECK (message.address == kept.fullPath);

    /*  The chunk still holds the stale binding: nothing was unbound for the user. */
    CHECK (one.chunk().slots[1].path == gone.fullPath);
}

TEST_CASE ("stale: the chunk's tree_hash against the live tree is one warning line, cleared by the hash of a rebind")
{
    Rig rig;
    const auto& gain = rig.leafWhere (isRangedFloat);

    auto ref = rig.device ("tree-hash");
    ref.treeHash = "not-the-hash-of-this-tree";

    Instance one (api(), 4, "tree-hash");
    const int device = one.addDevice (ref);
    std::string why;
    REQUIRE (one.bind (0, gain, 0, device, why));
    one.connect();
    rig.waitReady (one);

    REQUIRE (waitUntil ([&] { return anyWarningContains (one, "the tree changed since the bind"); }));
    auto status = Rig::deviceStatus (one);
    CHECK (static_cast<bool> (status.getProperty ("tree_changed", false)));
    const auto live = status.getProperty ("tree_hash_live", "").toString().toStdString();
    CHECK (live.size() == 64);
    CHECK (live == rig.tree.treeHash());

    /*  The editor rebinding writes the hash of the tree it shows. */
    one.setDeviceTreeHash (device, live);
    REQUIRE (waitUntil ([&] { return ! anyWarningContains (one, "the tree changed since the bind"); }));
    CHECK_FALSE (static_cast<bool> (Rig::deviceStatus (one).getProperty ("tree_changed", true)));
    CHECK (one.chunk().devices[0].treeHash == live);
}

TEST_CASE ("stale: ACCESS as reported to this client: a leaf the device lets us only read makes a sending slot unauthorised, which still receives")
{
    /*  The same fixture served by two devices: one whose allow-list has this
        client, one whose has not. Both clients sit on loopback, so the list
        is what tells them apart. */
    oqatest::MockDevice::Options stranger;
    stranger.name = "restricted";
    stranger.allowList = juce::StringArray { "203.0.113.1" };
    Rig restricted (stranger);

    oqatest::MockDevice::Options friendly;
    friendly.name = "allowed";
    friendly.allowList = juce::StringArray { "127.0.0.1" };
    Rig allowed (friendly);

    const auto& gain = restricted.leafWhere (isRangedFloat);

    Instance one (api(), 4, "access");
    const int restrictedDevice = one.addDevice (restricted.device ("restricted"));
    const int allowedDevice = one.addDevice (allowed.device ("allowed"));
    std::string why;
    REQUIRE (one.bind (0, gain, 0, restrictedDevice, why));
    one.setDirection (0, Direction::both);
    REQUIRE (one.bind (1, gain, 0, allowedDevice, why));
    one.connect();
    restricted.waitReady (one, restrictedDevice);
    allowed.waitReady (one, allowedDevice);

    REQUIRE (waitUntil ([&] { return slotState (one, 0) == "unauthorised" && slotState (one, 1) == "ok"; }));
    CHECK (slotStatus (one, 0).getProperty ("detail", "").toString().contains ("read-only"));

    /*  The device's own tree still says 3: it is this client it refuses. */
    CHECK (static_cast<int> (restricted.mock.node (gain.fullPath).getProperty ("ACCESS", 0)) == 3);
    const auto served = juce::JSON::parse (juce::String (one.deviceTreeJson (restrictedDevice, gain.fullPath).value_or ("{}")));
    CHECK (static_cast<int> (served.getProperty ("ACCESS", 0)) == 1);

    /*  Nothing sent to the restricted device; the allowed one gets the move. */
    restricted.mock.clearReceivedOsc();
    allowed.mock.clearReceivedOsc();
    float values[4] = { 0.7f, 0.7f, 0.0f, 0.0f };
    one.processBlock (values, 4);
    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (allowed.mock)) >= 1; }));
    sleepMs (100);
    CHECK (restricted.mock.receivedOsc().empty());

    /*  Still received from: the restricted device moving the leaf reaches the both slot. */
    restricted.waitListening (one, restrictedDevice);
    restricted.settleListen (one, 1, restrictedDevice);
    const auto& range = *gain.argument (0).range;
    REQUIRE (restricted.mock.setValue (gain.fullPath, { *range.min + 0.25 * (*range.max - *range.min) }));
    std::vector<std::pair<int, float>> got;
    REQUIRE (waitUntil ([&] { one.pollReceived ([&] (int s, float v) { got.emplace_back (s, v); }); return ! got.empty(); }));
    CHECK (got.front().first == 0);
    CHECK (got.front().second == doctest::Approx (0.25f).epsilon (1e-3));
}
