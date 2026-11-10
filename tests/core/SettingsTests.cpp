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
    THE SETTINGS ON DISK (devplan Phase 6a-1): a registry writes its settings
    to its file when they change and another registry on the same file reads
    them back; the server's keys round-trip with their clamps; a missing file
    is the defaults with no complaint, a broken one the defaults with the
    reason in the status; the test run's own file, never the user's. Under C
    and fr_FR like the rest, because two of the values are decimals.
*/

#include <doctest/doctest.h>

#include "TestSupport.h"
#include "MockDevice.h"
#include "support/WsTestClient.h"

#include <core/registry/Registry.h>
#include <core/registry/SettingsFile.h>

#include <string>

using namespace oqa::core;

namespace
{
    struct TempFile
    {
        juce::File file { juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("oqa-settings-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()))
                            .getChildFile ("registry.json") };

        ~TempFile() { file.getParentDirectory().deleteRecursively(); }
    };
}

//==============================================================================
TEST_CASE ("settings: the server's keys round-trip through JSON with their clamps")
{
    RegistrySettings s;
    CHECK (s.serverEnabled);
    CHECK (s.serverPort == 0);
    CHECK (s.serverOscPort == 0);
    CHECK (s.serverAllow.empty());
    CHECK (s.serverName.empty());

    s.serverEnabled = false;
    s.serverPort = 5600;
    s.serverOscPort = 5601;
    s.serverAllow = "192.0.2.10, any";
    s.serverName = "Rig A";
    s.gestureTimeoutMs = 350.5;

    RegistrySettings back;
    back.applyVar (juce::JSON::parse (juce::JSON::toString (s.toVar())));
    CHECK_FALSE (back.serverEnabled);
    CHECK (back.serverPort == 5600);
    CHECK (back.serverOscPort == 5601);
    CHECK (back.serverAllow == "192.0.2.10, any");
    CHECK (back.serverName == "Rig A");
    CHECK (back.gestureTimeoutMs == doctest::Approx (350.5));

    RegistrySettings clamped;
    clamped.applyVar (juce::JSON::parse ("{\"server_port\": 99999, \"server_osc_port\": -4, \"server_allow\": \"  any  \"}"));
    CHECK (clamped.serverPort == 65535);
    CHECK (clamped.serverOscPort == 0);
    CHECK (clamped.serverAllow == "any");
}

TEST_CASE ("settings: a registry writes its file on change, and another registry on the same file starts from it")
{
    TempFile temp;

    {
        Registry a (temp.file);
        CHECK (a.settingsFile() == temp.file);
        CHECK_FALSE (temp.file.existsAsFile());

        const auto status = a.status();
        CHECK (status.getProperty ("settings_file", {}).getProperty ("loaded", true) == juce::var (false));
        CHECK (status.getProperty ("settings_file", {}).getProperty ("error", "x").toString().isEmpty());

        auto s = a.settings();
        s.receivePort = 9000;
        s.loopGuardMs = 75.5;
        s.serverPort = 5600;
        s.serverAllow = "any";
        a.setSettings (s);

        REQUIRE (temp.file.existsAsFile());
        CHECK (a.status().getProperty ("settings_file", {}).getProperty ("loaded", false) == juce::var (true));
        a.shutdown();
    }

    {
        Registry b (temp.file);
        const auto s = b.settings();
        CHECK (s.receivePort == 9000);
        CHECK (s.loopGuardMs == doctest::Approx (75.5));
        CHECK (s.serverPort == 5600);
        CHECK (s.serverAllow == "any");
        CHECK (b.status().getProperty ("settings_file", {}).getProperty ("loaded", false) == juce::var (true));
        b.shutdown();
    }

    /*  The file is JSON a person can read, with the locale-independent decimal. */
    const auto text = temp.file.loadFileAsString();
    CHECK (text.contains ("\"loop_guard_ms\": 75.5"));
    CHECK (text.contains ("\"server_allow\": \"any\""));
}

TEST_CASE ("settings: a broken file is the defaults with the reason in the status; no file at all keeps nothing on disk")
{
    TempFile temp;
    temp.file.getParentDirectory().createDirectory();
    temp.file.replaceWithText ("{ not json");

    Registry broken (temp.file);
    CHECK (broken.settings().receivePort == 0);
    const auto status = broken.status().getProperty ("settings_file", {});
    CHECK (status.getProperty ("loaded", true) == juce::var (false));
    CHECK (status.getProperty ("error", "").toString().contains ("not JSON"));
    broken.shutdown();

    Registry nowhere { juce::File {} };
    auto s = nowhere.settings();
    s.receivePort = 1234;
    nowhere.setSettings (s);
    CHECK (nowhere.settings().receivePort == 1234);
    CHECK (nowhere.settingsFile() == juce::File());
    nowhere.shutdown();
}

TEST_CASE ("settings: the shared registry of this test run reads the test run's file, not the user's")
{
    const auto file = Registry::shared().settingsFile();
    CHECK (file != juce::File());
    CHECK (file.getFileName().startsWith ("oqa-tests-"));
    CHECK (file != SettingsFile::defaultFile());
    CHECK (SettingsFile::defaultFile().getFileName() == "OSCQuery Automation Plugin.registry.json");
}

//==============================================================================
//  2026-11-10: DNS-SD can be turned off (docs/bench/2026-11-10-mdns-flood.md).
TEST_CASE ("settings: with discovery off the server is served and not announced, a typed device is still probed, and the key round-trips")
{
    RegistrySettings defaults;
    CHECK (defaults.discoveryEnabled);
    CHECK (defaults.toVar().getProperty ("discovery_enabled", false) == juce::var (true));

    RegistrySettings read;
    read.applyVar (juce::JSON::parse ("{\"discovery_enabled\": false}"));
    CHECK_FALSE (read.discoveryEnabled);
    CHECK (read.serverEnabled);                         // the rest stays

    /*  A registry of the test's own, started from a file that turns discovery
        off and the server on: what a validator's run is given. */
    TempFile temp;
    temp.file.getParentDirectory().createDirectory();
    temp.file.replaceWithText ("{ \"server_enabled\": true, \"discovery_enabled\": false }");

    oqatest::MockDevice mock;
    REQUIRE (mock.loadTree (juce::JSON::parse (oqatest::fixture ("trees/generic-device.json"))));
    REQUIRE (mock.start());

    Registry a (temp.file);
    CHECK_FALSE (a.settings().discoveryEnabled);

    const auto server = [&a] { return a.status().getProperty ("server", {}); };
    REQUIRE (oqatest::waitUntil ([&] { return static_cast<bool> (server().getProperty ("running", false)); }));
    CHECK_FALSE (static_cast<bool> (server().getProperty ("announcing", true)));

    //  Nothing started the Finder to announce the server.
    CHECK_FALSE (static_cast<bool> (a.status().getProperty ("finder_running", true)));

    /*  A device typed by hand is found all the same. */
    a.finderAddManual ("127.0.0.1", mock.httpPort());

    REQUIRE (oqatest::waitUntil ([&]
    {
        const auto status = a.status();                 // held: the list below lives in it

        if (const auto* found = status.getProperty ("found", {}).getArray())
            for (const auto& f : *found)
                if (static_cast<int> (f.getProperty ("port", 0)) == mock.httpPort())
                    return static_cast<bool> (f.getProperty ("reachable", false));

        return false;
    }, 15000));

    CHECK (static_cast<bool> (a.status().getProperty ("finder_running", false)));
    CHECK_FALSE (static_cast<bool> (server().getProperty ("announcing", true)));      // running, and still silent

    /*  Turned on while running, then off again: the status follows. */
    auto s = a.settings();
    s.discoveryEnabled = true;
    s.serverEnabled = false;                            // nothing of this test's goes to the real group
    a.setSettings (s);
    CHECK (a.settings().discoveryEnabled);
    CHECK (temp.file.loadFileAsString().contains ("\"discovery_enabled\": true"));

    a.shutdown();
}
