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
    ONE DEVICE SESSION AGAINST THE MOCK (devplan Phase 2: "LISTEN / IGNORE,
    value stream, PATH_CHANGED, PATH_ADDED, PATH_REMOVED, PATH_RENAMED ...
    reconnect with backoff, re-LISTEN on reconnect. Features gated by
    EXTENSIONS, never assumed").

    Every wait is bounded (oqatest::waitUntil, 10 s) and every case ends by
    stopping the device before the mock goes, as a plugin instance does before
    its host unloads it. No address is typed: paths come from the fixture.
*/

#include <doctest/doctest.h>

#include "TestSupport.h"
#include "MockDevice.h"
#include "support/WsTestClient.h"

#include <oscquery/Device.h>

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace oqa::oscquery;
using oqatest::waitUntil;

namespace
{
    const std::string loopback = "127.0.0.1";

    juce::var loadFixture()
    {
        return juce::JSON::parse (oqatest::fixture ("trees/generic-device.json"));
    }

    void collect (const juce::var& node, std::vector<juce::var>& out)
    {
        out.push_back (node);

        if (auto* contents = node.getProperty ("CONTENTS", {}).getDynamicObject())
            for (const auto& child : contents->getProperties())
                collect (child.value, out);
    }

    std::vector<juce::var> nodesOf (const juce::var& root)
    {
        std::vector<juce::var> nodes;
        collect (root, nodes);
        return nodes;
    }

    std::string pathOf (const juce::var& node) { return node.getProperty ("FULL_PATH", {}).toString().toStdString(); }
    std::string typeOf (const juce::var& node) { return node.getProperty ("TYPE", {}).toString().toStdString(); }
    bool isLeaf (const juce::var& node) { return node.hasProperty ("TYPE"); }
    bool isContainer (const juce::var& node) { return ! isLeaf (node) && node.hasProperty ("CONTENTS"); }

    juce::var first (const juce::var& root, const std::function<bool (const juce::var&)>& what,
                     const char* description)
    {
        for (const auto& node : nodesOf (root))
            if (what (node))
                return node;

        FAIL ("the fixture has no " << description);
        return {};
    }

    //  The n-th node matching, for cases that need two different leaves.
    juce::var nth (const juce::var& root, const std::function<bool (const juce::var&)>& what, int n,
                   const char* description)
    {
        for (const auto& node : nodesOf (root))
            if (what (node) && n-- == 0)
                return node;

        FAIL ("the fixture has too few " << description);
        return {};
    }

    bool isFloatLeaf (const juce::var& n) { return typeOf (n) == "f" && n.hasProperty ("VALUE"); }

    struct Rig
    {
        juce::var fixture { loadFixture() };
        oqatest::MockDevice device;
        bool started = false;

        explicit Rig (oqatest::MockDevice::Options options = {}) : device (std::move (options))
        {
            started = device.loadTree (fixture) && device.start();
        }

        int port() const { return device.httpPort(); }
    };

    //  What the callbacks saw, under a mutex, since they fire on the Device's thread.
    struct Seen
    {
        std::mutex mutex;
        std::vector<std::pair<std::string, osc::Values>> values;
        std::vector<Device::State> states;
        std::vector<std::string> treeChanged, removed, errors;
        std::vector<std::pair<std::string, std::string>> renamed;

        Device::Callbacks callbacks()
        {
            Device::Callbacks c;
            c.onValue = [this] (const std::string& p, const osc::Values& v) { std::lock_guard<std::mutex> l (mutex); values.emplace_back (p, v); };
            c.onState = [this] (Device::State s) { std::lock_guard<std::mutex> l (mutex); states.push_back (s); };
            c.onTreeChanged = [this] (const std::string& p) { std::lock_guard<std::mutex> l (mutex); treeChanged.push_back (p); };
            c.onPathRemoved = [this] (const std::string& p) { std::lock_guard<std::mutex> l (mutex); removed.push_back (p); };
            c.onPathRenamed = [this] (const std::string& a, const std::string& b) { std::lock_guard<std::mutex> l (mutex); renamed.emplace_back (a, b); };
            c.onError = [this] (const std::string& m) { std::lock_guard<std::mutex> l (mutex); errors.push_back (m); };
            return c;
        }

        std::size_t valueCount()        { std::lock_guard<std::mutex> l (mutex); return values.size(); }
        std::size_t treeChangedCount()  { std::lock_guard<std::mutex> l (mutex); return treeChanged.size(); }
        std::size_t removedCount()      { std::lock_guard<std::mutex> l (mutex); return removed.size(); }
        std::size_t renamedCount()      { std::lock_guard<std::mutex> l (mutex); return renamed.size(); }

        bool sawState (Device::State s)
        {
            std::lock_guard<std::mutex> l (mutex);
            return std::find (states.begin(), states.end(), s) != states.end();
        }

        std::pair<std::string, osc::Values> lastValue()
        {
            std::lock_guard<std::mutex> l (mutex);
            return values.empty() ? std::pair<std::string, osc::Values> {} : values.back();
        }
    };

    Device::Options optionsFor (int port)
    {
        Device::Options options;
        options.host = loopback;
        options.port = port;
        options.reconnectInitialMs = 100;
        options.reconnectMaxMs = 1000;
        return options;
    }

    bool reaches (Device& device, Device::State state)
    {
        return waitUntil ([&] { return device.state() == state; });
    }
}

//==============================================================================
TEST_CASE ("device: probe, fetch and read block on the caller's thread and never throw")
{
    INFO ("locale in effect: " << std::string (oqatest::appliedLocaleName()));

    Rig rig;
    REQUIRE (rig.started);

    Device device (optionsFor (rig.port()));
    CHECK (device.state() == Device::State::Unprobed);
    CHECK_FALSE (device.hostInfo().has_value());
    CHECK_FALSE (device.treeLoaded());

    const auto before = device.revision();

    REQUIRE (device.probe());
    CHECK (device.state() == Device::State::Reachable);
    REQUIRE (device.hostInfo().has_value());
    CHECK (device.hostInfo()->name == rig.device.options().name.toStdString());
    CHECK (device.revision() > before);

    REQUIRE (device.fetchTree());
    CHECK (device.state() == Device::State::TreeLoaded);
    CHECK (device.treeLoaded());
    CHECK_FALSE (device.treeIsLarge());
    CHECK (device.tree().nodeCount() == nodesOf (rig.fixture).size());

    const auto leaf = first (rig.fixture, isFloatLeaf, "f leaf with a value");
    const auto values = device.readValue (pathOf (leaf));
    REQUIRE (values.has_value());
    REQUIRE (values->size() == 1);
    CHECK ((*values)[0] == osc::Value::float32 (static_cast<float> (static_cast<double> (leaf["VALUE"][0]))));

    device.withTree ([&] (const Tree& tree)
                     {
                         const auto* node = tree.find (pathOf (leaf));
                         REQUIRE (node != nullptr);
                         CHECK (node->value == *values);
                     });

    const auto container = first (rig.fixture, [] (auto& n) { return isContainer (n) && pathOf (n) != "/"; }, "container");
    CHECK (device.fetchSubtree (pathOf (container)));
    CHECK_FALSE (device.fetchSubtree (pathOf (container) + "/not/there"));
    CHECK_FALSE (device.readValue (pathOf (container) + "/not/there").has_value());

    //  Nothing started, so nothing to stop; the destructor copes.
    CHECK_FALSE (device.isStarted());
}

TEST_CASE ("device: a dead port is Unreachable, with a reason and within the timeout")
{
    Rig rig;
    REQUIRE (rig.started);
    const auto port = rig.port();
    rig.device.stop();

    auto options = optionsFor (port);
    options.httpTimeoutMs = 1000;
    Device device (options);

    CHECK_FALSE (device.probe());
    CHECK (device.state() == Device::State::Unreachable);
    CHECK_FALSE (device.lastError().empty());
    CHECK_FALSE (device.fetchTree());
    CHECK (device.state() == Device::State::Unreachable);
}

//==============================================================================
TEST_CASE ("device: started, it reaches Listening; LISTEN streams values, the current one first")
{
    Rig rig;
    REQUIRE (rig.started);

    Seen seen;
    Device device (optionsFor (rig.port()));
    device.setCallbacks (seen.callbacks());
    device.start();
    CHECK (device.isStarted());

    REQUIRE (reaches (device, Device::State::Listening));
    CHECK (seen.sawState (Device::State::Reachable));
    CHECK (seen.sawState (Device::State::TreeLoaded));
    CHECK (device.treeLoaded());
    REQUIRE (waitUntil ([&] { return rig.device.connectionCount() == 1; }));

    const auto leaf = first (rig.fixture, isFloatLeaf, "f leaf with a value");
    const auto path = pathOf (leaf);

    CHECK (device.listen (path) == Device::Listen::accepted);
    CHECK (device.subscriptions().count (path) == 1);
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (path) == 1; }));

    //  The mock does not push on LISTEN (nor does WFS-DIY): the first value is read over HTTP.
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 1; }));
    CHECK (seen.lastValue().first == path);
    CHECK (seen.lastValue().second == osc::Values { osc::Value::float32 (static_cast<float> (static_cast<double> (leaf["VALUE"][0]))) });

    //  Then the stream.
    REQUIRE (rig.device.setValue (path, { 0.75 }));
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 2; }));
    CHECK (seen.lastValue().first == path);
    CHECK (seen.lastValue().second == osc::Values { osc::Value::float32 (0.75f) });

    //  The tree follows the stream.
    device.withTree ([&] (const Tree& tree)
                     {
                         const auto* node = tree.find (path);
                         REQUIRE (node != nullptr);
                         CHECK (node->value == osc::Values { osc::Value::float32 (0.75f) });
                     });

    CHECK (device.stats().valuesReceived == 2);
    CHECK (device.stats().framesRefused == 0);

    //  IGNORE stops it.
    device.ignore (path);
    CHECK (device.subscriptions().empty());
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (path) == 0; }));

    device.stop();
    CHECK (device.state() == Device::State::Closed);
    CHECK_FALSE (device.isStarted());
    REQUIRE (waitUntil ([&] { return rig.device.connectionCount() == 0; }));
}

