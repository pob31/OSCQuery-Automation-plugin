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

#include <core/server/PluginServer.h>
#include <core/server/ServerNamespace.h>

#include <oscquery/tree/Tree.h>

#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
#endif

namespace oqa::core::server
{
    namespace
    {
        constexpr int tickMs = 50;
        constexpr juce::int64 rebuildEveryMs = 1000;
        constexpr double deviceUnitsBound = 1.0e6;      // RANGE of the slot's own min and max (C4 on our leaves)

        int processId()
        {
          #if defined (_WIN32)
            return static_cast<int> (GetCurrentProcessId());
          #else
            return static_cast<int> (getpid());
          #endif
        }

        //  The OSCQuery shape, as the proposal spells it, built as JSON and
        //  parsed by the library's own Tree: one reading of the shape.
        juce::var rangeMinMax (double min, double max)
        {
            auto* r = new juce::DynamicObject();
            r->setProperty ("MIN", min);
            r->setProperty ("MAX", max);
            juce::Array<juce::var> list;
            list.add (juce::var (r));
            return list;
        }

        juce::var rangeVals (const std::vector<const char*>& names)
        {
            juce::Array<juce::var> vals;

            for (const auto* n : names)
                vals.add (juce::String (n));

            auto* r = new juce::DynamicObject();
            r->setProperty ("VALS", vals);
            juce::Array<juce::var> list;
            list.add (juce::var (r));
            return list;
        }

        juce::var leaf (const std::string& fullPath, const char* type, int access, const juce::var& value,
                        const juce::var& range = {}, const char* description = nullptr)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("FULL_PATH", juce::String (fullPath));
            o->setProperty ("TYPE", type);
            o->setProperty ("ACCESS", access);
            juce::Array<juce::var> values;
            values.add (value);
            o->setProperty ("VALUE", values);

            if (! range.isVoid())
                o->setProperty ("RANGE", range);

            if (description != nullptr)
                o->setProperty ("DESCRIPTION", description);

            return juce::var (o);
        }

