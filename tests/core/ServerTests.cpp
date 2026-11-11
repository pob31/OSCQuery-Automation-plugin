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
    THE PLUGIN'S OWN SERVER (devplan Phase 6b-2), read by the library's own
    client: the slot table of two instances at /plugin/<id>/slot/<n>, the
    devices and the server itself; C4 on our own leaves; C5 across a stop and
    a start; LISTEN on a slot's value as the host moves it; an instance that
    comes and goes told as PATH_ADDED and PATH_REMOVED; the announce through
    the Finder. The server is off in a test run until a case turns it on.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"
#include "support/HttpGet.h"
#include "support/MdnsResponder.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>
#include <core/registry/SettingsFile.h>
#include <core/server/PluginServer.h>
#include <core/server/ServerNamespace.h>

#include <oscquery/Device.h>
#include <oscquery/HostInfo.h>
#include <oscquery/tree/Tree.h>

#include <mutex>
#include <string>
#include <vector>

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

    /** The server on, loopback writes allowed, the Finder pointed at a responder of the test's. */
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

        ~ServerOn()
        {
            Registry::shared().setSettings (before);
        }

        static juce::var serverStatus() { return Registry::shared().status().getProperty ("server", {}); }
        static int port()               { return static_cast<int> (serverStatus().getProperty ("port", 0)); }
    };

    juce::var get (const std::string& path)
    {
        const auto reply = oqatest::get (ServerOn::port(), path);
        REQUIRE_MESSAGE (reply.status == 200, (path + " answered " + std::to_string (reply.status)));
        return juce::JSON::parse (juce::String (reply.body));
    }

    juce::var valueOf (const std::string& leaf)
    {
        return get (leaf).getProperty ("VALUE", {})[0];
    }

    juce::int64 registryIdOf (const Instance& instance, const std::string& label)
    {
        const auto whole = juce::JSON::parse (juce::String (instance.processStatusJson()));

        if (const auto* instances = whole.getProperty ("instances", {}).getArray())
            for (const auto& i : *instances)
                if (i.getProperty ("label", "").toString().toStdString() == label)
                    return static_cast<juce::int64> (i.getProperty ("id", -1));

        return -1;
    }

    Device::Options clientFor (int port)
    {
        Device::Options options;
        options.host = "127.0.0.1";
        options.port = port;
        options.reconnectInitialMs = 100;
        options.reconnectMaxMs = 1000;
        return options;
    }
}

