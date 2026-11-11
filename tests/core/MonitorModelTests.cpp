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
    THE MONITOR'S ROWS (devplan Phase 6c): from a status fixture, every row
    the view draws; and live, a Monitor instance of zero slots registered
    beside a slot plugin's, reading the process-wide status, and setting a
    device's timing by name so the slot plugin's chunk takes it on save.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>
#include <editor/MonitorModel.h>

using namespace oqa::core;
using namespace oqa::editor;
using namespace oqa::oscquery;
using oqatest::Rig;
using oqatest::waitUntil;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    const bool shutdownRegistered = (oqatest::registerShutdownHook ([] { Registry::shared().shutdown(); }), true);

    const char* fixture = R"json({
        "devices": [
            { "name": "Renderer", "identity_value": "SN-1", "host": "192.0.2.10", "port": 5005, "state": "listening", "listen": "listening",
              "reachable": true, "sender_ready": true, "resolution": "answered", "last_seen_ms": 120, "honours_timetags": true, "offset_ms": 12.5,
              "timing_by_hand": true, "loop_guarded": 3, "rates": { "bundles_s": 100.0, "messages_s": 230.0, "received_s": 4.0, "udp_received_s": 1.0 } },
            { "name": "Lights", "host": "192.0.2.11", "port": 9000, "state": "unreachable", "listen": "", "reachable": false, "last_seen_ms": 65000,
              "last_error": "connection refused", "last_error_ms_ago": 3000 }
        ],
        "instances": [
            { "id": 7, "label": "OSCQuery Automation 32 #2", "slot_count": 32, "active": false, "instance": 3,
              "slots": [ { "id": 4, "display_name": "azim", "device": "Renderer", "device_index": 0, "path": "/obj/3/azim", "arg_index": 0, "type": "f", "direction": "both", "state": "ok" },
                         { "id": 1, "display_name": "gain", "device": "Renderer", "device_index": 0, "path": "/obj/3/gain", "arg_index": 0, "type": "f", "direction": "send", "state": "missing", "detail": "not in the tree" } ] },
            { "id": 3, "label": "OSCQuery Automation 32 #1", "slot_count": 32, "active": true, "instance": 1,
              "slots": [ { "id": 0, "display_name": "dim", "device": "Lights", "device_index": 0, "path": "/dim", "arg_index": 1, "type": "f", "direction": "send", "state": "ok" } ] },
            { "id": 9, "label": "OSCQuery Automation Monitor #1", "slot_count": 0, "active": true, "instance": 1, "slots": [] }
        ],
        "instance_count": 3,
        "receive_port": 9001,
        "finder_running": true,
        "settings_file": { "path": "C:/u/settings.json", "loaded": true, "error": "" },
        "server": { "name": "OSCQuery Automation @ bench (REAPER)", "error": "", "running": true, "port": 51234, "osc_port": 51235,
                    "refused_writes": 2, "accepted_writes": 5, "connections": 1, "enabled": true, "announcing": true }
    })json";
}

