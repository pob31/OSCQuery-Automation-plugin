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

#pragma once

/*
    THE MONITOR'S ROWS, HEADLESS (devplan Phase 6c; PRD, Architecture: "a
    Monitor plugin: it shows the registry's devices, every slot across all
    instances, packet rates and errors, and holds global settings, but owns
    nothing"). Everything the Monitor view draws is read here from the
    registry's process-wide status JSON (Instance::processStatusJson, 6a-2),
    so a test asserts the rows from a status fixture and the view only
    paints. juce_core only: no GUI module.
*/

#include <juce_core/juce_core.h>

#include <vector>

namespace oqa::editor
{
    struct MonitorModel
    {
        struct Device
        {
            juce::String name, identity, host;
            int port = 0;
            juce::String state, listen, resolution, lastError;
            bool reachable = false, senderReady = false;
            juce::int64 lastSeenMs = -1, lastErrorMsAgo = -1;
            double bundlesPerSecond = 0.0, messagesPerSecond = 0.0, receivedPerSecond = 0.0;
            bool honoursTimetags = false, timingByHand = false;
            double offsetMs = 0.0;

            /*  Phase 6x: whose word honoursTimetags is ("chunk", "device",
                "saved_hand" or "hand"), the clock the device declares when it
                declares one, and the DEVICE_ID it is known by. */
            juce::String timetagsOrigin, clockSource, clockDomain, deviceId;
            bool clockDeclared = false, clockScheduling = false;
            juce::int64 loopGuarded = 0, rateLimited = 0, datagramsDropped = 0, sendFailures = 0;

            /** "12 bundles/s, 40 msg/s, 3 rx/s" */
            juce::String ratesWords() const;

            /** The state, the websocket's word, and "seen 12 s ago" when unreachable. */
            juce::String stateWords() const;

            /** The last error with its age, or empty. */
            juce::String errorWords() const;

            /** "The device declares its clock: ptp, domain 0, scheduling by timetag. Ticked from that declaration." or empty. */
            juce::String timetagsWords() const;
        };

        /** The same words from one device's status object, for the editor's device pane. */
        static juce::String timetagsWords (const juce::var& deviceStatus);

        struct Slot
        {
            int id = 0;
            juce::String displayName, device, path, state, detail, direction, type;
            int argIndex = 0;
            int deviceIndex = -1;
        };

        struct Instance
        {
            juce::int64 id = 0;
            juce::String label;
            int number = 1;         ///< what $instance stands for
            int slotCount = 0;      ///< 0 for a Monitor
            bool active = false;
            std::vector<Slot> slots; ///< the bound ones, by id
        };

        /** One row of the slots table: an instance's bound slot. */
        struct SlotRow
        {
            int instance = 0;       ///< index into `instances`
            int slot = 0;           ///< index into that instance's `slots`
        };

        struct Server
        {
            bool present = false, enabled = false, running = false, announcing = false;
            juce::String name, error, deviceId;
            int port = 0, oscPort = 0, connections = 0;
            juce::int64 refusedWrites = 0, acceptedWrites = 0;

            /** The server line: name, ports, announce state or error, writes. */
            juce::String words() const;
        };

        std::vector<Device> devices;
        std::vector<Instance> instances;     ///< by id
        std::vector<SlotRow> slotRows;       ///< every bound slot of every instance, instances in order
        Server server;
        int instanceCount = 0;
        int receivePort = 0;                 ///< the UDP receiver's, 0 while it is not running
        bool finderRunning = false;
        juce::String settingsFilePath, settingsFileError;
        bool settingsFileLoaded = false;

        int boundSlots() const noexcept { return static_cast<int> (slotRows.size()); }

        const Instance& instanceOf (const SlotRow& row) const { return instances[static_cast<std::size_t> (row.instance)]; }
        const Slot& slotOf (const SlotRow& row) const { return instanceOf (row).slots[static_cast<std::size_t> (row.slot)]; }

        static MonitorModel fromStatus (const juce::var& processStatus);
    };
}