TEST_CASE ("device: a subscription made before the connection is sent when it opens")
{
    Rig rig;
    REQUIRE (rig.started);

    Seen seen;
    Device device (optionsFor (rig.port()));
    device.setCallbacks (seen.callbacks());

    const auto leaf = first (rig.fixture, isFloatLeaf, "f leaf with a value");
    const auto path = pathOf (leaf);

    //  Nothing is known about the device yet, so nothing is unsupported yet.
    CHECK (device.listen (path) == Device::Listen::accepted);

    device.start();
    REQUIRE (reaches (device, Device::State::Listening));
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (path) == 1; }));
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 1; }));
    CHECK (seen.lastValue().first == path);

    device.stop();
}

TEST_CASE ("device: a bundle frame is unpacked into one value per message")
{
    Rig rig;
    REQUIRE (rig.started);

    Seen seen;
    Device device (optionsFor (rig.port()));
    device.setCallbacks (seen.callbacks());
    device.start();
    REQUIRE (reaches (device, Device::State::Listening));

    const auto a = nth (rig.fixture, isFloatLeaf, 0, "f leaves with values");
    const auto b = nth (rig.fixture, isFloatLeaf, 1, "f leaves with values");

    device.listen (pathOf (a));
    device.listen (pathOf (b));
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (pathOf (a)) == 1 && rig.device.listenerCount (pathOf (b)) == 1; }));
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 2; }));     // the two current values

    REQUIRE (rig.device.setValueBundle ({ { pathOf (a), { 1.5 } }, { pathOf (b), { -2.5 } } }));
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 4; }));

    {
        //  Released before stop(): onState fires on this thread from stop() and takes the same mutex.
        std::lock_guard<std::mutex> l (seen.mutex);
        const auto& third = seen.values[2];
        const auto& fourth = seen.values[3];
        CHECK (third.first == pathOf (a));
        CHECK (third.second == osc::Values { osc::Value::float32 (1.5f) });
        CHECK (fourth.first == pathOf (b));
        CHECK (fourth.second == osc::Values { osc::Value::float32 (-2.5f) });
    }

    device.stop();
}

