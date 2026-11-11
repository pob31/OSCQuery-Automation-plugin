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

#include <oscquery/server/OscQueryServer.h>

#include <oscquery/net/UdpEndpoint.h>
#include <oscquery/osc/Codec.h>
#include <oscquery/server/AccessPolicy.h>

#include <juce_simpleweb/juce_simpleweb.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace oqa::oscquery::server
{
    namespace
    {
        /*  SimpleWeb keeps its status-code table in a function-local static,
            made on the first reply. Made here instead, when this library
            loads, so it is destroyed after the registry singleton (made later,
            on first use) and not before it: a request answered while the
            process is ending, after the table had gone and before the server
            stopped, read a destroyed map (SIGSEGV in reply() on the NUC under
            clap-validator, 2026-11-09, a sibling process's Finder probing). */
        const bool statusCodesWarmed = ! SimpleWeb::status_code (SimpleWeb::StatusCode::success_ok).empty();

        //  Every attribute the proposal defines. Anything else is a 400.
        const juce::StringArray proposalAttributes { "FULL_PATH", "CONTENTS", "TYPE", "VALUE",
                                                     "RANGE", "ACCESS", "DESCRIPTION", "TAGS",
                                                     "EXTENDED_TYPE", "UNIT", "CRITICAL",
                                                     "CLIPMODE", "OVERLOADS", "HTML" };

        std::string normalisePath (juce::String path)
        {
            path = juce::URL::removeEscapeChars (path);

            if (! path.startsWithChar ('/'))
                path = "/" + path;

            while (path.length() > 1 && path.endsWithChar ('/'))
                path = path.dropLastCharacters (1);

            return path.toStdString();
        }

        bool isAtOrUnder (const std::string& path, const std::string& prefix)
        {
            return path == prefix || (prefix != "/" ? juce::String (path).startsWith (juce::String (prefix) + "/")
                                                    : true);
        }

        /** `node` (a deep copy) with every leaf's ACCESS less its write bit, down the subtree (C6). */
        juce::var clampedToRead (juce::var node)
        {
            if (auto* object = node.getDynamicObject())
            {
                if (object->hasProperty ("ACCESS"))
                    object->setProperty ("ACCESS", static_cast<int> (object->getProperty ("ACCESS")) & 1);
                else if (object->hasProperty ("TYPE"))
                    object->setProperty ("ACCESS", 1);

                if (auto* contents = object->getProperty ("CONTENTS").getDynamicObject())
                    for (auto& child : contents->getProperties())
                        contents->setProperty (child.name, clampedToRead (child.value));
            }

            return node;
        }

        SimpleWeb::StatusCode statusCodeFor (int status)
        {
            switch (status)
            {
                case 200: return SimpleWeb::StatusCode::success_ok;
                case 204: return SimpleWeb::StatusCode::success_no_content;
                case 400: return SimpleWeb::StatusCode::client_error_bad_request;
                case 404: return SimpleWeb::StatusCode::client_error_not_found;
                case 405: return SimpleWeb::StatusCode::client_error_method_not_allowed;
                default:  return SimpleWeb::StatusCode::server_error_internal_server_error;
            }
        }

        void reply (const std::shared_ptr<HttpServer::Response>& response, int status, const juce::String& body)
        {
            const SimpleWeb::CaseInsensitiveMultimap fields {
                { "Content-Type", status == 200 ? "application/json" : "text/plain" }
            };
            response->write (statusCodeFor (status), body.toStdString(), fields);
        }

        juce::String command (const char* name, const juce::var& data)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("COMMAND", name);
            object->setProperty ("DATA", data);
            return juce::JSON::toString (juce::var (object), true);
        }
    }

    //==============================================================================
    struct OscQueryServer::Impl final : public SimpleWebSocketServerBase::RequestHandler,
                                        public SimpleWebSocketServerBase::Listener
    {
        Options options;
        Callbacks callbacks;

        std::unique_ptr<SimpleWebSocketServer> server;
        net::UdpEndpoint udp;
        std::atomic<bool> initFailed { false };
        std::string initError;                                  // under lock

        mutable std::mutex lock;                                // the tree, listeners, connections, options.allow
        Tree tree;
        std::map<std::string, std::set<juce::String>> listeners;   // path -> connection ids
        std::set<juce::String> connections;

        std::atomic<std::uint64_t> refused { 0 };
        std::atomic<std::uint64_t> accepted { 0 };

        bool advertises (const char* extension) const
        {
            for (const auto& e : options.extensions)
                if (e == extension)
                    return true;

            return false;
        }

        std::string allowNow() const
        {
            const std::lock_guard<std::mutex> guard (lock);
            return options.allow;
        }

        //==============================================================================
        juce::var hostInfo() const
        {
            auto* info = new juce::DynamicObject();
            info->setProperty ("NAME", juce::String (options.name));
            info->setProperty ("OSC_PORT", udp.boundPort());
            info->setProperty ("OSC_TRANSPORT", "UDP");

            if (options.websocket && server != nullptr)
                info->setProperty ("WS_PORT", server->port);

            auto* extensions = new juce::DynamicObject();

            for (const auto& name : options.extensions)
                extensions->setProperty (juce::String (name), true);

            /*  The two optional extensions: served and declared together, by
                this function alone, so that one cannot go without the other. */
            if (options.deviceId.has_value() && ! options.deviceId->empty())
            {
                info->setProperty (deviceIdExtension, juce::String (*options.deviceId));
                extensions->setProperty (deviceIdExtension, true);
            }

            if (options.clock.has_value())
            {
                info->setProperty (clockExtension, options.clock->toVar());
                extensions->setProperty (clockExtension, true);
            }

            info->setProperty ("EXTENSIONS", juce::var (extensions));
            return juce::var (info);
        }

        //==============================================================================
        //  HTTP, on the server's threads.
        void serverInitError (const juce::String& message) override
        {
            {
                const std::lock_guard<std::mutex> guard (lock);
                initError = message.toStdString();
            }

            initFailed.store (true);
        }

        bool handleHTTPRequest (std::shared_ptr<HttpServer::Response> response,
                                std::shared_ptr<HttpServer::Request> request) override
        {
            if (request->method != "GET")
            {
                reply (response, 405, "GET only\n");
                return true;
            }

            const auto path = normalisePath (juce::String (request->path));
            const auto query = juce::String (request->query_string);

            if (query == "HOST_INFO")
            {
                reply (response, 200, juce::JSON::toString (hostInfo(), true));
                return true;
            }

            const auto client = AccessPolicy::normaliseAddress (request->remote_endpoint().address().to_string());

            int status = 200;
            juce::String body;

            {
                const std::lock_guard<std::mutex> guard (lock);
                const auto* found = tree.find (path);

                if (found == nullptr)
                {
                    status = 404;
                    body = "no node at " + juce::String (path) + "\n";
                }
                else
                {
                    auto node = Tree::toVar (*found);

                    /*  ACCESS as reported to this client (C6). */
                    if (! AccessPolicy::allows (options.allow, client))
                        node = clampedToRead (node);

                    if (query.isEmpty())
                    {
                        body = juce::JSON::toString (node, true);
                    }
                    else if (! proposalAttributes.contains (query))
                    {
                        status = 400;
                        body = "not an OSCQuery attribute: " + query + "\n";
                    }
                    else
                    {
                        const auto access = static_cast<int> (node.getProperty ("ACCESS", 3));
                        const bool unreadable = query == "VALUE" && (access & 1) == 0;

                        if (unreadable || ! node.getDynamicObject()->hasProperty (query))
                        {
                            status = 204;
                        }
                        else
                        {
                            auto* object = new juce::DynamicObject();
                            object->setProperty (query, node.getProperty (query, {}));
                            body = juce::JSON::toString (juce::var (object), true);
                        }
                    }
                }
            }

            reply (response, status, body);
            return true;
        }

        //==============================================================================
        //  WebSocket.
        void connectionOpened (const juce::String& id) override
        {
            if (! options.websocket)
            {
                server->closeConnection (id, 1000, "this server has no websocket");
                return;
            }

            const std::lock_guard<std::mutex> guard (lock);
            connections.insert (id);
        }

        void connectionClosed (const juce::String& id, int, const juce::String&) override
        {
            const std::lock_guard<std::mutex> guard (lock);
            connections.erase (id);

            for (auto& entry : listeners)
                entry.second.erase (id);
        }

        void connectionError (const juce::String& id, int, const juce::String&) override
        {
            connectionClosed (id, 0, {});
        }

        void messageReceived (const juce::String& id, const juce::String& message) override
        {
            if (! options.websocket || ! advertises ("LISTEN"))
                return;

            const auto parsed = juce::JSON::parse (message);
            const auto name = parsed.getProperty ("COMMAND", {}).toString();
            const auto data = parsed.getProperty ("DATA", {});

            if (! data.isString())
                return;

            const auto path = normalisePath (data.toString());
            const std::lock_guard<std::mutex> guard (lock);

            if (name == "LISTEN" && tree.find (path) != nullptr)
                listeners[path].insert (id);
            else if (name == "IGNORE")
                listeners[path].erase (id);
        }

        /** Binary frames are OSC: the same write path as OSC_PORT (L3). */
        void dataReceived (const juce::String& id, const juce::MemoryBlock& data) override
        {
            std::string client;

            if (server != nullptr)
                if (const auto connection = server->connectionMap[id]; connection != nullptr)
                    client = AccessPolicy::normaliseAddress (connection->remote_endpoint().address().to_string());

            handleOsc (client, static_cast<const std::uint8_t*> (data.getData()), data.getSize());
        }

        //==============================================================================
        //  Writes, from the websocket and from OSC_PORT alike.
        void handleOsc (const std::string& client, const std::uint8_t* bytes, std::size_t size)
        {
            const auto decoded = osc::decode (bytes, size);

            if (! decoded.ok)
            {
                refuse (client, {}, "not OSC: " + decoded.error);
                return;
            }

            handlePacket (client, decoded.packet);
        }

        void handlePacket (const std::string& client, const osc::Packet& packet)
        {
            if (packet.isBundle())
            {
                for (const auto& element : packet.elements)
                    handlePacket (client, element);

                return;
            }

            std::string why;

            {
                const std::lock_guard<std::mutex> guard (lock);
                const auto* node = tree.find (packet.address);

                if (node == nullptr)
                    why = "no such path";
                else if (node->isContainer())
                    why = "a container, not a leaf";
                else if ((node->effectiveAccess() & 2) == 0)
                    why = "read-only";
                else if (! AccessPolicy::allows (options.allow, client))
                    why = "not on the allow-list";
            }

            if (! why.empty())
            {
                refuse (client, packet.address, why);
                return;
            }

            accepted.fetch_add (1, std::memory_order_relaxed);

            if (callbacks.onWrite)
                callbacks.onWrite (client, packet.address, packet.args);
        }

        void refuse (const std::string& client, const std::string& path, const std::string& why)
        {
            refused.fetch_add (1, std::memory_order_relaxed);

            if (callbacks.onRefused)
                callbacks.onRefused (client, path, why);
        }

        //==============================================================================
        //  Sending.
        void sendBinaryTo (const juce::String& id, const std::vector<std::uint8_t>& bytes)
        {
            if (server == nullptr)
                return;

            const auto connection = server->connectionMap[id];

            if (connection == nullptr)
                return;

            auto message = std::make_shared<WsServer::OutMessage>();
            message->write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
            connection->send (message, nullptr, 130);   // 130: one binary frame
        }

        void broadcastText (const juce::String& text)
        {
            if (server == nullptr || ! options.websocket)
                return;

            const auto utf8 = text.toStdString();
            const juce::ScopedLock mapLock (server->connectionMap.getLock());

            for (decltype (server->connectionMap)::Iterator it (server->connectionMap); it.next();)
                it.getValue()->send (utf8);
        }

        void notify (const char* extension, const juce::var& data)
        {
            if (advertises (extension))
                broadcastText (command (extension, data));
        }

        void dropListenersAtOrUnder (const std::string& path)
        {
            for (auto it = listeners.begin(); it != listeners.end();)
                it = isAtOrUnder (it->first, path) ? listeners.erase (it) : std::next (it);
        }
    };

    //==============================================================================
    OscQueryServer::OscQueryServer() : impl (std::make_unique<Impl>()) {}

    OscQueryServer::~OscQueryServer()
    {
        stop();
    }

    bool OscQueryServer::start (Options options, Callbacks callbacks, std::string& error)
    {
        if (impl->server != nullptr)
        {
            error = "already running";
            return false;
        }

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            impl->options = std::move (options);
            impl->callbacks = std::move (callbacks);
            impl->initError.clear();
        }

        impl->initFailed.store (false);

        /*  OSC_PORT first: a UDP endpoint of this server's own, its datagrams
            the same writes as the websocket's binary frames. */
        if (! impl->udp.start (impl->options.oscPort, [this] (net::Datagram datagram)
            {
                impl->handleOsc (AccessPolicy::normaliseAddress (datagram.senderIp), datagram.bytes.data(), datagram.bytes.size());
            }))
        {
            error = "OSC port " + std::to_string (impl->options.oscPort) + " could not be bound";
            return false;
        }

        impl->server = std::make_unique<SimpleWebSocketServer>();
        impl->server->addHTTPRequestHandler (impl.get());
        impl->server->addWebSocketListener (impl.get());

       #if defined (_WIN32)
        constexpr auto reuseAddress = false;
       #else
        constexpr auto reuseAddress = true;
       #endif

        impl->server->start (impl->options.httpPort, {}, {}, reuseAddress);

        const auto ready = [this] { return impl->server->isConnected && impl->server->port != 0; };

        for (int i = 0; i < 2000 && ! ready() && ! impl->initFailed.load(); ++i)
            juce::Thread::sleep (5);

        if (! ready())
        {
            std::string why;

            {
                const std::lock_guard<std::mutex> guard (impl->lock);
                why = impl->initError;
            }

            error = "HTTP port " + std::to_string (impl->options.httpPort) + " could not be bound"
                    + (why.empty() ? std::string() : ": " + why);
            stop();
            return false;
        }

        return true;
    }

    void OscQueryServer::stop()
    {
        if (impl->server != nullptr)
        {
            impl->server->removeHTTPRequestHandler (impl.get());
            impl->server->stop();
            impl->server->removeWebSocketListener (impl.get());
            impl->server.reset();
        }

        impl->udp.stop();

        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->listeners.clear();
        impl->connections.clear();
    }

    bool OscQueryServer::isRunning() const      { return impl->server != nullptr && impl->server->isConnected; }
    int OscQueryServer::httpPort() const        { return impl->server != nullptr ? impl->server->port : 0; }
    int OscQueryServer::oscPort() const         { return impl->udp.isRunning() ? impl->udp.boundPort() : 0; }

    OscQueryServer::Options OscQueryServer::options() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->options;
    }

    void OscQueryServer::setAllow (const std::string& allow)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->options.allow = allow;
    }

    //==============================================================================
    void OscQueryServer::setTree (Tree tree)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->tree = std::move (tree);
    }

    Tree OscQueryServer::treeCopy() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->tree;
    }

    bool OscQueryServer::setValue (const std::string& path, const osc::Values& values)
    {
        std::vector<juce::String> ids;
        std::vector<std::uint8_t> frame;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            auto* node = impl->tree.find (path);

            if (node == nullptr || node->isContainer())
                return false;

            node->value = values;

            const auto found = impl->listeners.find (node->fullPath);

            if (found == impl->listeners.end() || found->second.empty())
                return true;

            osc::Packet message;
            message.kind = osc::Packet::Kind::message;
            message.address = node->fullPath;
            message.args = values;

            std::string error;
            const auto encoded = osc::encode (message, error);

            if (! encoded.has_value())
                return true;

            frame = *encoded;
            ids.assign (found->second.begin(), found->second.end());
        }

        for (const auto& id : ids)
            impl->sendBinaryTo (id, frame);

        return true;
    }

    bool OscQueryServer::replaceSubtree (Node node)
    {
        const auto path = node.fullPath;
        bool existed = false;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            existed = impl->tree.find (path) != nullptr;

            if (! impl->tree.merge (std::move (node)))
                return false;
        }

        impl->notify (existed ? "PATH_CHANGED" : "PATH_ADDED", juce::String (path));
        return true;
    }

    bool OscQueryServer::removePath (const std::string& path)
    {
        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (! impl->tree.remove (path))
                return false;

            impl->dropListenersAtOrUnder (path);
        }

        impl->notify ("PATH_REMOVED", juce::String (path));
        return true;
    }

    bool OscQueryServer::renamePath (const std::string& oldPath, const std::string& newPath)
    {
        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (! impl->tree.rename (oldPath, newPath))
                return false;

            impl->dropListenersAtOrUnder (oldPath);
        }

        auto* data = new juce::DynamicObject();
        data->setProperty ("OLD", juce::String (oldPath));
        data->setProperty ("NEW", juce::String (newPath));
        impl->notify ("PATH_RENAMED", juce::var (data));
        return true;
    }

    void OscQueryServer::notifyChanged (const std::string& path)
    {
        impl->notify ("PATH_CHANGED", juce::String (path));
    }

    void OscQueryServer::notifyAdded (const std::string& path)
    {
        impl->notify ("PATH_ADDED", juce::String (path));
    }

    void OscQueryServer::notifyRemoved (const std::string& path)
    {
        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            impl->dropListenersAtOrUnder (path);
        }

        impl->notify ("PATH_REMOVED", juce::String (path));
    }

    //==============================================================================
    std::uint64_t OscQueryServer::refusedWrites() const  { return impl->refused.load (std::memory_order_relaxed); }
    std::uint64_t OscQueryServer::acceptedWrites() const { return impl->accepted.load (std::memory_order_relaxed); }

    int OscQueryServer::connectionCount() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return static_cast<int> (impl->connections.size());
    }

    int OscQueryServer::listenerCount (const std::string& path) const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        const auto found = impl->listeners.find (path);
        return found == impl->listeners.end() ? 0 : static_cast<int> (found->second.size());
    }
}
