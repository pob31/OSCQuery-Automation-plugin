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
    THE GENERIC OSCQUERY SERVER (devplan Phase 6b-1), judged by the library's
    own client and a raw GET, against the generic fixture tree, by the profile
    draft's own words: C1, C4, C6, C7, L1, L2, L3, L4. The matrix's independent
    judge is Phase 8's CLI; this is the server passing its own checks before it
    carries the plugin's slot table.
*/

#include <doctest/doctest.h>

#include "TestSupport.h"
#include "support/HttpGet.h"
#include "support/WsTestClient.h"

#include <oscquery/Device.h>
#include <oscquery/net/UdpEndpoint.h>
#include <oscquery/osc/Codec.h>
#include <oscquery/server/AccessPolicy.h>
#include <oscquery/server/OscQueryServer.h>
#include <oscquery/tree/Tree.h>

#include <mutex>
#include <string>
#include <vector>

using namespace oqa::oscquery;
using oqa::oscquery::server::AccessPolicy;
using oqa::oscquery::server::OscQueryServer;
using oqatest::waitUntil;

namespace
{
    const std::vector<std::string> everyExtension { "ACCESS", "VALUE", "RANGE", "TYPE", "DESCRIPTION", "LISTEN",
                                                    "PATH_CHANGED", "PATH_ADDED", "PATH_REMOVED", "PATH_RENAMED" };

    /** A server on the generic fixture, every write and refusal recorded. */
    struct Served
    {
        Tree tree;
        OscQueryServer server;

        std::mutex mutex;
        std::vector<std::tuple<std::string, std::string, osc::Values>> writes;
        std::vector<std::tuple<std::string, std::string, std::string>> refusals;

        explicit Served (std::string allow = "any", std::vector<std::string> extensions = everyExtension)
        {
            std::string error;
            auto parsed = Tree::parse (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json")), error);
            REQUIRE_MESSAGE (parsed.has_value(), error);
            tree = *parsed;
            server.setTree (tree);

            OscQueryServer::Options options;
            options.name = "Served T";
            options.extensions = std::move (extensions);
            options.allow = std::move (allow);

            OscQueryServer::Callbacks callbacks;
            callbacks.onWrite = [this] (const std::string& c, const std::string& p, const osc::Values& v) { std::lock_guard<std::mutex> l (mutex); writes.emplace_back (c, p, v); };
            callbacks.onRefused = [this] (const std::string& c, const std::string& p, const std::string& w) { std::lock_guard<std::mutex> l (mutex); refusals.emplace_back (c, p, w); };

            REQUIRE_MESSAGE (server.start (std::move (options), std::move (callbacks), error), error);
        }

        const Node& leafWhere (const std::function<bool (const Node&)>& predicate) const
        {
            for (const auto* leaf : tree.leaves())
                if (predicate (*leaf))
                    return *leaf;

            FAIL ("no such leaf in the fixture");
            return *tree.leaves().front();
        }

        std::size_t writeCount()   { std::lock_guard<std::mutex> l (mutex); return writes.size(); }
        std::size_t refusalCount() { std::lock_guard<std::mutex> l (mutex); return refusals.size(); }
        std::string lastWhy()      { std::lock_guard<std::mutex> l (mutex); return refusals.empty() ? std::string() : std::get<2> (refusals.back()); }
    };

    bool isRangedFloat (const Node& n)
    {
        return n.type.has_value() && *n.type == "f" && n.argumentCount() == 1
            && n.argument (0).range.has_value() && n.argument (0).range->hasMinMax();
    }

    struct Seen
    {
        std::mutex mutex;
        std::vector<std::pair<std::string, osc::Values>> values;
        std::vector<std::string> removed, changed;
        std::vector<std::pair<std::string, std::string>> renamed;

        Device::Callbacks callbacks()
        {
            Device::Callbacks c;
            c.onValue = [this] (const std::string& p, const osc::Values& v) { std::lock_guard<std::mutex> l (mutex); values.emplace_back (p, v); };
            c.onPathRemoved = [this] (const std::string& p) { std::lock_guard<std::mutex> l (mutex); removed.push_back (p); };
            c.onPathRenamed = [this] (const std::string& a, const std::string& b) { std::lock_guard<std::mutex> l (mutex); renamed.emplace_back (a, b); };
            c.onTreeChanged = [this] (const std::string& p) { std::lock_guard<std::mutex> l (mutex); changed.push_back (p); };
            return c;
        }