//==============================================================================
TEST_CASE ("device: the four notifications change the tree and are reported")
{
    Rig rig;
    REQUIRE (rig.started);

    Seen seen;
    Device device (optionsFor (rig.port()));
    device.setCallbacks (seen.callbacks());
    device.start();
    REQUIRE (reaches (device, Device::State::Listening));

    const auto count = device.tree().nodeCount();

    //  PATH_RENAMED: a leaf moves beside itself; the subscription on it is dropped.
    const auto leaf = nth (rig.fixture, isFloatLeaf, 0, "f leaves");
    const auto oldPath = pathOf (leaf);
    const auto newPath = parentPathOf (oldPath) == "/" ? "/renamed" : parentPathOf (oldPath) + "/renamed";

    device.listen (oldPath);
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (oldPath) == 1; }));

    REQUIRE (rig.device.rename (oldPath, newPath));
    REQUIRE (waitUntil ([&] { return seen.renamedCount() == 1; }));

    {
        std::lock_guard<std::mutex> l (seen.mutex);
        CHECK (seen.renamed[0].first == oldPath);
        CHECK (seen.renamed[0].second == newPath);
    }

    CHECK (device.subscriptions().empty());

    device.withTree ([&] (const Tree& tree)
                     {
                         CHECK (tree.find (oldPath) == nullptr);
                         REQUIRE (tree.find (newPath) != nullptr);
                         CHECK (tree.find (newPath)->fullPath == newPath);
                         CHECK (tree.nodeCount() == count);
                     });

    //  PATH_REMOVED.
    const auto gone = nth (rig.fixture, isFloatLeaf, 1, "f leaves");
    REQUIRE (rig.device.remove (pathOf (gone)));
    REQUIRE (waitUntil ([&] { return seen.removedCount() == 1; }));

    {
        std::lock_guard<std::mutex> l (seen.mutex);
        CHECK (seen.removed[0] == pathOf (gone));
    }

    device.withTree ([&] (const Tree& tree)
                     {
                         CHECK (tree.find (pathOf (gone)) == nullptr);
                         CHECK (tree.nodeCount() == count - 1);
                     });

    //  PATH_CHANGED: the client re-reads that path.
    const auto retyped = first (rig.fixture, [] (auto& n) { return typeOf (n) == "i"; }, "i leaf");
    REQUIRE (rig.device.retype (pathOf (retyped), "f"));
    REQUIRE (waitUntil ([&] { return seen.treeChangedCount() == 1; }));

    {
        std::lock_guard<std::mutex> l (seen.mutex);
        CHECK (seen.treeChanged[0] == pathOf (retyped));
    }

    device.withTree ([&] (const Tree& tree)
                     {
                         const auto* node = tree.find (pathOf (retyped));
                         REQUIRE (node != nullptr);
                         CHECK (node->type == std::optional<std::string> ("f"));
                     });

    //  PATH_ADDED: the client reads the new node.
    const auto container = first (rig.fixture, [] (auto& n) { return isContainer (n) && pathOf (n) != "/"; }, "container");
    const auto addedPath = pathOf (container) + "/added";
    auto* added = new juce::DynamicObject();
    added->setProperty ("TYPE", "i");
    added->setProperty ("ACCESS", 3);
    added->setProperty ("VALUE", juce::Array<juce::var> { 7 });
    REQUIRE (rig.device.add (addedPath, juce::var (added)));
    REQUIRE (waitUntil ([&] { return seen.treeChangedCount() == 2; }));

    device.withTree ([&] (const Tree& tree)
                     {
                         const auto* node = tree.find (addedPath);
                         REQUIRE (node != nullptr);
                         CHECK (node->fullPath == addedPath);
                         CHECK (node->value == osc::Values { osc::Value::int32 (7) });
                         CHECK (tree.nodeCount() == count);         // one gone, one added
                     });

    CHECK (device.stats().notifications == 4);
    CHECK (device.stats().unknownCommands == 0);

    device.stop();
}

