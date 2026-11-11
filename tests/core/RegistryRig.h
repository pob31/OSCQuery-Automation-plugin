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
    THE RIG THE REGISTRY'S TESTS SHARE (RegistryTests.cpp for the send path,
    ReceiveTests.cpp for record): a mock device on loopback, its tree parsed
    for choosing leaves by what they are, and the few readers of what the mock
    received. NO ADDRESS IS TYPED here or in the cases: every node is found in
    the fixture by its type, access, range or argument count.
*/

#include <doctest/doctest.h>

#include "TestSupport.h"
#include "support/WsTestClient.h"

#include <MockDevice.h>

#include <core/Instance.h>
#include <core/registry/RegistryApi.h>

#include <oscquery/osc/Codec.h>
#include <oscquery/tree/Tree.h>

#include <functional>
#include <string>
#include <vector>

namespace oqatest
{
    inline const OqaCoreApi* coreApi() { return oqa::core::staticCoreApi(); }

    struct Rig
    {
        MockDevice mock;
        oqa::oscquery::Tree tree;

        explicit Rig (MockDevice::Options options = {}, const juce::var& fixtureTree = {})
            : mock (options)
        {
            const auto root = fixtureTree.isVoid() ? juce::JSON::parse (fixture ("trees/generic-device.json")) : fixtureTree;
            REQUIRE (mock.loadTree (root));
            REQUIRE (mock.start());

            std::string error;
            auto parsed = oqa::oscquery::Tree::parse (root, error);
            REQUIRE_MESSAGE (parsed.has_value(), error);
            tree = *parsed;
        }

        /*  The session's reference to this mock. The mock takes the name too:
            since Phase 5b the registry drives a device only once HOST_INFO's
            NAME is the chunk's. */
        oqa::core::DeviceRef device (const std::string& name)
        {
            mock.setName (juce::String (name));

            oqa::core::DeviceRef ref;
            ref.name = name;
            ref.lastHost = "127.0.0.1";
            ref.lastPort = mock.httpPort();
            return ref;
        }

        const oqa::oscquery::Node& leafWhere (const std::function<bool (const oqa::oscquery::Node&)>& predicate) const
        {
            for (const auto* leaf : tree.leaves())
                if (predicate (*leaf))
                    return *leaf;

            FAIL ("the fixture has no such leaf");
            return tree.root();
        }

        /** The registry's status of `instance`'s device `deviceIndex`, or void. */
        static juce::var deviceStatus (const oqa::core::Instance& instance, int deviceIndex = 0)
        {
            const auto status = juce::JSON::parse (juce::String (instance.statusJson()));
            const auto* devices = status.getProperty ("devices", {}).getArray();

            if (devices == nullptr || deviceIndex >= devices->size())
                return {};

            return (*devices)[deviceIndex];
        }

        /** Wait until the registry can send to this mock from `instance`. */
        void waitReady (const oqa::core::Instance& instance, int deviceIndex = 0) const
        {
            const bool ready = waitUntil ([&]
            {
                const auto d = deviceStatus (instance, deviceIndex);
                return static_cast<bool> (d.getProperty ("sender_ready", false)) && static_cast<bool> (d.getProperty ("tree_loaded", false));
            });

            /*  A device that takes more than ten seconds to be ready on
                loopback has been seen, rarely, on macOS (the Mac mini and CI
                runners, 2026-11-11) and is not explained yet: where it stood
                is printed, so that the next time says which stage waited. */
            if (! ready)
                MESSAGE ("not ready after 10 s; the device's status: " << juce::JSON::toString (deviceStatus (instance, deviceIndex), true).toStdString());

            REQUIRE (ready);
        }

        /** Wait until the registry's websocket to this mock is open and streaming. */
        void waitListening (const oqa::core::Instance& instance, int deviceIndex = 0) const
        {
            REQUIRE (waitUntil ([&]
            {
                return deviceStatus (instance, deviceIndex).getProperty ("listen", "").toString() == "listening";
            }));
        }

        /*  Wait until a websocket client is connected to this mock. A change
            notification (PATH_REMOVED, PATH_RENAMED, PATH_CHANGED) goes to the
            websocket: sent before the client's is open, it is never heard, and
            a slot being "ok" does not say the websocket is up yet. Call this
            before making the mock change its tree (two such races lost on
            macOS runners, 2026-11-10 and 2026-11-11). */
        void waitHeard() const
        {
            REQUIRE (waitUntil ([&] { return mock.connectionCount() >= 1; }));
        }

        /*  On LISTEN the library reads each path's current value once and
            delivers it (a device need not push on LISTEN). Wait for those
            `paths` first values, then drain what reached the instance, so a
            case sees only what the device moves afterwards. */
        void settleListen (oqa::core::Instance& instance, int paths, int deviceIndex = 0) const
        {
            waitListening (instance, deviceIndex);
            REQUIRE (waitUntil ([&]
            {
                return static_cast<int> (deviceStatus (instance, deviceIndex).getProperty ("values_received", 0)) >= paths;
            }));
            juce::Thread::sleep (30);
            instance.pollReceived ({});
        }
    };

    inline std::vector<oqa::oscquery::osc::Packet> bundlesReceived (const MockDevice& mock)
    {
        std::vector<oqa::oscquery::osc::Packet> out;

        for (const auto& datagram : mock.receivedOsc())
        {
            const auto decoded = oqa::oscquery::osc::decode (datagram.data(), datagram.size());
            REQUIRE_MESSAGE (decoded.ok, decoded.error);
            out.push_back (decoded.packet);
        }

        return out;
    }

    inline std::size_t messagesIn (const std::vector<oqa::oscquery::osc::Packet>& bundles)
    {
        std::size_t n = 0;

        for (const auto& b : bundles)
            n += b.isBundle() ? b.elements.size() : 1;

        return n;
    }

    inline bool isRangedFloat (const oqa::oscquery::Node& n)
    {
        const auto a = n.argument (0);
        return n.type == "f" && n.bindableForSend() && a.range.has_value() && a.range->hasMinMax() && ! n.critical;
    }
}
