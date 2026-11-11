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
    BINDING OVER THE PLUGIN'S OWN SERVER (devplan Phase 6b-3; PRD, Headless
    use: "Go.dot or a script can bind slots without opening the editor"): a
    write to a slot's path over UDP or over the websocket becomes a command the
    instance applies on its main thread (here, the test's), binding the slot;
    the other writable leaves set what they name; a device nobody knows, a
    slot that is unbound, a bad word are refused with the reason in the
    instance's warnings; a client the allow-list refuses never reaches the
    instance.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"
#include "support/HttpGet.h"
#include "support/MdnsResponder.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>
#include <core/server/ServerNamespace.h>

#include <oscquery/Device.h>
#include <oscquery/net/UdpEndpoint.h>
#include <oscquery/osc/Codec.h>

#include <atomic>
#include <string>

using namespace oqa::core;
using namespace oqa::oscquery;
namespace ns = oqa::core::server::ns;
using oqatest::Rig;
using oqatest::waitUntil;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    const bool shutdownRegistered = (oqatest::registerShutdownHook ([] { Registry::shared().shutdown(); }), true);

    struct ServerOn
    {
        oqatest::MdnsResponder responder { {} };
        RegistrySettings before;

        ServerOn()
        {
            Finder::Options finder;
            finder.browse = true;
            finder.discovery.port = 0;
            finder.discovery.peers = { { "127.0.0.1", responder.port() } };
            finder.httpTimeoutMs = 1000;
            finder.reprobeMs = 1000;
            finder.tickMs = 50;
            Registry::shared().setFinderOptions (finder);

            before = Registry::shared().settings();
            auto s = before;
            s.serverEnabled = true;
            s.serverAllow = "any";
            Registry::shared().setSettings (s);
            REQUIRE (waitUntil ([] { return port() > 0; }));
        }

        ~ServerOn() { Registry::shared().setSettings (before); }

        static juce::var serverStatus() { return Registry::shared().status().getProperty ("server", {}); }
        static int port()               { return static_cast<int> (serverStatus().getProperty ("port", 0)); }
        static int oscPort()            { return static_cast<int> (serverStatus().getProperty ("osc_port", 0)); }
    };

    juce::int64 registryIdOf (const Instance& instance, const std::string& label)
    {
        const auto whole = juce::JSON::parse (juce::String (instance.processStatusJson()));

        if (const auto* instances = whole.getProperty ("instances", {}).getArray())
            for (const auto& i : *instances)
                if (i.getProperty ("label", "").toString().toStdString() == label)
                    return static_cast<juce::int64> (i.getProperty ("id", -1));

        return -1;
    }

    std::vector<std::uint8_t> message (const std::string& address, osc::Values args)
    {
        osc::Packet p;
        p.address = address;
        p.args = std::move (args);
        std::string error;
        auto encoded = osc::encode (p, error);
        REQUIRE_MESSAGE (encoded.has_value(), error);
        return *encoded;
    }

    /** An instance with a command wake counted, so a test knows when to poll. */
    struct Commanded
    {
        Instance instance;
        std::atomic<int> wakes { 0 };
        juce::int64 id = -1;
        net::UdpEndpoint sender;

        Commanded (const char* label, Rig& rig, const char* deviceName) : instance (api(), 8, label)
        {
            instance.addDevice (rig.device (deviceName));
            instance.setCommandWake ([this] { ++wakes; });
            instance.connect();
            id = registryIdOf (instance, label);
            REQUIRE (id >= 0);
            REQUIRE (waitUntil ([&] { return oqatest::get (ServerOn::port(), slot (0) + "/path").status == 200; }));
        }

        std::string slot (int n) const { return ns::slotPath (static_cast<unsigned long> (id), n); }
        std::string plugin() const     { return ns::pluginPath (static_cast<unsigned long> (id)); }

        /*  Write over UDP, wait for the wake, apply: how many commands were applied.

            The leaf is waited for first. A slot's leaves other than its path
            are served only once it is bound, and the server learns of a bind
            on its own thread: a write sent the instant after the bind may
            reach the server before the leaf does, be refused as "no such
            path" and wake nobody (seen once on a macOS runner, 2026-11-11). */
        int write (const std::string& leaf, osc::Values values)
        {
            REQUIRE (waitUntil ([&] { return oqatest::get (ServerOn::port(), leaf).status == 200; }));

            const auto before = wakes.load();
            REQUIRE (sender.send ("127.0.0.1", ServerOn::oscPort(), message (leaf, std::move (values))));
            REQUIRE (waitUntil ([&] { return wakes.load() > before; }));
            return instance.pollCommands();
        }

        std::string lastWarning() const
        {
            return instance.warnings().empty() ? std::string() : instance.warnings().back();
        }
    };
}