TEST_CASE ("device: an unlisted notification is never asked for, but one that arrives is handled")
{
    //  A device that lists LISTEN and PATH_REMOVED but not PATH_RENAMED: a rename goes unreported.
    oqatest::MockDevice::Options quiet;
    quiet.extensions = { "ACCESS", "VALUE", "RANGE", "TYPE", "LISTEN", "PATH_REMOVED" };

    {
        Rig rig (quiet);
        REQUIRE (rig.started);

        Seen seen;
        Device device (optionsFor (rig.port()));
        device.setCallbacks (seen.callbacks());
        device.start();
        REQUIRE (reaches (device, Device::State::Listening));

        const auto a = nth (rig.fixture, isFloatLeaf, 0, "f leaves");
        const auto b = nth (rig.fixture, isFloatLeaf, 1, "f leaves");

        REQUIRE (rig.device.rename (pathOf (a), parentPathOf (pathOf (a)) + "/elsewhere"));
        REQUIRE (rig.device.remove (pathOf (b)));

        //  The remove, sent after the rename, arrives; the rename never does.
        REQUIRE (waitUntil ([&] { return seen.removedCount() == 1; }));
        CHECK (seen.renamedCount() == 0);
        CHECK (device.tree().find (pathOf (a)) != nullptr);        // still there, as far as the client knows

        device.stop();
    }

    //  WFS-DIY's way: PATH_CHANGED sent without being listed. Handled all the same.
    oqatest::MockDevice::Options wfs;
    wfs.extensions = { "ACCESS", "VALUE", "RANGE", "DESCRIPTION", "TYPE", "CLIPMODE", "LISTEN" };
    wfs.notifyUnlisted = true;

    {
        Rig rig (wfs);
        REQUIRE (rig.started);

        Seen seen;
        Device device (optionsFor (rig.port()));
        device.setCallbacks (seen.callbacks());
        device.start();
        REQUIRE (reaches (device, Device::State::Listening));
        CHECK_FALSE (device.hostInfo()->has ("PATH_CHANGED"));

        const auto retyped = first (rig.fixture, [] (auto& n) { return typeOf (n) == "i"; }, "i leaf");
        REQUIRE (rig.device.retype (pathOf (retyped), "f"));
        REQUIRE (waitUntil ([&] { return seen.treeChangedCount() == 1; }));
        CHECK (device.tree().find (pathOf (retyped))->type == std::optional<std::string> ("f"));

        device.stop();
    }
}