        juce::var container (const std::string& fullPath)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("FULL_PATH", juce::String (fullPath));
            o->setProperty ("CONTENTS", juce::var (new juce::DynamicObject()));
            return juce::var (o);
        }

        void add (juce::var& parent, const std::string& childName, const juce::var& child)
        {
            parent.getProperty ("CONTENTS", {}).getDynamicObject()->setProperty (juce::String (childName), child);
        }

        juce::var childOf (const juce::var& parent, const juce::String& childName)
        {
            return parent.getProperty ("CONTENTS", {}).getProperty (childName, {});
        }

        bool sameJson (const juce::var& a, const juce::var& b)
        {
            return juce::JSON::toString (a, true) == juce::JSON::toString (b, true);
        }

        juce::StringArray childNames (const juce::var& parent)
        {
            juce::StringArray names;

            if (const auto* contents = parent.getProperty ("CONTENTS", {}).getDynamicObject())
                for (const auto& p : contents->getProperties())
                    names.add (p.name.toString());

            return names;
        }
    }

    //==============================================================================
    PluginServer::PluginServer (Hooks hooksIn)
        : juce::Thread ("oqa server"), hooks (std::move (hooksIn))
    {
    }

    PluginServer::~PluginServer()
    {
        stop();
    }

    /*  The process, not the module: `currentExecutableFile` is found from a
        symbol of JUCE's own, which lives in the shared core library, so it
        named that library ("libOSCQuery-Automation-Core") in every host and
        every DAW on a machine served one NAME and one DEVICE_ID (Julien Bayle's
        retest of ec6725c, 2.11). `hostApplicationPath` is the executable that
        loaded the plugin. */
    std::string PluginServer::hostApplicationName()
    {
        return juce::File::getSpecialLocation (juce::File::hostApplicationPath).getFileNameWithoutExtension().toStdString();
    }

    std::string PluginServer::defaultName()
    {
        return ("OSCQuery Automation @ " + juce::SystemStats::getComputerName() + " (" + juce::String (hostApplicationName()) + ")").toStdString();
    }

    std::string PluginServer::deviceIdFor (const std::string& seed, const std::string& hostApplication)
    {
        if (seed.empty())
            return {};

        const auto text = seed + "\n" + hostApplication;
        const auto digest = juce::SHA256 (text.data(), text.size()).getRawData();

        std::uint8_t bytes[16];
        std::memcpy (bytes, digest.getData(), sizeof (bytes));
        bytes[6] = static_cast<std::uint8_t> ((bytes[6] & 0x0f) | 0x80);     // version 8
        bytes[8] = static_cast<std::uint8_t> ((bytes[8] & 0x3f) | 0x80);     // the RFC's variant

        return juce::Uuid (bytes).toDashedString().toStdString();
    }

    void PluginServer::setDeviceSeed (const std::string& seed)
    {
        const std::lock_guard<std::mutex> guard (lock);
        deviceSeed = seed;
    }

    std::string PluginServer::name() const
    {
        const std::lock_guard<std::mutex> guard (lock);
        return serverName;
    }

    bool PluginServer::start (const RegistrySettings& settings, std::string& error)
    {
        stop();

        oscquery::server::OscQueryServer::Options options;
        options.httpPort = settings.serverPort;
        options.oscPort = settings.serverOscPort;
        options.name = settings.serverName.empty() ? defaultName() : settings.serverName;
        options.extensions = { "ACCESS", "VALUE", "RANGE", "TYPE", "DESCRIPTION", "LISTEN",
                               "PATH_CHANGED", "PATH_ADDED", "PATH_REMOVED" };
        options.allow = settings.serverAllow;

        /*  The two optional extensions (6x-6). The identifier, when the
            registry has a seed for it. The clock, claiming the least: writes
            are applied on arrival (a bundle's time tag is not read), and what
            disciplines the system clock is not this plugin's to know. */
        std::string seed;

        {
            const std::lock_guard<std::mutex> guard (lock);
            seed = deviceSeed;
        }

        if (const auto id = deviceIdFor (seed, hostApplicationName()); ! id.empty())
            options.deviceId = id;

        options.clock = oscquery::ClockInfo { "local", std::nullopt, false };

        {
            const std::lock_guard<std::mutex> guard (lock);
            serverDeviceId = options.deviceId.value_or (std::string());
            serverName = options.name;
            lastError.clear();
            shownVar = juce::var();
            shownValues.clear();
        }

        oscquery::server::OscQueryServer::Callbacks callbacks;
        callbacks.onWrite = [this] (const std::string& client, const std::string& path, const oscquery::osc::Values& values)
        {
            if (hooks.write)
                hooks.write (client, path, values);
        };

        if (! server.start (std::move (options), std::move (callbacks), error))
        {
            const std::lock_guard<std::mutex> guard (lock);
            lastError = error;
            return false;
        }

        running.store (true, std::memory_order_release);
        dirty.store (true, std::memory_order_relaxed);
        rebuild();
        startThread (Priority::normal);


        if (hooks.announce)
            hooks.announce (name(), server.httpPort(), server.oscPort());

        return true;
    }

    void PluginServer::stop (bool unannounce)
    {
        if (! running.exchange (false))
            return;

        signalThreadShouldExit();
        notify();
        stopThread (3000);

        if (unannounce && hooks.unannounce)
            hooks.unannounce();

        server.stop();
    }

    bool PluginServer::isRunning() const    { return running.load (std::memory_order_acquire) && server.isRunning(); }
    int PluginServer::httpPort() const      { return server.httpPort(); }
    int PluginServer::oscPort() const       { return server.oscPort(); }

    void PluginServer::setAllow (const std::string& allow)
    {
        server.setAllow (allow);
    }

    //==============================================================================
    void PluginServer::run()
    {
        while (! threadShouldExit())
        {
            wait (tickMs);

            if (threadShouldExit())
                break;

            const auto now = juce::Time::currentTimeMillis();

            if (dirty.exchange (false, std::memory_order_relaxed) || now - lastRebuildMs >= rebuildEveryMs)
                rebuild();

            streamValues();
        }
    }

    void PluginServer::rebuild()
    {

        if (! hooks.processStatus)
            return;

        lastRebuildMs = juce::Time::currentTimeMillis();

        std::vector<SlotValue> values;

        if (hooks.pullValues)
            hooks.pullValues (values);

        std::map<std::pair<std::uint32_t, int>, float> byKey;

        for (const auto& v : values)
            byKey[{ v.instance, v.slot }] = v.normalised;

        const auto status = hooks.processStatus();
        const auto fresh = buildVar (status, byKey);

        std::string error;
        auto tree = oscquery::Tree::parse (fresh, error);

        if (! tree.has_value())
        {
            const std::lock_guard<std::mutex> guard (lock);
            lastError = "the server's own tree would not parse: " + error;
            return;
        }

        juce::var previous;

        {
            const std::lock_guard<std::mutex> guard (lock);
            previous = shownVar;
            shownVar = fresh;
            shownValues = byKey;
        }

        server.setTree (std::move (*tree));

        if (previous.isVoid())
            return;

        /*  What changed, told after the tree is in place so a client's re-read
            sees it. Instances come and go whole; a slot's leaves, an
            instance's own leaves, a device and the server change in place. */
        const auto oldPlugins = childOf (previous, ns::pluginRoot + 1);
        const auto newPlugins = childOf (fresh, ns::pluginRoot + 1);
        const auto oldIds = childNames (oldPlugins);
        const auto newIds = childNames (newPlugins);

        for (const auto& id : newIds)
            if (! oldIds.contains (id))
                server.notifyAdded (ns::join (ns::pluginRoot, id.toStdString()));

        for (const auto& id : oldIds)
            if (! newIds.contains (id))
                server.notifyRemoved (ns::join (ns::pluginRoot, id.toStdString()));

        for (const auto& id : newIds)
        {
            if (! oldIds.contains (id))
                continue;

            const auto oldPlugin = childOf (oldPlugins, id);
            const auto newPlugin = childOf (newPlugins, id);
            const auto pluginPath = ns::join (ns::pluginRoot, id.toStdString());

            for (const auto* own : { ns::label, ns::instance, ns::active })
                if (! sameJson (childOf (oldPlugin, own), childOf (newPlugin, own)))
                    server.notifyChanged (ns::join (pluginPath, own));

            const auto oldSlots = childOf (oldPlugin, ns::slot);
            const auto newSlots = childOf (newPlugin, ns::slot);

            for (const auto& n : childNames (newSlots))
                if (! sameJson (childOf (oldSlots, n), childOf (newSlots, n)))
                    server.notifyChanged (ns::join (ns::join (pluginPath, ns::slot), n.toStdString()));
        }

        if (! sameJson (childOf (previous, ns::deviceRoot + 1), childOf (fresh, ns::deviceRoot + 1)))
            server.notifyChanged (ns::deviceRoot);

        if (! sameJson (childOf (previous, ns::serverRoot + 1), childOf (fresh, ns::serverRoot + 1)))
            server.notifyChanged (ns::serverRoot);
    }

    void PluginServer::streamValues()
    {
        if (! hooks.pullValues)
            return;

        std::vector<SlotValue> values;
        hooks.pullValues (values);

        std::vector<SlotValue> changed;

        {
            const std::lock_guard<std::mutex> guard (lock);

            for (const auto& v : values)
            {
                auto& shown = shownValues[{ v.instance, v.slot }];

                if (shown != v.normalised)
                {
                    shown = v.normalised;
                    changed.push_back (v);
                }
            }
        }

        for (const auto& v : changed)
            server.setValue (ns::join (ns::slotPath (v.instance, v.slot), ns::value), { oscquery::osc::Value::float32 (v.normalised) });
    }

    //==============================================================================
    juce::var PluginServer::buildVar (const juce::var& status, const std::map<std::pair<std::uint32_t, int>, float>& values) const
    {
        constexpr int readOnly = 1;
        constexpr int writable = 3;

        auto root = container ("/");
        auto plugins = container (ns::pluginRoot);
        auto devices = container (ns::deviceRoot);
        auto self = container (ns::serverRoot);

        if (const auto* instances = status.getProperty ("instances", {}).getArray())
        {
            for (const auto& i : *instances)
            {
                const auto id = static_cast<std::uint32_t> (static_cast<juce::int64> (i.getProperty ("id", 0)));
                const auto slotCount = static_cast<int> (i.getProperty ("slot_count", 0));

                if (slotCount <= 0)
                    continue;       // the Monitor's own instance has no slots to serve

                const auto pluginPath = ns::pluginPath (id);
                auto plugin = container (pluginPath);
                add (plugin, ns::label, leaf (ns::join (pluginPath, ns::label), "s", readOnly, i.getProperty ("label", "")));
                add (plugin, ns::instance, leaf (ns::join (pluginPath, ns::instance), "i", writable, static_cast<int> (i.getProperty ("instance", 1)), rangeMinMax (1, 9999), "what $instance stands for in this instance's paths"));
                add (plugin, ns::active, leaf (ns::join (pluginPath, ns::active), "i", readOnly, static_cast<bool> (i.getProperty ("active", true)) ? 1 : 0, rangeMinMax (0, 1)));

                auto slots = container (ns::join (pluginPath, ns::slot));

                std::map<int, juce::var> bound;

                if (const auto* list = i.getProperty ("slots", {}).getArray())
                    for (const auto& s : *list)
                        bound[static_cast<int> (s.getProperty ("id", -1))] = s;

                for (int n = 0; n < slotCount; ++n)
                {
                    const auto slotPath = ns::slotPath (id, n);
                    auto slot = container (slotPath);
                    const auto found = bound.find (n);

                    if (found == bound.end())
                    {
                        add (slot, ns::path, leaf (ns::join (slotPath, ns::path), "s", writable, "", {}, "the OSC path to bind; a second string names the device"));
                        add (slot, ns::device, leaf (ns::join (slotPath, ns::device), "s", writable, "", {}, "the device's NAME for the next path written"));
                        add (slot, ns::state, leaf (ns::join (slotPath, ns::state), "s", readOnly, "unbound"));
                    }
                    else
                    {
                        const auto& s = found->second;
                        add (slot, ns::path, leaf (ns::join (slotPath, ns::path), "s", writable, s.getProperty ("path", ""), {}, "the bound OSC path; empty unbinds; a second string names the device"));
                        add (slot, ns::device, leaf (ns::join (slotPath, ns::device), "s", writable, s.getProperty ("device", ""), {}, "the device's NAME"));
                        add (slot, ns::state, leaf (ns::join (slotPath, ns::state), "s", readOnly, s.getProperty ("state", "pending")));
                        add (slot, ns::argIndex, leaf (ns::join (slotPath, ns::argIndex), "i", readOnly, static_cast<int> (s.getProperty ("arg_index", 0)), rangeMinMax (0, 3)));
                        add (slot, ns::type, leaf (ns::join (slotPath, ns::type), "s", readOnly, s.getProperty ("type", "f"), rangeVals ({ "f", "i", "T", "F" })));

                        auto range = container (ns::join (slotPath, ns::range));
                        const auto rangePath = ns::join (slotPath, ns::range);
                        add (range, ns::min, leaf (ns::join (rangePath, ns::min), "f", writable, static_cast<double> (s.getProperty ("range_min", 0.0)), rangeMinMax (-deviceUnitsBound, deviceUnitsBound), "device units"));
                        add (range, ns::max, leaf (ns::join (rangePath, ns::max), "f", writable, static_cast<double> (s.getProperty ("range_max", 1.0)), rangeMinMax (-deviceUnitsBound, deviceUnitsBound), "device units"));
                        add (slot, ns::range, range);

                        add (slot, ns::scaling, leaf (ns::join (slotPath, ns::scaling), "s", writable, s.getProperty ("scaling", "linear"), rangeVals ({ "linear", "log", "exp" })));
                        add (slot, ns::direction, leaf (ns::join (slotPath, ns::direction), "s", writable, s.getProperty ("direction", "send"), rangeVals ({ "send", "receive", "both" })));
                        add (slot, ns::displayName, leaf (ns::join (slotPath, ns::displayName), "s", writable, s.getProperty ("display_name", "")));

                        const auto v = values.find ({ id, n });
                        add (slot, ns::value, leaf (ns::join (slotPath, ns::value), "f", readOnly, static_cast<double> (v != values.end() ? v->second : 0.0f), rangeMinMax (0, 1), "the host's value, normalised"));
                    }

                    add (slots, std::to_string (n), slot);
                }

                add (plugin, ns::slot, slots);
                add (plugins, std::to_string (id), plugin);
            }
        }

        if (const auto* list = status.getProperty ("devices", {}).getArray())
        {
            int index = 0;

            for (const auto& d : *list)
            {
                const auto devicePath = ns::devicePath (index);
                auto device = container (devicePath);
                add (device, ns::name, leaf (ns::join (devicePath, ns::name), "s", readOnly, d.getProperty ("name", "")));
                add (device, ns::host, leaf (ns::join (devicePath, ns::host), "s", readOnly, d.getProperty ("host", "")));
                add (device, ns::port, leaf (ns::join (devicePath, ns::port), "i", readOnly, static_cast<int> (d.getProperty ("port", 0)), rangeMinMax (0, 65535)));
                add (device, ns::resolution, leaf (ns::join (devicePath, ns::resolution), "s", readOnly, d.getProperty ("resolution", "")));
                add (device, ns::reachable, leaf (ns::join (devicePath, ns::reachable), "i", readOnly, static_cast<bool> (d.getProperty ("reachable", false)) ? 1 : 0, rangeMinMax (0, 1)));
                add (device, ns::state, leaf (ns::join (devicePath, ns::state), "s", readOnly, d.getProperty ("state", "")));
                add (device, ns::honoursTimetags, leaf (ns::join (devicePath, ns::honoursTimetags), "i", readOnly, static_cast<bool> (d.getProperty ("honours_timetags", false)) ? 1 : 0, rangeMinMax (0, 1)));
                add (device, ns::offsetMs, leaf (ns::join (devicePath, ns::offsetMs), "f", readOnly, static_cast<double> (d.getProperty ("offset_ms", 0.0)), rangeMinMax (-10000, 10000), "milliseconds"));
                add (devices, std::to_string (index), device);
                ++index;
            }
        }

        add (self, ns::name, leaf (ns::join (ns::serverRoot, ns::name), "s", readOnly, juce::String (name())));
        add (self, ns::processId, leaf (ns::join (ns::serverRoot, ns::processId), "i", readOnly, processId(), rangeMinMax (0, 2147483647), "this process; the identity leaf of this server"));
        add (self, ns::port, leaf (ns::join (ns::serverRoot, ns::port), "i", readOnly, server.httpPort(), rangeMinMax (0, 65535)));
        add (self, ns::oscPort, leaf (ns::join (ns::serverRoot, ns::oscPort), "i", readOnly, server.oscPort(), rangeMinMax (0, 65535)));
        add (self, ns::announcing, leaf (ns::join (ns::serverRoot, ns::announcing), "i", readOnly, static_cast<bool> (status.getProperty ("finder_running", false)) ? 1 : 0, rangeMinMax (0, 1)));

        add (root, ns::pluginRoot + 1, plugins);
        add (root, ns::deviceRoot + 1, devices);
        add (root, ns::serverRoot + 1, self);
        return root;
    }

    //==============================================================================
    juce::var PluginServer::status() const
    {
        auto* o = new juce::DynamicObject();

        {
            const std::lock_guard<std::mutex> guard (lock);
            o->setProperty ("name", juce::String (serverName));
            o->setProperty ("device_id", juce::String (serverDeviceId));
            o->setProperty ("error", juce::String (lastError));
        }

        o->setProperty ("running", isRunning());
        o->setProperty ("port", server.httpPort());
        o->setProperty ("osc_port", server.oscPort());
        o->setProperty ("refused_writes", static_cast<juce::int64> (server.refusedWrites()));
        o->setProperty ("accepted_writes", static_cast<juce::int64> (server.acceptedWrites()));
        o->setProperty ("connections", server.connectionCount());
        return juce::var (o);
    }
}