        std::size_t valueCount() { std::lock_guard<std::mutex> l (mutex); return values.size(); }
        std::pair<std::string, osc::Values> last() { std::lock_guard<std::mutex> l (mutex); return values.empty() ? std::pair<std::string, osc::Values> {} : values.back(); }
    };

    Device::Options clientFor (int port)
    {
        Device::Options options;
        options.host = "127.0.0.1";
        options.port = port;
        options.reconnectInitialMs = 100;
        options.reconnectMaxMs = 1000;
        return options;
    }

    std::vector<std::uint8_t> message (const std::string& address, osc::Values args)
    {
        osc::Packet p;
        p.kind = osc::Packet::Kind::message;
        p.address = address;
        p.args = std::move (args);
        std::string error;
        auto encoded = osc::encode (p, error);
        REQUIRE_MESSAGE (encoded.has_value(), error);
        return *encoded;
    }
}

//==============================================================================
TEST_CASE ("server C1: GET /?HOST_INFO names the server, its OSC port and transport, its websocket and what it extends")
{
    Served s;
    const auto reply = oqatest::get (s.server.httpPort(), "/?HOST_INFO");
    REQUIRE (reply.status == 200);

    const auto info = juce::JSON::parse (juce::String (reply.body));
    CHECK (info.getProperty ("NAME", "").toString() == "Served T");
    CHECK (static_cast<int> (info.getProperty ("OSC_PORT", 0)) == s.server.oscPort());
    CHECK (s.server.oscPort() > 0);
    CHECK (info.getProperty ("OSC_TRANSPORT", "").toString() == "UDP");
    CHECK (static_cast<int> (info.getProperty ("WS_PORT", 0)) == s.server.httpPort());
    CHECK (info.getProperty ("EXTENSIONS", {}).getProperty ("LISTEN", false) == juce::var (true));
    CHECK (info.getProperty ("EXTENSIONS", {}).getProperty ("PATH_RENAMED", false) == juce::var (true));

    /*  The path in the URL is functionally irrelevant, as the proposal says. */
    CHECK (oqatest::get (s.server.httpPort(), "/anything/at/all?HOST_INFO").status == 200);
}

TEST_CASE ("server C7 and C4: the whole tree comes back as served, a subtree is just that node, an unknown path is 404, attributes answer 200, 204 and 400")
{
    Served s;

    const auto whole = oqatest::get (s.server.httpPort(), "/");
    REQUIRE (whole.status == 200);
    std::string error;
    const auto back = Tree::parse (juce::JSON::parse (juce::String (whole.body)), error);
    REQUIRE_MESSAGE (back.has_value(), error);
    CHECK (back->treeHash() == s.tree.treeHash());

    /*  C4 is the tree's to pass, not the server's to lose: every leaf comes
        back with its TYPE, and with a RANGE exactly where the fixture has one
        (the generic fixture keeps one rangeless leaf on purpose, for the
        "range unknown" row). The plugin's own tree meets C4 in 6b-2. */
    for (const auto* leaf : back->leaves())
    {
        INFO ("leaf " << leaf->fullPath);
        const auto* original = s.tree.find (leaf->fullPath);
        REQUIRE (original != nullptr);
        CHECK (leaf->type == original->type);
        CHECK (leaf->effectiveAccess() == original->effectiveAccess());

        for (std::size_t i = 0; i < leaf->argumentCount(); ++i)
            CHECK (leaf->argument (i).range.has_value() == original->argument (i).range.has_value());
    }

    const auto& gain = s.leafWhere (isRangedFloat);
    const auto sub = oqatest::get (s.server.httpPort(), gain.fullPath);
    REQUIRE (sub.status == 200);
    const auto node = juce::JSON::parse (juce::String (sub.body));
    CHECK (node.getProperty ("FULL_PATH", "").toString().toStdString() == gain.fullPath);
    CHECK_FALSE (node.hasProperty ("CONTENTS"));

    const auto container = gain.fullPath.substr (0, gain.fullPath.rfind ('/'));
    const auto subtree = oqatest::get (s.server.httpPort(), container.empty() ? "/" : container);
    REQUIRE (subtree.status == 200);
    CHECK (juce::JSON::parse (juce::String (subtree.body)).hasProperty ("CONTENTS"));

    CHECK (oqatest::get (s.server.httpPort(), "/nowhere/at/all").status == 404);

    const auto type = oqatest::get (s.server.httpPort(), gain.fullPath + "?TYPE");
    REQUIRE (type.status == 200);
    CHECK (juce::JSON::parse (juce::String (type.body)).getProperty ("TYPE", "").toString() == "f");

    CHECK (oqatest::get (s.server.httpPort(), gain.fullPath + "?FOO").status == 400);
    CHECK (oqatest::get (s.server.httpPort(), gain.fullPath + "?HTML").status == 204);
    CHECK (oqatest::get (s.server.httpPort(), "/?CONTENTS").status == 200);
}

TEST_CASE ("server C6: a client the allow-list refuses sees every leaf read-only; one it admits sees ACCESS as the tree says")
{
    Served refused ("192.0.2.1");
    const auto& gain = refused.leafWhere (isRangedFloat);

    auto reply = oqatest::get (refused.server.httpPort(), gain.fullPath);
    REQUIRE (reply.status == 200);
    CHECK ((static_cast<int> (juce::JSON::parse (juce::String (reply.body)).getProperty ("ACCESS", 3)) & 2) == 0);

    reply = oqatest::get (refused.server.httpPort(), "/");
    std::string error;
    const auto clamped = Tree::parse (juce::JSON::parse (juce::String (reply.body)), error);
    REQUIRE (clamped.has_value());

    for (const auto* leaf : clamped->leaves())
        CHECK_FALSE (leaf->bindableForSend());

    /*  The allow-list may open while serving. */
    refused.server.setAllow ("any");
    reply = oqatest::get (refused.server.httpPort(), gain.fullPath);
    CHECK ((static_cast<int> (juce::JSON::parse (juce::String (reply.body)).getProperty ("ACCESS", 0)) & 2) != 0);

    /*  The policy itself: loopback when nothing is said; a list; any. */
    CHECK (AccessPolicy::allows ("", "127.0.0.1"));
    CHECK (AccessPolicy::allows ("", "::ffff:127.0.0.1"));
    CHECK (AccessPolicy::allows ("", "::1"));
    CHECK_FALSE (AccessPolicy::allows ("", "192.0.2.7"));
    CHECK (AccessPolicy::allows ("192.0.2.7, 192.0.2.8", "::ffff:192.0.2.8"));
    CHECK_FALSE (AccessPolicy::allows ("192.0.2.7", "127.0.0.1"));
    CHECK (AccessPolicy::allows ("192.0.2.7, loopback", "127.0.0.1"));
    CHECK (AccessPolicy::allows ("any", "203.0.113.9"));
}

TEST_CASE ("server L1: LISTEN streams a leaf's changes as binary OSC to the connection that asked, the current value first; IGNORE stops it")
{
    Served s;
    const auto& gain = s.leafWhere (isRangedFloat);

    Seen seen;
    Device client (clientFor (s.server.httpPort()));
    client.setCallbacks (seen.callbacks());
    client.start();
    REQUIRE (waitUntil ([&] { return client.state() == Device::State::Listening; }));
    REQUIRE (waitUntil ([&] { return s.server.connectionCount() == 1; }));

    CHECK (client.listen (gain.fullPath) == Device::Listen::accepted);
    REQUIRE (waitUntil ([&] { return s.server.listenerCount (gain.fullPath) == 1; }));
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 1; }));     // the current value, read on LISTEN

    REQUIRE (s.server.setValue (gain.fullPath, { osc::Value::float32 (0.75f) }));
    REQUIRE (waitUntil ([&] { return seen.valueCount() >= 2; }));
    const auto last = seen.last();
    CHECK (last.first == gain.fullPath);
    REQUIRE (last.second.size() == 1);
    CHECK (last.second[0].getFloat32() == doctest::Approx (0.75f));

    /*  The served VALUE follows too. */
    const auto reply = oqatest::get (s.server.httpPort(), gain.fullPath + "?VALUE");
    REQUIRE (reply.status == 200);
    CHECK (static_cast<double> (juce::JSON::parse (juce::String (reply.body)).getProperty ("VALUE", {})[0]) == doctest::Approx (0.75));

    CHECK_FALSE (s.server.setValue ("/no/such/leaf", { osc::Value::float32 (1.0f) }));

    client.ignore (gain.fullPath);
    REQUIRE (waitUntil ([&] { return s.server.listenerCount (gain.fullPath) == 0; }));
}