//==============================================================================
TEST_CASE ("commands: a path written to a slot over UDP binds it, with the device named or the chunk's; every other writable leaf sets what it names; \"\" unbinds")
{
    ServerOn on;
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && ! n.unit.empty() && ! n.unit[0].empty(); });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });
    const Node* xyz = nullptr;

    for (const auto* leaf : rig.tree.leaves())
        if (leaf->argumentCount() == 3)
            xyz = leaf;

    REQUIRE (xyz != nullptr);

    Commanded c ("commanded", rig, "commanded");
    rig.waitReady (c.instance);

    /*  The chunk's one device, unnamed. */
    CHECK (c.write (c.slot (3) + "/path", { osc::Value::string (gain.fullPath) }) == 1);
    REQUIRE (c.instance.chunk().slots[3].bound());
    CHECK (c.instance.chunk().slots[3].path == gain.fullPath);
    CHECK (c.instance.chunk().slots[3].deviceIndex == 0);
    CHECK (c.instance.warnings().empty());

    /*  Named, as a second string. */
    CHECK (c.write (c.slot (4) + "/path", { osc::Value::string (pan.fullPath), osc::Value::string ("commanded") }) == 1);
    CHECK (c.instance.chunk().slots[4].path == pan.fullPath);

    /*  A device written first, then the path. */
    CHECK (c.write (c.slot (5) + "/device", { osc::Value::string ("commanded") }) == 1);
    CHECK_FALSE (c.instance.chunk().slots[5].bound());
    CHECK (c.write (c.slot (5) + "/path", { osc::Value::string (gain.fullPath) }) == 1);
    CHECK (c.instance.chunk().slots[5].bound());

    /*  A node of three arguments takes consecutive slots. */
    CHECK (c.write (c.slot (0) + "/path", { osc::Value::string (xyz->fullPath) }) == 1);
    CHECK (c.instance.chunk().slots[0].path == xyz->fullPath);
    CHECK (c.instance.chunk().slots[1].path == xyz->fullPath);
    CHECK (c.instance.chunk().slots[2].argIndex == 2);

    /*  The other leaves. */
    REQUIRE (waitUntil ([&] { return oqatest::get (ServerOn::port(), c.slot (3) + "/display_name").status == 200; }));
    CHECK (c.write (c.slot (3) + "/display_name", { osc::Value::string ("Gain from afar") }) == 1);
    CHECK (c.instance.chunk().slots[3].displayName == "Gain from afar");
    CHECK (c.write (c.slot (3) + "/scaling", { osc::Value::string ("exp") }) == 1);
    CHECK (c.instance.chunk().slots[3].scaling == Scaling::exponential);
    CHECK (c.write (c.slot (3) + "/direction", { osc::Value::string ("both") }) == 1);
    CHECK (c.instance.chunk().slots[3].direction == Direction::both);
    CHECK (c.write (c.slot (3) + "/range/min", { osc::Value::float32 (-3.0f) }) == 1);
    CHECK (c.write (c.slot (3) + "/range/max", { osc::Value::float32 (3.0f) }) == 1);
    CHECK (*c.instance.chunk().slots[3].rangeMin == doctest::Approx (-3.0));
    CHECK (*c.instance.chunk().slots[3].rangeMax == doctest::Approx (3.0));
    CHECK (c.instance.chunk().slots[3].rangeOverridden);

    /*  The instance's number. */
    CHECK (c.write (c.plugin() + "/instance", { osc::Value::int32 (7) }) == 1);
    CHECK (c.instance.instanceNumber() == 7);

    /*  The served tree follows: the slot's state is ok, the path is there. */
    REQUIRE (waitUntil ([&]
    {
        const auto reply = oqatest::get (ServerOn::port(), c.slot (3) + "/state");
        return reply.status == 200 && juce::JSON::parse (juce::String (reply.body)).getProperty ("VALUE", {})[0].toString() == "ok";
    }));

    /*  "" unbinds. */
    CHECK (c.write (c.slot (4) + "/path", { osc::Value::string ("") }) == 1);
    CHECK_FALSE (c.instance.chunk().slots[4].bound());
    CHECK (c.instance.warnings().empty());
}