TEST_CASE ("device: a device without websocket is TreeLoaded, and listen is unsupported")
{
    oqatest::MockDevice::Options options;
    options.websocket = false;
    options.extensions = { "ACCESS", "VALUE", "RANGE", "TYPE" };

    Rig rig (options);
    REQUIRE (rig.started);

    Seen seen;
    Device device (optionsFor (rig.port()));
    device.setCallbacks (seen.callbacks());
    device.start();

    REQUIRE (reaches (device, Device::State::TreeLoaded));
    CHECK_FALSE (device.hostInfo()->offersWebsocket());

    const auto leaf = first (rig.fixture, isFloatLeaf, "f leaf");
    CHECK (device.listen (pathOf (leaf)) == Device::Listen::unsupported);
    CHECK (device.subscriptions().empty());

    std::string error;
    CHECK_FALSE (device.sendOsc (osc::Packet::message (pathOf (leaf), { osc::Value::float32 (1.0f) }), error));
    CHECK_FALSE (error.empty());

    juce::Thread::sleep (300);
    CHECK (device.state() == Device::State::TreeLoaded);
    CHECK (rig.device.connectionCount() == 0);
    CHECK_FALSE (seen.sawState (Device::State::Listening));

    //  The tree is still read, over HTTP alone.
    CHECK (device.readValue (pathOf (leaf)).has_value());

    device.stop();
}

//==============================================================================
TEST_CASE ("device: a dropped websocket comes back with its subscriptions, and values resume")
{
    Rig rig;
    REQUIRE (rig.started);

    Seen seen;
    Device device (optionsFor (rig.port()));
    device.setCallbacks (seen.callbacks());
    device.start();
    REQUIRE (reaches (device, Device::State::Listening));

    const auto leaf = first (rig.fixture, isFloatLeaf, "f leaf");
    const auto path = pathOf (leaf);
    device.listen (path);
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (path) == 1 && seen.valueCount() >= 1; }));

    rig.device.closeAllConnections();

    REQUIRE (waitUntil ([&] { return seen.sawState (Device::State::Reconnecting); }));
    REQUIRE (reaches (device, Device::State::Listening));
    REQUIRE (waitUntil ([&] { return rig.device.listenerCount (path) == 1; }));
    CHECK (device.stats().reconnects == 1);
    CHECK (device.subscriptions().count (path) == 1);

    //  The current value again after the reopen, then the stream.
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 2; }));
    REQUIRE (rig.device.setValue (path, { 0.125 }));
    REQUIRE (waitUntil ([&] { return seen.lastValue().second == osc::Values { osc::Value::float32 (0.125f) }; }));

    device.stop();
}