TEST_CASE ("server L2: a path removed, renamed or added is told to every connection, and the client's tree follows")
{
    Served s;
    const auto& gain = s.leafWhere (isRangedFloat);
    const auto& other = s.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.fullPath != gain.fullPath; });

    Seen seen;
    Device client (clientFor (s.server.httpPort()));
    client.setCallbacks (seen.callbacks());
    client.start();
    REQUIRE (waitUntil ([&] { return client.state() == Device::State::Listening; }));

    REQUIRE (s.server.removePath (gain.fullPath));
    REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (seen.mutex); return ! seen.removed.empty(); }));
    { std::lock_guard<std::mutex> l (seen.mutex); CHECK (seen.removed.back() == gain.fullPath); }
    CHECK (oqatest::get (s.server.httpPort(), gain.fullPath).status == 404);

    const auto moved = other.fullPath + "_moved";
    REQUIRE (s.server.renamePath (other.fullPath, moved));
    REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (seen.mutex); return ! seen.renamed.empty(); }));
    { std::lock_guard<std::mutex> l (seen.mutex); CHECK (seen.renamed.back() == std::make_pair (other.fullPath, moved)); }
    CHECK (oqatest::get (s.server.httpPort(), moved).status == 200);

    /*  Added back under its old name: PATH_ADDED, and the client re-reads it. */
    Node fresh = gain;
    REQUIRE (s.server.replaceSubtree (fresh));
    REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (seen.mutex); return ! seen.changed.empty(); }));
    CHECK (oqatest::get (s.server.httpPort(), gain.fullPath).status == 200);

    CHECK_FALSE (s.server.removePath ("/no/such/path"));
    CHECK_FALSE (s.server.renamePath ("/no/such/path", "/elsewhere"));
}

