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
    AN OSCQUERY SERVER OVER A TREE (devplan Phase 6b-1; PRD, Headless use:
    "everything in the editor is also reachable through the plugin's own
    OSCQuery server"; devplan: "this is the first server the conformance CLI
    runs against in Phase 8, so it must pass its own matrix"). Generic: it
    serves whatever oscquery::Tree it is given, knows no path, and the owner
    (src/core/server/PluginServer for the plugin) decides what the tree holds
    and what a write means.

    What it does, by the profile draft (docs/spec/core-profile-draft-0.1.md):
      C1  GET /?HOST_INFO with NAME, OSC_PORT (a UDP endpoint of its own),
          OSC_TRANSPORT UDP, WS_PORT, EXTENSIONS as listed
      C6  ACCESS as reported to this client: a client the allow-list refuses
          sees every leaf's write bit off (AccessPolicy)
      C7  GET /<path> is that node and its descendants; an unknown path 404;
          ?ATTR 200 with the one attribute, 204 for one the node lacks (or a
          VALUE that may not be read), 400 for a name the proposal does not have
      L1  LISTEN / IGNORE as text frames; setValue() streams binary OSC to the
          path's listeners (one message per change; a bundle when told so)
      L2  PATH_ADDED, PATH_CHANGED, PATH_REMOVED, PATH_RENAMED to every
          connection, each only when EXTENSIONS lists it
      L3  OSC over the websocket (binary frames) and over OSC_PORT alike: a
          message to a writable leaf from a client the allow-list admits is a
          write (Callbacks::onWrite); anything else is refused and counted
      L4  binary frames are accepted, so a CRITICAL leaf has its TCP route

    On juce_simpleweb's SimpleWebSocketServer, as the mock device in tests/ is;
    the shape is the mock's, the mock stays the tests' (its fault injection is
    scaffolding, and a mock built on this would judge the server by itself).
    Callbacks run on the server's threads; the owner marshals.
*/

#include <oscquery/HostInfo.h>
#include <oscquery/osc/Value.h>
#include <oscquery/tree/Tree.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oqa::oscquery::server
{
    class OscQueryServer
    {
    public:
        struct Options
        {
            int httpPort = 0;                       ///< HTTP and websocket; 0 for an ephemeral port
            int oscPort = 0;                        ///< OSC_PORT, a UDP endpoint of its own; 0 for an ephemeral one
            bool websocket = true;
            std::string name;                       ///< HOST_INFO's NAME
            std::vector<std::string> extensions;    ///< what EXTENSIONS lists; LISTEN and the PATH_* govern behaviour
            std::string allow;                      ///< AccessPolicy's setting: who may write

            /*  The two optional extensions (docs/spec/extensions-draft-0.1.md).
                Set, each is served in HOST_INFO and listed in EXTENSIONS by the
                server itself; unset, HOST_INFO is what it was without them. */
            std::optional<std::string> deviceId;    ///< DEVICE_ID
            std::optional<ClockInfo> clock;         ///< CLOCK
        };

        struct Callbacks
        {
            /** A write from a client the allow-list admits, to a writable leaf that exists. Server thread. */
            std::function<void (const std::string& clientAddress, const std::string& path, const osc::Values& values)> onWrite;

            /** A write refused, and why (not allowed, no such path, read-only, not OSC). Server thread. */
            std::function<void (const std::string& clientAddress, const std::string& path, const std::string& why)> onRefused;
        };

        OscQueryServer();
        ~OscQueryServer();

        OscQueryServer (const OscQueryServer&) = delete;
        OscQueryServer& operator= (const OscQueryServer&) = delete;

        /** Bind and serve; false with the reason (a port in use). Serving the tree given before or after. */
        bool start (Options options, Callbacks callbacks, std::string& error);
        void stop();

        bool isRunning() const;
        int httpPort() const;
        int oscPort() const;
        Options options() const;

        /** The allow-list may change while serving. */
        void setAllow (const std::string& allow);

        //==============================================================================
        //  The tree. Every call takes the server's lock; the owner notifies as it
        //  changes the tree, through the methods that do both.

        /** Replace the tree whole. No notification: the owner knows what changed. */
        void setTree (Tree tree);
        Tree treeCopy() const;

        /** The leaf's VALUE, and the change streamed to its listeners (L1). False for no such leaf. */
        bool setValue (const std::string& path, const osc::Values& values);

        /** Merge a node (and its descendants) into the tree, then PATH_ADDED (new) or PATH_CHANGED. */
        bool replaceSubtree (Node node);

        /** Remove a path and its descendants, then PATH_REMOVED; its listeners are dropped. */
        bool removePath (const std::string& path);

        /** Rename a path, then PATH_RENAMED {OLD, NEW}; its listeners are dropped. */
        bool renamePath (const std::string& oldPath, const std::string& newPath);

        /** PATH_CHANGED for a path whose attributes changed in place. */
        void notifyChanged (const std::string& path);

        /** PATH_ADDED and PATH_REMOVED for an owner that replaced the tree whole with setTree(). */
        void notifyAdded (const std::string& path);
        void notifyRemoved (const std::string& path);

        //==============================================================================
        //  For the status and the tests.
        std::uint64_t refusedWrites() const;
        std::uint64_t acceptedWrites() const;
        int connectionCount() const;
        int listenerCount (const std::string& path) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