//==============================================================================
TEST_CASE ("monitor model: every row from a status fixture, instances by id, slots by id, the server line in words")
{
    const auto status = juce::JSON::parse (juce::String (fixture));
    REQUIRE (status.isObject());
    const auto m = MonitorModel::fromStatus (status);

    REQUIRE (m.devices.size() == 2);
    CHECK (m.devices[0].name == "Renderer");
    CHECK (m.devices[0].identity == "SN-1");
    CHECK (m.devices[0].honoursTimetags);
    CHECK (m.devices[0].offsetMs == doctest::Approx (12.5));
    CHECK (m.devices[0].receivedPerSecond == doctest::Approx (5.0));
    CHECK (m.devices[0].ratesWords() == "100 bundles/s, 230 msg/s, 5 rx/s");
    CHECK (m.devices[0].stateWords() == "listening (LISTEN)");
    CHECK (m.devices[0].errorWords().isEmpty());
    CHECK_FALSE (m.devices[1].reachable);
    CHECK (m.devices[1].stateWords() == "unreachable, seen 65 s ago");
    CHECK (m.devices[1].errorWords() == "connection refused (3 s ago)");

    REQUIRE (m.instances.size() == 3);
    CHECK (m.instances[0].id == 3);
    CHECK (m.instances[1].id == 7);
    CHECK (m.instances[2].id == 9);
    CHECK (m.instances[2].slotCount == 0);
    CHECK (m.instances[1].number == 3);
    CHECK_FALSE (m.instances[1].active);
    REQUIRE (m.instances[1].slots.size() == 2);
    CHECK (m.instances[1].slots[0].id == 1);
    CHECK (m.instances[1].slots[1].id == 4);
    CHECK (m.instances[1].slots[0].state == "missing");

    REQUIRE (m.boundSlots() == 3);
    CHECK (m.slotOf (m.slotRows[0]).path == "/dim");
    CHECK (m.slotOf (m.slotRows[0]).argIndex == 1);
    CHECK (m.instanceOf (m.slotRows[1]).label == "OSCQuery Automation 32 #2");
    CHECK (m.slotOf (m.slotRows[2]).displayName == "azim");

    CHECK (m.instanceCount == 3);
    CHECK (m.receivePort == 9001);
    CHECK (m.finderRunning);
    CHECK (m.settingsFileLoaded);
    CHECK (m.settingsFilePath == "C:/u/settings.json");

    CHECK (m.server.present);
    CHECK (m.server.running);
    CHECK (m.server.port == 51234);
    const auto line = m.server.words();
    CHECK (line.contains ("OSCQuery Automation @ bench (REAPER)"));
    CHECK (line.contains ("port 51234"));
    CHECK (line.contains ("UDP 51235"));
    CHECK (line.contains ("announced over DNS-SD"));
    CHECK (line.contains ("5 writes accepted, 2 refused"));
    CHECK (line.contains ("1 connection,"));

    MonitorModel::Server off;
    CHECK (off.words() == "Server: not available on this registry");
    off.present = true;
    CHECK (off.words() == "Server: off (settings)");
    off.enabled = true;
    off.error = "port 8000 in use";
    CHECK (off.words() == "Server: failed: port 8000 in use");

    const auto empty = MonitorModel::fromStatus (juce::var());
    CHECK (empty.devices.empty());
    CHECK (empty.instances.empty());
    CHECK (empty.boundSlots() == 0);
    CHECK_FALSE (empty.server.present);
}

