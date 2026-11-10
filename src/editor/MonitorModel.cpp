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

#include "MonitorModel.h"
#include "SlotPresentation.h"

#include <algorithm>
#include <cmath>

namespace oqa::editor
{
    namespace
    {
        juce::String rate (double perSecond)
        {
            const bool whole = std::abs (perSecond - std::round (perSecond)) < 0.05;
            return juce::String (perSecond, whole || perSecond >= 10.0 ? 0 : 1);
        }
    }

    juce::String MonitorModel::Device::ratesWords() const
    {
        return rate (bundlesPerSecond) + " bundles/s, " + rate (messagesPerSecond) + " msg/s, " + rate (receivedPerSecond) + " rx/s";
    }

    juce::String MonitorModel::Device::timetagsWords() const
    {
        juce::String words;

        if (clockDeclared)
            words << "The device declares its clock: " << (clockSource.isNotEmpty() ? clockSource : juce::String ("unnamed"))
                  << (clockDomain.isNotEmpty() ? ", domain " + clockDomain : juce::String())
                  << (clockScheduling ? ", scheduling by timetag." : ", applying on arrival.");

        juce::String whose;

        if (timetagsOrigin == "device")
            whose = honoursTimetags ? "Ticked from that declaration; a hand overrules it." : "Unticked from that declaration; a hand overrules it.";
        else if (timetagsOrigin == "hand" || timetagsOrigin == "saved_hand")
            whose = clockDeclared ? "Set by hand, over the declaration." : "Set by hand.";

        if (whose.isNotEmpty())
            words << (words.isNotEmpty() ? " " : "") << whose;

        return words;
    }

    namespace
    {
        void readTiming (const juce::var& d, MonitorModel::Device& out)
        {
            out.honoursTimetags = static_cast<bool> (d.getProperty ("honours_timetags", false));
            out.timetagsOrigin = d.getProperty ("timetags_origin", "").toString();
            out.deviceId = d.getProperty ("device_id", "").toString();

            const auto clock = d.getProperty ("clock", {});
            out.clockDeclared = clock.isObject() && ! clock.isArray();

            if (out.clockDeclared)
            {
                out.clockSource = clock.getProperty ("source", "").toString();
                out.clockDomain = clock.getProperty ("domain", "").toString();
                out.clockScheduling = static_cast<bool> (clock.getProperty ("scheduling", false));
            }
        }
    }

    juce::String MonitorModel::timetagsWords (const juce::var& deviceStatus)
    {
        Device device;
        readTiming (deviceStatus, device);
        return device.timetagsWords();
    }

    juce::String MonitorModel::Device::stateWords() const
    {
        juce::String words = state;

        if (listen == "listening")
            words << " (LISTEN)";
        else if (listen == "unsupported")
            words << " (UDP, no LISTEN)";

        if (! reachable)
            words << ", seen " << juce::String (presentation::agoWords (lastSeenMs));

        return words;
    }

    juce::String MonitorModel::Device::errorWords() const
    {
        if (lastError.isEmpty())
            return {};

        return lastErrorMsAgo >= 0 ? lastError + " (" + juce::String (presentation::agoWords (lastErrorMsAgo)) + ")" : lastError;
    }

    juce::String MonitorModel::Server::words() const
    {
        if (! present)
            return "Server: not available on this registry";

        if (! enabled)
            return "Server: off (settings)";

        juce::String words = "Server:" + (name.isNotEmpty() ? " " + name : juce::String());

        if (! running)
            return words + (error.isNotEmpty() ? " failed: " + error : juce::String (" starting"));

        words << ", HTTP and websocket on port " << juce::String (port) << ", OSC on UDP " << juce::String (oscPort)
              << (announcing ? ", announced over DNS-SD" : ", not announced");

        if (error.isNotEmpty())
            words << " (" << error << ")";

        words << "; " << juce::String (connections) << " connection" << (connections == 1 ? "" : "s")
              << ", " << juce::String (acceptedWrites) << " writes accepted, " << juce::String (refusedWrites) << " refused";
        return words;
    }

