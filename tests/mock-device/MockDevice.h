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
    THE MOCK OSCQUERY DEVICE (devplan, Phase 0): a tiny server that every later
    phase's tests drive instead of a real device, and the conformance CLI's first
    subject.

    It serves a tree loaded from a JSON fixture, verbatim in the shape a device
    answers `GET /` with, over HTTP and WebSocket on one port (juce_simpleweb):

        GET /?HOST_INFO          NAME, OSC_PORT (a real bound UDP port),
                                 OSC_TRANSPORT, WS_PORT, EXTENSIONS
        GET /                    the whole tree
        GET /<path>              that node and its descendants, else 404
        GET /<path>?<ATTRIBUTE>  200 with {"ATTRIBUTE": ...}; 204 when the node
                                 lacks it (or VALUE on a write-only node);
                                 400 for an attribute the proposal does not have

    Over the websocket it takes LISTEN and IGNORE, and a test changes the device
    under the client's feet with setValue, rename, remove, retype and add, which
    stream the value or send PATH_RENAMED / PATH_REMOVED / PATH_CHANGED /
    PATH_ADDED, each only if Options::extensions lists it.

    It knows no namespace: every address it serves comes from the fixture.
    Thread-safe: requests are answered on the server's own thread, controls are
    called from the test's, and the tree is behind one mutex.