TEST_CASE ("monitor live: a zero-slot instance registers, reads the process-wide status, and sets a device's timing by name for every instance")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Instance a (api(), 8, "monitored");
    std::string why;
    const auto device = a.addDevice (rig.device ("monitored-device"));
    REQUIRE (a.bind (2, gain, 0, device, why));
    a.connect();
    rig.waitReady (a);

    /*  The Monitor: zero slots, registered, in the status but with nothing to bind. */
    Instance monitor (api(), 0, "monitor");
    monitor.connect();

    const auto status = juce::JSON::parse (juce::String (monitor.processStatusJson()));
    const auto m = MonitorModel::fromStatus (status);

    const auto find = [&] (const juce::String& label) -> const MonitorModel::Instance*
    {
        for (const auto& i : m.instances)
            if (i.label == label)
                return &i;

        return nullptr;
    };

    const auto* monitored = find ("monitored");
    const auto* self = find ("monitor");
    REQUIRE (monitored != nullptr);
    REQUIRE (self != nullptr);
    CHECK (self->slotCount == 0);
    CHECK (self->slots.empty());
    CHECK (monitored->slotCount == 8);
    REQUIRE (monitored->slots.size() == 1);
    CHECK (monitored->slots[0].id == 2);
    CHECK (monitored->slots[0].device == "monitored-device");
    CHECK (monitored->slots[0].path == juce::String (gain.fullPath));

    bool deviceListed = false;

    for (const auto& d : m.devices)
        deviceListed = deviceListed || d.name == "monitored-device";

    CHECK (deviceListed);

    /*  Timing by name from the Monitor: by hand, so the slot plugin's chunk
        takes it on its next save (6a-3). */
    CHECK_FALSE (a.chunk().devices[static_cast<std::size_t> (device)].honoursTimetags);
    monitor.setDeviceTimingByName ("monitored-device", "", true, 7.5);
    REQUIRE (waitUntil ([&]
    {
        const auto d = Rig::deviceStatus (a, device);
        return static_cast<bool> (d.getProperty ("honours_timetags", false)) && static_cast<bool> (d.getProperty ("timing_by_hand", false));
    }));

    a.saveState();
    CHECK (a.chunk().devices[static_cast<std::size_t> (device)].honoursTimetags);
    CHECK (a.chunk().devices[static_cast<std::size_t> (device)].offsetMs == doctest::Approx (7.5));

    /*  Pointing by name: the registry drives the device at the new address
        and the slot plugin's status says so. */
    CHECK (monitor.pointDeviceByName ("monitored-device", "", "127.0.0.1", rig.mock.httpPort()));
    CHECK_FALSE (monitor.pointDeviceByName ("nobody-has-this-device", "", "127.0.0.1", 1));
}

//==============================================================================
//  Phase 6x-5: what the Monitor and the device pane say of a declared clock.
TEST_CASE ("monitor model: a declared clock and whose word the timetags setting is, in words")
{
    const auto status = juce::JSON::parse (juce::String (R"json({
        "devices": [
            { "name": "Scheduler", "host": "192.0.2.20", "port": 5005, "honours_timetags": true, "timetags_origin": "device",
              "device_id": "6f1c2a9e", "clock": { "source": "ptp", "domain": "0", "scheduling": true } },
            { "name": "Overruled", "host": "192.0.2.21", "port": 5005, "honours_timetags": false, "timetags_origin": "saved_hand",
              "clock": { "source": "ntp", "domain": null, "scheduling": true } },
            { "name": "Arrival", "host": "192.0.2.22", "port": 5005, "honours_timetags": false, "timetags_origin": "device",
              "clock": { "source": "local", "scheduling": false } },
            { "name": "By hand", "host": "192.0.2.23", "port": 5005, "honours_timetags": true, "timetags_origin": "hand", "clock": null },
            { "name": "Plain", "host": "192.0.2.24", "port": 5005, "honours_timetags": false, "timetags_origin": "chunk" }
        ] })json"));

    const auto m = MonitorModel::fromStatus (status);
    REQUIRE (m.devices.size() == 5);

    CHECK (m.devices[0].clockDeclared);
    CHECK (m.devices[0].clockScheduling);
    CHECK (m.devices[0].deviceId == "6f1c2a9e");
    CHECK (m.devices[0].timetagsWords() == "The device declares its clock: ptp, domain 0, scheduling by timetag. Ticked from that declaration; a hand overrules it.");
    CHECK (m.devices[1].timetagsWords() == "The device declares its clock: ntp, scheduling by timetag. Set by hand, over the declaration.");
    CHECK (m.devices[2].timetagsWords() == "The device declares its clock: local, applying on arrival. Unticked from that declaration; a hand overrules it.");
    CHECK_FALSE (m.devices[3].clockDeclared);
    CHECK (m.devices[3].timetagsWords() == "Set by hand.");
    CHECK (m.devices[4].timetagsWords().isEmpty());             // a device with no clock and no hand says nothing new

    //  The device pane asks with one device's status object.
    CHECK (MonitorModel::timetagsWords (status["devices"][0]) == m.devices[0].timetagsWords());
    CHECK (MonitorModel::timetagsWords (juce::var()).isEmpty());
}