    //==============================================================================
    MonitorModel MonitorModel::fromStatus (const juce::var& status)
    {
        MonitorModel m;

        if (const auto* devices = status.getProperty ("devices", {}).getArray())
        {
            for (const auto& d : *devices)
            {
                if (d.isVoid())
                    continue;

                Device out;
                out.name = d.getProperty ("name", "").toString();
                out.identity = d.getProperty ("identity_value", "").toString();
                out.host = d.getProperty ("host", "").toString();
                out.port = static_cast<int> (d.getProperty ("port", 0));
                out.state = d.getProperty ("state", "").toString();
                out.listen = d.getProperty ("listen", "").toString();
                out.resolution = d.getProperty ("resolution", "").toString();
                out.lastError = d.getProperty ("last_error", "").toString();
                out.reachable = static_cast<bool> (d.getProperty ("reachable", false));
                out.senderReady = static_cast<bool> (d.getProperty ("sender_ready", false));
                out.lastSeenMs = static_cast<juce::int64> (d.getProperty ("last_seen_ms", -1));
                out.lastErrorMsAgo = static_cast<juce::int64> (d.getProperty ("last_error_ms_ago", -1));
                readTiming (d, out);
                out.offsetMs = static_cast<double> (d.getProperty ("offset_ms", 0.0));
                out.timingByHand = static_cast<bool> (d.getProperty ("timing_by_hand", false));
                out.loopGuarded = static_cast<juce::int64> (d.getProperty ("loop_guarded", 0));
                out.rateLimited = static_cast<juce::int64> (d.getProperty ("rate_limited", 0));
                out.datagramsDropped = static_cast<juce::int64> (d.getProperty ("datagrams_dropped", 0));
                out.sendFailures = static_cast<juce::int64> (d.getProperty ("send_failures", 0));

                const auto rates = d.getProperty ("rates", {});
                out.bundlesPerSecond = static_cast<double> (rates.getProperty ("bundles_s", 0.0));
                out.messagesPerSecond = static_cast<double> (rates.getProperty ("messages_s", 0.0));
                out.receivedPerSecond = static_cast<double> (rates.getProperty ("received_s", 0.0))
                                      + static_cast<double> (rates.getProperty ("udp_received_s", 0.0));
                m.devices.push_back (std::move (out));
            }
        }

        if (const auto* instances = status.getProperty ("instances", {}).getArray())
        {
            for (const auto& i : *instances)
            {
                Instance out;
                out.id = static_cast<juce::int64> (i.getProperty ("id", 0));
                out.label = i.getProperty ("label", "").toString();
                out.number = static_cast<int> (i.getProperty ("instance", 1));
                out.slotCount = static_cast<int> (i.getProperty ("slot_count", 0));
                out.active = static_cast<bool> (i.getProperty ("active", false));

                if (const auto* slots = i.getProperty ("slots", {}).getArray())
                {
                    for (const auto& s : *slots)
                    {
                        Slot slot;
                        slot.id = static_cast<int> (s.getProperty ("id", 0));
                        slot.displayName = s.getProperty ("display_name", "").toString();
                        slot.device = s.getProperty ("device", "").toString();
                        slot.deviceIndex = static_cast<int> (s.getProperty ("device_index", -1));
                        slot.path = s.getProperty ("path", "").toString();
                        slot.argIndex = static_cast<int> (s.getProperty ("arg_index", 0));
                        slot.state = s.getProperty ("state", "").toString();
                        slot.detail = s.getProperty ("detail", "").toString();
                        slot.direction = s.getProperty ("direction", "").toString();
                        slot.type = s.getProperty ("type", "").toString();
                        out.slots.push_back (std::move (slot));
                    }
                }

                std::sort (out.slots.begin(), out.slots.end(), [] (const Slot& a, const Slot& b) { return a.id < b.id; });
                m.instances.push_back (std::move (out));
            }
        }

        std::sort (m.instances.begin(), m.instances.end(), [] (const Instance& a, const Instance& b) { return a.id < b.id; });

        for (std::size_t i = 0; i < m.instances.size(); ++i)
            for (std::size_t s = 0; s < m.instances[i].slots.size(); ++s)
                m.slotRows.push_back ({ static_cast<int> (i), static_cast<int> (s) });

        m.instanceCount = static_cast<int> (status.getProperty ("instance_count", static_cast<int> (m.instances.size())));
        m.receivePort = static_cast<int> (status.getProperty ("receive_port", 0));
        m.finderRunning = static_cast<bool> (status.getProperty ("finder_running", false));

        const auto file = status.getProperty ("settings_file", {});
        m.settingsFilePath = file.getProperty ("path", "").toString();
        m.settingsFileLoaded = static_cast<bool> (file.getProperty ("loaded", false));
        m.settingsFileError = file.getProperty ("error", "").toString();

        if (status.hasProperty ("server"))
        {
            const auto s = status["server"];
            m.server.present = true;
            m.server.enabled = static_cast<bool> (s.getProperty ("enabled", false));
            m.server.running = static_cast<bool> (s.getProperty ("running", false));
            m.server.announcing = static_cast<bool> (s.getProperty ("announcing", false));
            m.server.name = s.getProperty ("name", "").toString();
            m.server.error = s.getProperty ("error", "").toString();
            m.server.port = static_cast<int> (s.getProperty ("port", 0));
            m.server.oscPort = static_cast<int> (s.getProperty ("osc_port", 0));
            m.server.connections = static_cast<int> (s.getProperty ("connections", 0));
            m.server.refusedWrites = static_cast<juce::int64> (s.getProperty ("refused_writes", 0));
            m.server.acceptedWrites = static_cast<juce::int64> (s.getProperty ("accepted_writes", 0));
        }

        return m;
    }
}