//==============================================================================
TEST_CASE ("plugin server: two instances' slot tables, the devices and the server itself are served; our own leaves meet C4; the hash holds across a restart (C5)")
{
    ServerOn on;
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && ! n.unit.empty() && ! n.unit[0].empty(); });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });

    Instance a (api(), 8, "served-a");
    Instance b (api(), 8, "served-b");
    std::string why;
    REQUIRE (a.bind (2, gain, 0, a.addDevice (rig.device ("served")), why));
    REQUIRE (b.bind (5, pan, 0, b.addDevice (rig.device ("served")), why));
    a.setDisplayName (2, "the gain");
    a.connect();
    b.connect();
    rig.waitReady (a);

    const auto idA = registryIdOf (a, "served-a");
    const auto idB = registryIdOf (b, "served-b");
    REQUIRE (idA >= 0);
    REQUIRE (idB >= 0);

    /*  The tree, once the server has rebuilt it from the bound instances. */
    const auto slotA = ns::slotPath (static_cast<unsigned long> (idA), 2);
    REQUIRE (waitUntil ([&] { return oqatest::get (ServerOn::port(), slotA + "/path").status == 200
                                  && valueOf (slotA + "/path").toString().toStdString() == gain.fullPath; }));

    CHECK (valueOf (ns::join (ns::pluginPath (static_cast<unsigned long> (idA)), ns::label)).toString() == "served-a");
    CHECK (static_cast<int> (valueOf (ns::join (ns::pluginPath (static_cast<unsigned long> (idA)), ns::active))) == 1);
    CHECK (valueOf (slotA + "/device").toString() == "served");
    CHECK (valueOf (slotA + "/display_name").toString() == "the gain");
    CHECK (valueOf (slotA + "/type").toString() == "f");
    CHECK (valueOf (slotA + "/direction").toString() == "send");
    CHECK (valueOf (slotA + "/scaling").toString() == "linear");
    CHECK (static_cast<double> (valueOf (slotA + "/range/min")) == doctest::Approx (*gain.argument (0).range->min));
    CHECK (static_cast<double> (valueOf (slotA + "/range/max")) == doctest::Approx (*gain.argument (0).range->max));
    REQUIRE (waitUntil ([&] { return valueOf (slotA + "/state").toString() == "ok"; }));

    /*  An unbound slot has a node with its path empty, its state said, and a
        device leaf for the next path written (6b-3); nothing else. */
    const auto unbound = ns::slotPath (static_cast<unsigned long> (idA), 0);
    CHECK (valueOf (unbound + "/path").toString().isEmpty());
    CHECK (valueOf (unbound + "/state").toString() == "unbound");
    CHECK (valueOf (unbound + "/device").toString().isEmpty());
    CHECK (oqatest::get (ServerOn::port(), unbound + "/display_name").status == 404);

    const auto slotB = ns::slotPath (static_cast<unsigned long> (idB), 5);
    CHECK (valueOf (slotB + "/path").toString().toStdString() == pan.fullPath);

    /*  The devices (in the registry's key order, so found by name: an earlier
        case's devices come first in a whole run) and the server. */
    std::string servedDevice;
    const auto deviceNode = get (ns::deviceRoot);                       // held: a pointer into a temporary var dangles
    const auto deviceContents = deviceNode.getProperty ("CONTENTS", {});

    if (const auto* contents = deviceContents.getDynamicObject())
        for (const auto& child : contents->getProperties())
            if (child.value.getProperty ("CONTENTS", {}).getProperty ("name", {}).getProperty ("VALUE", {})[0].toString() == "served")
                servedDevice = ns::join (ns::deviceRoot, child.name.toString().toStdString());

    REQUIRE_FALSE (servedDevice.empty());
    CHECK (valueOf (ns::join (servedDevice, ns::name)).toString() == "served");
    CHECK (static_cast<int> (valueOf (ns::join (servedDevice, ns::port))) == rig.mock.httpPort());
    CHECK (valueOf (ns::join (ns::serverRoot, ns::name)).toString().startsWith ("OSCQuery Automation @ "));
    CHECK (static_cast<int> (valueOf (ns::join (ns::serverRoot, ns::port))) == ServerOn::port());
    CHECK (static_cast<int> (valueOf (ns::join (ns::serverRoot, ns::processId))) > 0);

    /*  HOST_INFO (C1) and C4 on every leaf of ours. */
    const auto info = get ("/?HOST_INFO");
    CHECK (info.getProperty ("NAME", "").toString().startsWith ("OSCQuery Automation @ "));
    CHECK (static_cast<int> (info.getProperty ("OSC_PORT", 0)) == static_cast<int> (ServerOn::serverStatus().getProperty ("osc_port", -1)));
    CHECK (info.getProperty ("EXTENSIONS", {}).getProperty ("LISTEN", false) == juce::var (true));

    std::string error;
    const auto whole = Tree::parse (get ("/"), error);
    REQUIRE_MESSAGE (whole.has_value(), error);

    for (const auto* leaf : whole->leaves())
    {
        INFO ("leaf " << leaf->fullPath);
        REQUIRE (leaf->type.has_value());

        for (std::size_t i = 0; i < leaf->argumentCount(); ++i)
            if ((*leaf->type)[i] == 'f' || (*leaf->type)[i] == 'i')
                CHECK (leaf->argument (i).range.has_value());
    }

    /*  C5: the same tree after the server is stopped and started. */
    const auto hashBefore = whole->treeHash();
    auto s = Registry::shared().settings();
    s.serverEnabled = false;
    Registry::shared().setSettings (s);
    REQUIRE (waitUntil ([] { return ServerOn::port() == 0; }));
    s.serverEnabled = true;
    Registry::shared().setSettings (s);
    REQUIRE (waitUntil ([] { return ServerOn::port() > 0; }));
    REQUIRE (waitUntil ([&] { return oqatest::get (ServerOn::port(), slotA + "/path").status == 200; }));

    const auto again = Tree::parse (get ("/"), error);
    REQUIRE (again.has_value());
    CHECK (again->treeHash() == hashBefore);
}