TEST_CASE ("commands: refused with the reason: a device nobody knows, a path the device does not serve, an unbound slot's name, a bad word; a client the allow-list refuses never reaches the instance")
{
    ServerOn on;
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Commanded c ("refused", rig, "refused");
    rig.waitReady (c.instance);

    CHECK (c.write (c.slot (1) + "/path", { osc::Value::string (gain.fullPath), osc::Value::string ("Nobody's Renderer") }) == 0);
    CHECK (juce::String (c.lastWarning()).contains ("known to nobody"));

    CHECK (c.write (c.slot (1) + "/path", { osc::Value::string ("/not/served/here") }) == 0);
    CHECK (juce::String (c.lastWarning()).contains ("has not served"));

    /*  An unbound slot has no display_name leaf to write: the server itself
        refuses that, counted, and the instance never hears of it. */
    {
        const auto refusedBefore = static_cast<juce::int64> (ServerOn::serverStatus().getProperty ("refused_writes", 0));
        const auto wakesBefore = c.wakes.load();
        REQUIRE (c.sender.send ("127.0.0.1", ServerOn::oscPort(), message (c.slot (1) + "/display_name", { osc::Value::string ("too soon") })));
        REQUIRE (waitUntil ([&] { return static_cast<juce::int64> (ServerOn::serverStatus().getProperty ("refused_writes", 0)) > refusedBefore; }));
        CHECK (c.wakes.load() == wakesBefore);
    }

    CHECK (c.write (c.slot (1) + "/path", { osc::Value::string (gain.fullPath) }) == 1);
    CHECK (c.write (c.slot (1) + "/scaling", { osc::Value::string ("quadratic") }) == 0);
    CHECK (juce::String (c.lastWarning()).contains ("linear, log or exp"));

    /*  The server's own refusals: a client not on the list. */
    auto s = Registry::shared().settings();
    s.serverAllow = "192.0.2.1";
    Registry::shared().setSettings (s);

    const auto refusedBefore = static_cast<juce::int64> (ServerOn::serverStatus().getProperty ("refused_writes", 0));
    const auto wakesBefore = c.wakes.load();
    REQUIRE (c.sender.send ("127.0.0.1", ServerOn::oscPort(), message (c.slot (2) + "/path", { osc::Value::string (gain.fullPath) })));
    REQUIRE (waitUntil ([&] { return static_cast<juce::int64> (ServerOn::serverStatus().getProperty ("refused_writes", 0)) > refusedBefore; }));
    CHECK (c.wakes.load() == wakesBefore);
    CHECK (c.instance.pollCommands() == 0);
    CHECK_FALSE (c.instance.chunk().slots[2].bound());

    /*  Over the websocket, admitted again: the same path. */
    s.serverAllow = "any";
    Registry::shared().setSettings (s);

    Device::Options options;
    options.host = "127.0.0.1";
    options.port = ServerOn::port();
    Device client (options);
    client.start();
    REQUIRE (waitUntil ([&] { return client.state() == Device::State::Listening; }));

    osc::Packet p;
    p.address = c.slot (2) + "/path";
    p.args = { osc::Value::string (gain.fullPath) };
    std::string error;
    const auto before = c.wakes.load();
    REQUIRE_MESSAGE (client.sendOsc (p, error), error);
    REQUIRE (waitUntil ([&] { return c.wakes.load() > before; }));
    CHECK (c.instance.pollCommands() == 1);
    CHECK (c.instance.chunk().slots[2].path == gain.fullPath);
}
