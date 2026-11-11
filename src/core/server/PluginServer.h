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
    THE PLUGIN'S OWN OSCQUERY SERVER (devplan Phase 6b-2; PRD, Headless use:
    "everything in the editor is also reachable through the plugin's own
    OSCQuery server ... so Go.dot or a script can bind slots without opening
    the editor. Dogfooding the spec."). One per process, in the registry,
    serving the slot table of every instance (ServerNamespace.h) on the
    library's generic server, announced over DNS-SD by the Finder's Discovery
    under "OSCQuery Automation @ <machine> (<host app>)" (decided 2026-11-09),
    with /server/process_id as its identity leaf.

    HOW THE TREE IS MADE: from the registry's process-wide status, on this
    server's own thread, whenever the registry says something changed
    (rebuildSoon) and once a second regardless (the devices' standing). The
    new tree replaces the old whole; what differs is told to the clients as
    PATH_ADDED (an instance registered), PATH_REMOVED (one gone), PATH_CHANGED
    (a slot's attributes, an instance's leaves, a device, the server). Every
    slot has a node, so a write to an unbound slot's `path` has somewhere to
    land (6b-3); a bound slot has the rest of its leaves. The host's current
    values are pulled twenty times a second and streamed to whoever LISTENs.

    WRITES (6b-3): a slot's path (one string, with the device's NAME as an
    optional second string), device, range/min, range/max, scaling, direction,
    display_name and an instance's number are ACCESS 3; a write from a client
    the allow-list admits goes to Hooks::write, which the registry turns into
    a command for the instance (Registry::Impl::onServerWrite). The result is
    asynchronous by design: the shim applies it on its main thread, the next
    plan says what it made of it, and the tree follows.
*/

#include <core/registry/Registry.h>

#include <oscquery/server/OscQueryServer.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace oqa::core::server
{
    class PluginServer final : private juce::Thread
    {
    public:
        struct SlotValue
        {
            std::uint32_t instance;
            int slot;
            float normalised;
        };

        struct Hooks
        {
            /** The registry's process-wide status, as Registry::status() makes it. */
            std::function<juce::var()> processStatus;

            /** The host's current value of every bound slot of every instance. */
            std::function<void (std::vector<SlotValue>&)> pullValues;

            /** Announce over DNS-SD under `name`, and stop announcing. */
            std::function<void (const std::string& name, int httpPort, int oscPort)> announce;
            std::function<void()> unannounce;

            /** A write from an admitted client to a writable leaf (6b-3). Server thread. */
            std::function<void (const std::string& client, const std::string& path, const oscquery::osc::Values& values)> write;
        };

        explicit PluginServer (Hooks hooks);
        ~PluginServer() override;

        /*  Bind, build the first tree, announce; false with the reason kept
            for status() (a pinned port in use), never another port. */
        bool start (const RegistrySettings& settings, std::string& error);
        /** Stop serving; `unannounce` false leaves the DNS-SD record to whoever stops the Finder (the registry's shutdown). */
        void stop (bool unannounce = true);

        bool isRunning() const;

        /** The allow-list changed; nothing else needs a restart. */
        void setAllow (const std::string& allow);

        /** Something in the registry changed: the tree is rebuilt on the next tick. Any thread. */
        void rebuildSoon() noexcept { dirty.store (true, std::memory_order_relaxed); notify(); }

        /** For the status: name, port, osc_port, error, counters. */
        juce::var status() const;

        /** The NAME this process announces under, from the settings or the default. */
        static std::string defaultName();

        /*  The DEVICE_ID served (Phase 6x-6; docs/spec/extensions-draft-0.1.md):
            a UUID derived from the registry's per-user seed and the host
            application's name. The server is one per process and has no
            project to keep an identifier in; two DAWs on one machine share the
            settings file and must not serve one identifier; and renaming the
            server must not change it. Empty for an empty seed. Version 8 of
            RFC 9562 (a UUID of the application's own making): SHA-256 of the
            two, its first sixteen bytes, the version and variant bits set. */
        static std::string deviceIdFor (const std::string& seed, const std::string& hostApplication);
        static std::string hostApplicationName();

        /** The seed the next start() derives the DEVICE_ID from; empty serves none. */
        void setDeviceSeed (const std::string& seed);
        std::string name() const;

        int httpPort() const;
        int oscPort() const;

    private:
        void run() override;
        void rebuild();
        void streamValues();
        juce::var buildVar (const juce::var& status, const std::map<std::pair<std::uint32_t, int>, float>& values) const;

        Hooks hooks;
        oscquery::server::OscQueryServer server;

        mutable std::mutex lock;
        std::string serverName;
        std::string serverDeviceId;         // under lock: the DEVICE_ID served, or empty
        std::string deviceSeed;             // under lock: what it is derived from
        std::string lastError;              // under lock
        juce::var shownVar;                 // the tree as last built, for the diff (under lock)
        std::map<std::pair<std::uint32_t, int>, float> shownValues;

        std::atomic<bool> dirty { true };
        std::atomic<bool> running { false };
        juce::int64 lastRebuildMs = 0;
    };
}