*/

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace oqatest
{
    class MockDevice
    {
    public:
        struct Options
        {
            /** HOST_INFO's NAME. */
            juce::String name { "Mock OSCQuery device" };

            /** What HOST_INFO's EXTENSIONS lists. A websocket feature or
                notification not listed here is not served or sent. */
            juce::StringArray extensions { "ACCESS", "VALUE", "RANGE", "DESCRIPTION", "TYPE",
                                           "UNIT", "CLIPMODE", "CRITICAL", "LISTEN",
                                           "PATH_CHANGED", "PATH_ADDED", "PATH_REMOVED",
                                           "PATH_RENAMED" };

            /** False: a device without websocket (optional in the proposal).
                Upgrades are closed at once and HOST_INFO has no WS_PORT. */
            bool websocket = true;

            /** The HTTP port to bind; 0 lets the system choose, which is what
                lets tests run in parallel. */
            int httpPort = 0;

            /** True: a device that answers `?HOST_INFO` and nothing else (every
                other GET is 404), for the client's "reachable without a tree". */
            bool hostInfoOnly = false;

            /** True: notifications go out whether or not `extensions` lists them,
                as WFS-DIY sends PATH_CHANGED without listing it. */
            bool notifyUnlisted = false;

            /** "UDP" (a datagram socket is bound and read) or "TCP" (a listener is
                bound and every connection's bytes are kept, unframed). */
            juce::String oscTransport { "UDP" };

            /** Emitted as OSC_IP when not empty; absent otherwise, as most devices. */
            juce::String oscIp;

            /*  Members added to HOST_INFO as they are, when this is an object
                (Phase 6x: the optional DEVICE_ID and CLOCK, or anything else a
                device might put there). Nothing is added to `extensions` for
                them: a test lists the names itself, so that the device which
                serves a key without declaring it can be built too. */
            juce::var hostInfoExtras;

            /*  At or above zero: a device that applies what it is sent and
                streams it back to its LISTENers after this many milliseconds,
                as a real one echoes (Phase 4's loop guard). Below zero: off.
                UDP only; messages of f, i, T and F, and bundles of them. */
            int echoDelayMs = -1;

            /*  Per-client ACCESS (spec draft C6; Phase 5). Set: a request from
                an address not listed sees every leaf's ACCESS clamped to read
                (3 -> 1, 2 -> 0) in the tree, a subtree and `?ACCESS`, as a
                device's client table refuses a stranger writes; the stored
                tree is untouched, so node() still says what the owner sees.
                Unset: every client sees the tree's ACCESS. */
            std::optional<juce::StringArray> allowList;
        };

        MockDevice();
        explicit MockDevice (Options options);
        ~MockDevice();

        /** Loads a tree in `GET /` shape. False, and the tree unchanged, if it is
            not a JSON object. */
        bool loadTree (const juce::var& root);
        bool loadTreeFromFile (const juce::File& file);

        /** Binds HTTP/WebSocket and the UDP port behind OSC_PORT, and returns once
            the server answers. False if either cannot be bound. */
        bool start();
        void stop();
        bool isRunning() const;

        int httpPort() const;
        int oscPort() const;

        const Options& options() const;

        /** What `GET /?HOST_INFO` answers. */
        juce::var hostInfo() const;

        /** A deep copy of the node at `path` ("/" for the root), or void. */
        juce::var node (const juce::String& path) const;

        //==============================================================================
        //  Controls for tests. Each returns false, and changes nothing, when the
        //  path it needs does not exist (or, for rename and add, when the target
        //  already does).

        /** Sets VALUE and sends one binary OSC message to every client that
            LISTENs to `path`. One element per argument of the node's TYPE. */
        bool setValue (const juce::String& path, const juce::Array<juce::var>& values);

        /** Moves a node, rewriting FULL_PATH down its subtree; PATH_RENAMED. */
        bool rename (const juce::String& oldPath, const juce::String& newPath);

        /** Removes a node and its subtree; PATH_REMOVED. */
        bool remove (const juce::String& path);

        /** Changes a leaf's TYPE; PATH_CHANGED. */
        bool retype (const juce::String& path, const juce::String& newType);

        /** Adds `node` at `path` under an existing container, setting FULL_PATH
            down its subtree; PATH_ADDED. */
        bool add (const juce::String& path, const juce::var& node);

        /** How many websocket connections LISTEN to `path` right now. */
        int listenerCount (const juce::String& path) const;

        /** How many websocket connections are open. */
        int connectionCount() const;

        /** Sets each VALUE and sends ONE binary frame holding an OSC bundle of
            the messages (time tag "immediately") to every connection that
            LISTENs to any of the paths: the proposal's "changes may be coalesced". */
        bool setValueBundle (const std::vector<std::pair<juce::String, juce::Array<juce::var>>>& values);

        /** Closes every websocket connection from the server's side, as a
            device rebooting would. Listeners go with them. */
        void closeAllConnections();

        /*  Where this device also sends its values as UDP OSC, as a device
            told a client's address does (Phase 4: devices without LISTEN):
            setValue and setValueBundle send there too, from OSC_PORT. Port 0
            turns it off. UDP transport only. */
        void setFeedbackTarget (const juce::String& host, int port);

        /** Replaces Options::allowList while running: the device's client table edited under the client's feet. */
        void setAllowList (std::optional<juce::StringArray> allowList);

        /*  Replaces HOST_INFO's NAME. For a test rig that names the device the
            session refers to (Phase 5 compares NAME on reopen); set before the
            client asks, as a device's name is its own. */
        void setName (const juce::String& name);

        /*  Replaces Options::hostInfoExtras and Options::extensions. Set before
            the client asks, as with setName(): a device's word is its own. */
        void setHostInfoExtras (const juce::var& extras);
        void setExtensions (const juce::StringArray& extensions);

        /** Every binary frame received over the websocket, in order: the OSC a
            client sends a device this way (spec draft L3, L4). */
        std::vector<std::vector<std::uint8_t>> receivedWebsocketOsc() const;

        /** Every datagram received on OSC_PORT (UDP), in order. */
        std::vector<std::vector<std::uint8_t>> receivedOsc() const;

        /** Every byte received on OSC_PORT (TCP), in order, as the stream carried it. */
        std::vector<std::uint8_t> receivedOscStream() const;

        /** When each UDP datagram was read, one per entry of receivedOsc(), for a latency harness. */
        std::vector<std::chrono::steady_clock::time_point> receivedOscTimes() const;

        /** Forget everything received over UDP, TCP and the websocket. */
        void clearReceivedOsc();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE (MockDevice)
    };
}