TEST_CASE ("device: a device that is not there yet is found when it appears")
{
    //  A port nobody holds right now, then a mock on exactly that port.
    int port = 0;

    {
        juce::StreamingSocket probe;
        REQUIRE (probe.createListener (0, loopback));
        port = probe.getBoundPort();
        probe.close();
    }

    REQUIRE (port > 0);

    Seen seen;
    auto options = optionsFor (port);
    options.httpTimeoutMs = 1000;
    Device device (options);
    device.setCallbacks (seen.callbacks());

    const auto fixture = loadFixture();
    const auto leaf = first (fixture, isFloatLeaf, "f leaf");
    device.listen (pathOf (leaf));

    device.start();
    REQUIRE (reaches (device, Device::State::Unreachable));
    CHECK_FALSE (device.lastError().empty());

    oqatest::MockDevice::Options mockOptions;
    mockOptions.httpPort = port;
    oqatest::MockDevice mock (mockOptions);
    REQUIRE (mock.loadTree (fixture));
    REQUIRE (mock.start());

    REQUIRE (reaches (device, Device::State::Listening));
    REQUIRE (waitUntil ([&] { return mock.listenerCount (pathOf (leaf)) == 1; }));
    CHECK (device.treeLoaded());

    device.stop();
}

TEST_CASE ("device: OSC goes out over the websocket as a binary frame")
{
    Rig rig;
    REQUIRE (rig.started);

    Device device (optionsFor (rig.port()));

    const auto leaf = first (rig.fixture, isFloatLeaf, "f leaf");
    const auto packet = osc::Packet::message (pathOf (leaf), { osc::Value::float32 (0.5f) });

    std::string error;
    CHECK_FALSE (device.sendOsc (packet, error));          // not open yet
    CHECK_FALSE (error.empty());

    device.start();
    REQUIRE (reaches (device, Device::State::Listening));

    error.clear();
    REQUIRE (device.sendOsc (packet, error));
    REQUIRE (waitUntil ([&] { return rig.device.receivedWebsocketOsc().size() == 1; }));

    const auto frames = rig.device.receivedWebsocketOsc();
    const auto decoded = osc::decode (frames[0].data(), frames[0].size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet == packet);

    //  What the wire cannot carry is refused before it goes anywhere.
    CHECK_FALSE (device.sendOsc (osc::Packet::message ("not an address"), error));
    CHECK_FALSE (error.empty());

    device.stop();
}

//==============================================================================
//  Julien Bayle's retest of ec6725c, 2.7: a device that was gone long enough to
//  be Unreachable came back with the tree it had before, whatever it served now.
TEST_CASE ("device: back from Unreachable, the tree is read again, so an ACCESS the device changed meanwhile is seen")
{
    /*  A port of the test's choosing, so the device restarts where it was. */
    int port = 0;

    {
        oqatest::MockDevice probe;
        REQUIRE (probe.loadTree (loadFixture()));
        REQUIRE (probe.start());
        port = probe.httpPort();
        probe.stop();
    }

    oqatest::MockDevice::Options options;
    options.httpPort = port;
    oqatest::MockDevice mock (options);
    const auto fixture = loadFixture();
    REQUIRE (mock.loadTree (fixture));
    REQUIRE (mock.start());
    REQUIRE (mock.httpPort() == port);

    Seen seen;
    Device device (optionsFor (port));
    device.setCallbacks (seen.callbacks());
    device.start();
    REQUIRE (reaches (device, Device::State::Listening));

    const auto path = pathOf (first (fixture, isFloatLeaf, "f leaf"));

    const auto accessNow = [&]
    {
        int access = -1;
        device.withTree ([&] (const Tree& tree)
        {
            if (const auto* node = tree.find (path))
                access = node->effectiveAccess();
        });
        return access;
    };

    REQUIRE ((accessNow() & 2) != 0);

    /*  The device goes away, and while it is away decides nobody may write. */
    mock.setAllowList (juce::StringArray { "192.0.2.99" });
    mock.stop();
    REQUIRE (waitUntil ([&] { return seen.sawState (Device::State::Unreachable); }));

    bool back = false;

    for (int tries = 0; tries < 50 && ! back; ++tries)
    {
        back = mock.start() && mock.httpPort() == port;

        if (! back)
        {
            mock.stop();
            juce::Thread::sleep (100);
        }
    }

    REQUIRE (back);
    REQUIRE (reaches (device, Device::State::Listening));

    /*  What it serves now, not what it served before. */
    REQUIRE (waitUntil ([&] { return (accessNow() & 2) == 0; }));
    CHECK ((accessNow() & 1) != 0);

    device.stop();
}