TEST_CASE ("server L3 and L4: OSC over UDP and over a websocket frame is a write from an admitted client to a writable leaf, and a refusal otherwise, counted with its reason")
{
    const auto writable = [] (const Node& n) { return isRangedFloat (n) && n.bindableForSend(); };
    const auto readOnly = [] (const Node& n) { return n.isLeaf() && ! n.bindableForSend(); };

    SUBCASE ("admitted, over UDP and over the websocket")
    {
        Served s ("any");
        const auto& gain = s.leafWhere (writable);

        net::UdpEndpoint sender;
        REQUIRE (sender.send ("127.0.0.1", s.server.oscPort(), message (gain.fullPath, { osc::Value::float32 (0.5f) })));
        REQUIRE (waitUntil ([&] { return s.writeCount() >= 1; }));

        {
            std::lock_guard<std::mutex> l (s.mutex);
            CHECK (std::get<0> (s.writes[0]) == "127.0.0.1");
            CHECK (std::get<1> (s.writes[0]) == gain.fullPath);
            CHECK (std::get<2> (s.writes[0])[0].getFloat32() == doctest::Approx (0.5f));
        }

        Device client (clientFor (s.server.httpPort()));
        client.start();
        REQUIRE (waitUntil ([&] { return client.state() == Device::State::Listening; }));

        osc::Packet p;
        p.address = gain.fullPath;
        p.args = { osc::Value::float32 (0.25f) };
        std::string error;
        REQUIRE_MESSAGE (client.sendOsc (p, error), error);
        REQUIRE (waitUntil ([&] { return s.writeCount() >= 2; }));
        CHECK (s.server.acceptedWrites() == 2);
        CHECK (s.server.refusedWrites() == 0);

        /*  A bundle is its messages. */
        osc::Packet bundle;
        bundle.kind = osc::Packet::Kind::bundle;
        bundle.elements = { p, p };
        std::string bundleError;
        const auto bytes = osc::encode (bundle, bundleError);
        REQUIRE (bytes.has_value());
        REQUIRE (sender.send ("127.0.0.1", s.server.oscPort(), *bytes));
        REQUIRE (waitUntil ([&] { return s.writeCount() >= 4; }));
    }

    SUBCASE ("refused: not on the allow-list, read-only, no such path, not OSC")
    {
        Served s ("192.0.2.1");
        const auto& gain = s.leafWhere (writable);
        const auto& ro = s.leafWhere (readOnly);

        net::UdpEndpoint sender;
        REQUIRE (sender.send ("127.0.0.1", s.server.oscPort(), message (gain.fullPath, { osc::Value::float32 (0.5f) })));
        REQUIRE (waitUntil ([&] { return s.refusalCount() >= 1; }));
        CHECK (s.lastWhy() == "not on the allow-list");

        s.server.setAllow ("any");
        REQUIRE (sender.send ("127.0.0.1", s.server.oscPort(), message (ro.fullPath, { osc::Value::float32 (0.5f) })));
        REQUIRE (waitUntil ([&] { return s.refusalCount() >= 2; }));
        CHECK (s.lastWhy() == "read-only");

        REQUIRE (sender.send ("127.0.0.1", s.server.oscPort(), message ("/no/such/path", { osc::Value::int32 (1) })));
        REQUIRE (waitUntil ([&] { return s.refusalCount() >= 3; }));
        CHECK (s.lastWhy() == "no such path");

        const std::vector<std::uint8_t> garbage { 'n', 'o', 't', ' ', 'o', 's', 'c' };
        REQUIRE (sender.send ("127.0.0.1", s.server.oscPort(), garbage));
        REQUIRE (waitUntil ([&] { return s.refusalCount() >= 4; }));
        CHECK (juce::String (s.lastWhy()).startsWith ("not OSC"));

        CHECK (s.server.refusedWrites() == 4);
        CHECK (s.server.acceptedWrites() == 0);
        CHECK (s.writeCount() == 0);
    }
}