TEST_CASE ("plugin server: LISTEN on a slot's value hears the host move it; an instance registering and leaving is PATH_ADDED and PATH_REMOVED")
{
    ServerOn on;
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Instance a (api(), 8, "listened-a");
    std::string why;
    REQUIRE (a.bind (1, gain, 0, a.addDevice (rig.device ("listened")), why));
    a.connect();
    rig.waitReady (a);

    const auto idA = registryIdOf (a, "listened-a");
    REQUIRE (idA >= 0);
    const auto valuePath = ns::join (ns::slotPath (static_cast<unsigned long> (idA), 1), ns::value);
    REQUIRE (waitUntil ([&] { return oqatest::get (ServerOn::port(), valuePath).status == 200; }));

    std::mutex mutex;
    std::vector<std::pair<std::string, osc::Values>> values;
    std::vector<std::string> added, removed;

    Device::Callbacks callbacks;
    callbacks.onValue = [&] (const std::string& p, const osc::Values& v) { std::lock_guard<std::mutex> l (mutex); values.emplace_back (p, v); };
    callbacks.onTreeChanged = [&] (const std::string& p) { std::lock_guard<std::mutex> l (mutex); added.push_back (p); };
    callbacks.onPathRemoved = [&] (const std::string& p) { std::lock_guard<std::mutex> l (mutex); removed.push_back (p); };

    Device client (clientFor (ServerOn::port()));
    client.setCallbacks (callbacks);
    client.start();
    REQUIRE (waitUntil ([&] { return client.state() == Device::State::Listening; }));
    REQUIRE (client.listen (valuePath) == Device::Listen::accepted);
    REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (mutex); return values.size() >= 1; }));   // the current value

    float block[8] = {};
    block[1] = 0.625f;
    a.processBlock (block, 8);
    REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (mutex); return values.size() >= 2 && values.back().second.size() == 1 && values.back().second[0].getFloat32() == doctest::Approx (0.625f); }));

    /*  Another instance arrives: PATH_ADDED of its node; it leaves: PATH_REMOVED. */
    juce::int64 idB = -1;

    {
        Instance b (api(), 8, "listened-b");
        b.connect();
        idB = registryIdOf (b, "listened-b");
        REQUIRE (idB >= 0);
        const auto pluginB = ns::pluginPath (static_cast<unsigned long> (idB));
        REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (mutex); return std::find (added.begin(), added.end(), pluginB) != added.end(); }));
        CHECK (oqatest::get (ServerOn::port(), ns::join (pluginB, ns::label)).status == 200);
    }

    const auto pluginB = ns::pluginPath (static_cast<unsigned long> (idB));
    REQUIRE (waitUntil ([&] { std::lock_guard<std::mutex> l (mutex); return std::find (removed.begin(), removed.end(), pluginB) != removed.end(); }));
    CHECK (oqatest::get (ServerOn::port(), pluginB).status == 404);

    /*  The status's word on the server, and the announce. */
    const auto status = ServerOn::serverStatus();
    CHECK (status.getProperty ("running", false) == juce::var (true));
    CHECK (status.getProperty ("enabled", false) == juce::var (true));
    CHECK (status.getProperty ("announcing", false) == juce::var (true));
    CHECK (status.getProperty ("error", "x").toString().isEmpty());
    CHECK (static_cast<int> (status.getProperty ("connections", 0)) >= 1);
}

TEST_CASE ("plugin server: a pinned HTTP port already in use is an error in the status, never another port; disabled, it serves nothing")
{
    ServerOn on;
    const auto taken = ServerOn::port();
    REQUIRE (taken > 0);

    /*  A second server (the library's) on the port ours holds; then ours told
        to move onto a port that is now that server's... the simplest way to a
        collision is to pin ours to the mock's HTTP port. */
    Rig rig;
    auto s = Registry::shared().settings();
    s.serverPort = rig.mock.httpPort();
    Registry::shared().setSettings (s);

    REQUIRE (waitUntil ([] { return ! ServerOn::serverStatus().getProperty ("error", "").toString().isEmpty(); }));
    CHECK (ServerOn::serverStatus().getProperty ("running", true) == juce::var (false));
    CHECK (ServerOn::serverStatus().getProperty ("error", "").toString().contains ("HTTP port"));
    CHECK (static_cast<int> (ServerOn::serverStatus().getProperty ("port", 1)) == 0);

    s.serverPort = 0;
    Registry::shared().setSettings (s);
    REQUIRE (waitUntil ([] { return ServerOn::port() > 0; }));
    CHECK (ServerOn::serverStatus().getProperty ("error", "x").toString().isEmpty());

    s.serverEnabled = false;
    Registry::shared().setSettings (s);
    REQUIRE (waitUntil ([] { return ServerOn::port() == 0; }));
    CHECK (ServerOn::serverStatus().getProperty ("running", true) == juce::var (false));
    CHECK (ServerOn::serverStatus().getProperty ("announcing", true) == juce::var (false));
}

//==============================================================================
//  Phase 6x-6: the server says of itself what it reads of others
//  (docs/spec/extensions-draft-0.1.md).
TEST_CASE ("plugin server: it serves and declares a DEVICE_ID that holds across a restart and a rename, and a CLOCK that claims the least")
{
    ServerOn on;

    std::string error;
    const auto first = parseHostInfo (juce::JSON::toString (get ("/?HOST_INFO")).toStdString(), error);
    REQUIRE (first.has_value());

    /*  Declared, both, and read by the library as any client would. */
    CHECK (first->has ("DEVICE_ID"));
    CHECK (first->has ("CLOCK"));
    REQUIRE (first->deviceId.has_value());
    CHECK (first->deviceId->size() == 36);                                   // a UUID in its dashed form
    CHECK ((*first->deviceId)[14] == '8');                                    // of the application's own making (RFC 9562, version 8)
    REQUIRE (first->clock.has_value());
    CHECK (first->clock->source == "local");
    CHECK_FALSE (first->clock->scheduling);
    CHECK_FALSE (first->clock->domain.has_value());

    //  The status says the same identifier, for the Monitor's server line.
    CHECK (ServerOn::serverStatus().getProperty ("device_id", "").toString().toStdString() == *first->deviceId);

    //  It is what the seed and this executable's name give, and nothing else.
    const auto seedFile = SettingsFile::serverSeedFile (Registry::shared().settingsFile());
    REQUIRE (seedFile.existsAsFile());
    const auto seed = seedFile.loadFileAsString().trim().toStdString();
    REQUIRE_FALSE (seed.empty());
    CHECK (SettingsFile::serverSeed (Registry::shared().settingsFile()) == seed);      // read, not made again
    CHECK (*first->deviceId == oqa::core::server::PluginServer::deviceIdFor (seed, oqa::core::server::PluginServer::hostApplicationName()));

    /*  Renamed (which restarts it, on another port): the same identifier. */
    auto renamed = Registry::shared().settings();
    renamed.serverName = "Renamed For The Test";
    Registry::shared().setSettings (renamed);
    REQUIRE (waitUntil ([] { return ServerOn::serverStatus().getProperty ("name", "").toString() == "Renamed For The Test" && ServerOn::port() > 0; }));

    const auto second = parseHostInfo (juce::JSON::toString (get ("/?HOST_INFO")).toStdString(), error);
    REQUIRE (second.has_value());
    CHECK (second->name == "Renamed For The Test");
    CHECK (second->deviceId == first->deviceId);

    /*  Off and on again: the same. */
    auto off = Registry::shared().settings();
    off.serverEnabled = false;
    Registry::shared().setSettings (off);
    REQUIRE (waitUntil ([] { return ! static_cast<bool> (ServerOn::serverStatus().getProperty ("running", true)); }));

    auto back = Registry::shared().settings();
    back.serverEnabled = true;
    Registry::shared().setSettings (back);
    REQUIRE (waitUntil ([] { return static_cast<bool> (ServerOn::serverStatus().getProperty ("running", false)) && ServerOn::port() > 0; }));

    const auto third = parseHostInfo (juce::JSON::toString (get ("/?HOST_INFO")).toStdString(), error);
    REQUIRE (third.has_value());
    CHECK (third->deviceId == first->deviceId);
}