TEST_CASE ("server: an HTTP port already in use is a start that fails with the reason; an ephemeral pair beside it is distinct")
{
    /*  A UDP port taken twice is not refused on every platform (JUCE binds its
        datagram sockets with address reuse, and Windows then lets two bind one
        port), so a pinned OSC port in use is not a failure the server can
        promise to report; the ephemeral default is the safe one. */
    Served first;

    OscQueryServer second;
    OscQueryServer::Options options;
    options.name = "Second";
    options.httpPort = first.server.httpPort();
    std::string error;
    CHECK_FALSE (second.start (options, {}, error));
    CHECK (juce::String (error).contains ("HTTP port"));
    CHECK_FALSE (second.isRunning());

    OscQueryServer fourth;
    options.httpPort = 0;
    error.clear();
    REQUIRE_MESSAGE (fourth.start (options, {}, error), error);
    CHECK (fourth.httpPort() != first.server.httpPort());
    CHECK (fourth.oscPort() != first.server.oscPort());
    CHECK (fourth.isRunning());
}

//==============================================================================
//  The two optional extensions (docs/spec/extensions-draft-0.1.md; devplan 6x-1).
TEST_CASE ("server extensions: DEVICE_ID and CLOCK are served and declared together, and neither appears unasked")
{
    //  Unasked: HOST_INFO is what it was.
    {
        Served s;
        const auto info = juce::JSON::parse (juce::String (oqatest::get (s.server.httpPort(), "/?HOST_INFO").body));
        CHECK_FALSE (info.hasProperty ("DEVICE_ID"));
        CHECK_FALSE (info.hasProperty ("CLOCK"));
        CHECK_FALSE (info.getProperty ("EXTENSIONS", {}).hasProperty ("DEVICE_ID"));
        CHECK_FALSE (info.getProperty ("EXTENSIONS", {}).hasProperty ("CLOCK"));
    }

    OscQueryServer server;
    OscQueryServer::Options options;
    options.name = "Served X";
    options.extensions = { "ACCESS", "VALUE", "TYPE" };           // the owner lists neither: the server does
    options.allow = "any";
    options.deviceId = "6f1c2a9e-0b4d-4e2a-9f7c-3d5b8a1e2c47";
    options.clock = oqa::oscquery::ClockInfo { "local", std::nullopt, false };

    std::string error;
    REQUIRE_MESSAGE (server.start (options, {}, error), error);

    const auto reply = oqatest::get (server.httpPort(), "/?HOST_INFO");
    REQUIRE (reply.status == 200);

    //  Read back by the library's own parser, as a client would.
    const auto info = oqa::oscquery::parseHostInfo (reply.body, error);
    REQUIRE (info.has_value());
    CHECK (info->has ("DEVICE_ID"));
    CHECK (info->has ("CLOCK"));
    CHECK (info->deviceId == options.deviceId);
    REQUIRE (info->clock.has_value());
    CHECK (*info->clock == *options.clock);
    CHECK (info->meetsC1());
    CHECK (info->has ("ACCESS"));

    server.stop();
}