TEST_CASE ("plugin server: the DEVICE_ID is one per seed and per host application, and none without a seed")
{
    using oqa::core::server::PluginServer;

    const auto reaper = PluginServer::deviceIdFor ("3f0a6c1e-1111-4222-8333-444455556666", "reaper");
    const auto live   = PluginServer::deviceIdFor ("3f0a6c1e-1111-4222-8333-444455556666", "Ableton Live 12 Suite");
    const auto other  = PluginServer::deviceIdFor ("9b7d2e40-aaaa-4bbb-8ccc-ddddeeeeffff", "reaper");

    CHECK (reaper == PluginServer::deviceIdFor ("3f0a6c1e-1111-4222-8333-444455556666", "reaper"));   // the same in, the same out
    CHECK (reaper != live);            // two DAWs sharing one settings file
    CHECK (reaper != other);           // two users, or two machines
    CHECK (reaper.size() == 36);
    CHECK (reaper[14] == '8');
    CHECK (std::string ("89ab").find (reaper[19]) != std::string::npos);
    CHECK (PluginServer::deviceIdFor ("", "reaper").empty());

    /*  The seed is a file of its own beside the settings file: made once, read
        after that, and not a setting (the settings file is not written for it). */
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("oqa-seed-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    const auto settingsFile = folder.getChildFile ("registry.json");

    CHECK (SettingsFile::serverSeedFile (settingsFile) == folder.getChildFile ("registry.json.server-id"));
    CHECK (SettingsFile::serverSeed ({}).empty());                       // no settings file at all, no seed

    const auto made = SettingsFile::serverSeed (settingsFile);
    CHECK (made.size() == 36);
    CHECK (SettingsFile::serverSeedFile (settingsFile).existsAsFile());
    CHECK_FALSE (settingsFile.existsAsFile());
    CHECK (SettingsFile::serverSeed (settingsFile) == made);

    folder.deleteRecursively();
}

//==============================================================================
//  Julien Bayle's retest of ec6725c, 2.11: the name is the process's, not a library's.
TEST_CASE ("plugin server: the host application is the process that loaded the plugin, in the NAME and under the DEVICE_ID")
{
    const auto host = oqa::core::server::PluginServer::hostApplicationName();
    const auto process = juce::File::getSpecialLocation (juce::File::hostApplicationPath).getFileNameWithoutExtension().toStdString();

    CHECK (host == process);
    CHECK_FALSE (host.empty());
    CHECK (host.find ("OSCQuery-Automation-Core") == std::string::npos);      // never the shared core library
    CHECK (juce::String (oqa::core::server::PluginServer::defaultName()).endsWith ("(" + juce::String (process) + ")"));
}
