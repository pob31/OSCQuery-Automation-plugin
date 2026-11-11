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

#include "MockDevice.h"

#include <chrono>

/*
    Where this comes from. The server shell (one juce_simpleweb server for HTTP
    and WebSocket, start(0) then wait for the granted port, answering on the
    server's thread, one binary frame per listener) follows go.dot's
    src/wfg/engine/oscquery/OscQueryServer.cpp at 1c8aaa0a. The walk down
    CONTENTS follows WFS-DIY's Source/Network/OSCQueryServer.cpp, and the OSC
    encoding its pushValueChange. Nothing is taken from any other OSCQuery
    implementation.
*/

#include <juce_simpleweb/juce_simpleweb.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace oqatest
{
    namespace
    {
        // Every attribute the proposal defines. Anything else is a 400.
        const juce::StringArray proposalAttributes { "FULL_PATH", "CONTENTS", "TYPE", "VALUE",
                                                     "RANGE", "ACCESS", "DESCRIPTION", "TAGS",
                                                     "EXTENDED_TYPE", "UNIT", "CRITICAL",
                                                     "CLIPMODE", "OVERLOADS", "HTML" };

        juce::String normalisePath (juce::String path)
        {
            path = juce::URL::removeEscapeChars (path);

            if (! path.startsWithChar ('/'))
                path = "/" + path;

            while (path.length() > 1 && path.endsWithChar ('/'))
                path = path.dropLastCharacters (1);

            return path;
        }

        juce::StringArray segmentsOf (const juce::String& path)
        {
            juce::StringArray segments;
            segments.addTokens (path, "/", "");
            segments.removeEmptyStrings();
            return segments;
        }

        juce::String parentOf (const juce::String& path)
        {
            const auto parent = path.upToLastOccurrenceOf ("/", false, false);
            return parent.isEmpty() ? juce::String ("/") : parent;
        }

        juce::String lastSegmentOf (const juce::String& path)
        {
            return path.fromLastOccurrenceOf ("/", false, false);
        }

        juce::String childPath (const juce::String& parent, const juce::String& name)
        {
            return parent == "/" ? "/" + name : parent + "/" + name;
        }

        bool isAtOrUnder (const juce::String& path, const juce::String& prefix)
        {
            return path == prefix || path.startsWith (prefix == "/" ? prefix : prefix + "/");
        }

        // The node at `path`, sharing its objects with `root`, or void.
        juce::var findIn (const juce::var& root, const juce::String& path)
        {
            auto node = root;

            for (const auto& segment : segmentsOf (path))
            {
                const auto contents = node.getProperty ("CONTENTS", {});
                auto* object = contents.getDynamicObject();

                if (object == nullptr || ! object->hasProperty (segment))
                    return {};

                node = object->getProperty (segment);
            }

            return node.isObject() ? node : juce::var();
        }

        // The CONTENTS object of a container node, created if it has none yet.
        juce::DynamicObject* contentsOf (const juce::var& node)
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return nullptr;

            if (! object->getProperty ("CONTENTS").isObject())
                object->setProperty ("CONTENTS", juce::var (new juce::DynamicObject()));

            return object->getProperty ("CONTENTS").getDynamicObject();
        }

        void rewriteFullPaths (const juce::var& node, const juce::String& path)
        {
            auto* object = node.getDynamicObject();

            if (object == nullptr)
                return;

            object->setProperty ("FULL_PATH", path);

            if (auto* contents = object->getProperty ("CONTENTS").getDynamicObject())
                for (auto& child : contents->getProperties())
                    rewriteFullPaths (child.value, childPath (path, child.name.toString()));
        }

        juce::String command (const juce::String& name, const juce::var& data)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("COMMAND", name);
            object->setProperty ("DATA", data);
            return juce::JSON::toString (juce::var (object), true);
        }

        /*  One OSC message, big-endian, every field padded to four bytes. T and F
            nodes take the tag from the value, as a toggle does on the wire. */
        juce::MemoryBlock encodeOsc (const juce::String& address, const juce::String& type,
                                     const juce::Array<juce::var>& values)
        {
            juce::MemoryOutputStream out;

            const auto writePadded = [&out] (const juce::String& text)
            {
                const auto utf8 = text.toStdString();
                out.write (utf8.c_str(), utf8.size() + 1);

                while (out.getDataSize() % 4 != 0)
                    out.writeByte (0);
            };

            writePadded (address);

            juce::String tags (",");

            for (int i = 0; i < type.length(); ++i)
            {
                const auto tag = type[i];
                tags << ((tag == 'T' || tag == 'F') ? (static_cast<bool> (values[i]) ? "T" : "F")
                                                    : juce::String::charToString (tag));
            }

            writePadded (tags);

            for (int i = 0; i < type.length(); ++i)
            {
                switch (type[i])
                {
                    case 'f':
                    {
                        const auto value = static_cast<float> (static_cast<double> (values[i]));
                        std::uint32_t bits = 0;
                        std::memcpy (&bits, &value, sizeof (bits));
                        out.writeIntBigEndian (static_cast<int> (bits));
                        break;
                    }
                    case 'i':
                        out.writeIntBigEndian (static_cast<int> (values[i]));
                        break;
                    case 's':
                        writePadded (values[i].toString());
                        break;
                    default:
                        break;   // T, F and N carry no argument bytes
                }
            }

            return out.getMemoryBlock();
        }

        /*  One OSC bundle, time tag 1 ("immediately"), each element its size
            then its bytes, as OSC 1.0 lays it out. */
        juce::MemoryBlock encodeOscBundle (const std::vector<juce::MemoryBlock>& messages)
        {
            juce::MemoryOutputStream out;
            out.write ("#bundle", 8);                    // seven characters and the terminator
            out.writeInt64BigEndian (1);

            for (const auto& message : messages)
            {
                out.writeIntBigEndian (static_cast<int> (message.getSize()));
                out.write (message.getData(), message.getSize());
            }

            return out.getMemoryBlock();
        }
    }

    //==============================================================================
    struct MockDevice::Impl final : public SimpleWebSocketServerBase::RequestHandler,
                                    public SimpleWebSocketServerBase::Listener
    {
        explicit Impl (Options o) : options (std::move (o)) {}

        Options options;

        std::unique_ptr<SimpleWebSocketServer> server;   // one per start(), so a restart is clean
        std::unique_ptr<juce::DatagramSocket> osc;       // OSC_PORT over UDP, read by oscThread
        std::unique_ptr<juce::StreamingSocket> oscListener;   // OSC_PORT over TCP, accepted by oscThread
        std::thread oscThread;
        std::atomic<bool> oscStopping { false };
        std::vector<std::vector<std::uint8_t>> oscDatagrams;  // under lock
        std::vector<std::chrono::steady_clock::time_point> oscTimes;   // when each datagram was read
        std::vector<std::uint8_t> oscStream;                  // under lock

        mutable std::mutex lock;                          // the tree, listeners, connections
        juce::var root;
        std::map<juce::String, std::set<juce::String>> listeners;   // path -> connection ids
        std::set<juce::String> connections;
        std::vector<std::vector<std::uint8_t>> wsOsc;                // binary frames received

        bool advertises (const char* extension) const
        {
            return options.extensions.contains (extension);
        }

        int boundHttpPort() const
        {
            return server != nullptr ? server->port : 0;
        }

        bool oscOverTcp() const { return options.oscTransport.equalsIgnoreCase ("TCP"); }

        int boundOscPort() const
        {
            if (oscOverTcp())
                return oscListener != nullptr ? oscListener->getBoundPort() : 0;

            return osc != nullptr ? osc->getBoundPort() : 0;
        }

        /*  Reads OSC_PORT on its own thread: datagrams whole, a TCP connection's
            bytes as they come (one connection at a time; a test opens one). */
        void readOsc()
        {
            std::vector<std::uint8_t> buffer (65536);

            while (! oscStopping.load())
            {
                if (! oscOverTcp())
                {
                    if (osc->waitUntilReady (true, 50) != 1)
                        continue;

                    const auto got = osc->read (buffer.data(), static_cast<int> (buffer.size()), false);

                    if (got <= 0)
                        continue;

                    const auto readAt = std::chrono::steady_clock::now();

                    {
                        const std::lock_guard<std::mutex> guard (lock);
                        oscDatagrams.emplace_back (buffer.begin(), buffer.begin() + got);
                        oscTimes.push_back (readAt);
                    }

                    if (options.echoDelayMs >= 0)
                        scheduleEcho (decodeForEcho (buffer.data(), static_cast<std::size_t> (got)));

                    continue;
                }

                if (oscListener->waitUntilReady (true, 50) != 1)
                    continue;

                std::unique_ptr<juce::StreamingSocket> connection (oscListener->waitForNextConnection());

                if (connection == nullptr)
                    continue;

                while (! oscStopping.load())
                {
                    const auto ready = connection->waitUntilReady (true, 50);

                    if (ready < 0)
                        break;

                    if (ready == 0)
                        continue;

                    const auto got = connection->read (buffer.data(), static_cast<int> (buffer.size()), false);

                    if (got <= 0)
                        break;

                    const std::lock_guard<std::mutex> guard (lock);
                    oscStream.insert (oscStream.end(), buffer.begin(), buffer.begin() + got);
                }
            }
        }

        //==============================================================================
        //  The echo (Options::echoDelayMs): what arrives over UDP is applied and
        //  streamed back after the delay, on a thread of its own per datagram,
        //  joined at stop. A decoder for the four types a slot binds, enough for
        //  the loop guard's test; anything else in a message drops the message.
        using EchoValues = std::vector<std::pair<juce::String, juce::Array<juce::var>>>;
        std::vector<std::thread> echoThreads;        // under lock
        std::function<void (const EchoValues&)> applyEcho;

        static EchoValues decodeForEcho (const std::uint8_t* data, std::size_t size)
        {
            EchoValues out;

            const auto readPadded = [&] (std::size_t& pos, juce::String& text) -> bool
            {
                const auto* begin = data + pos;
                const auto* end = static_cast<const std::uint8_t*> (std::memchr (begin, 0, size - pos));

                if (end == nullptr)
                    return false;

                text = juce::String::fromUTF8 (reinterpret_cast<const char*> (begin), static_cast<int> (end - begin));
                pos += static_cast<std::size_t> (end - begin) + 1;
                pos = (pos + 3) & ~std::size_t (3);
                return pos <= size;
            };

            const auto readInt = [&] (std::size_t& pos, std::int32_t& value) -> bool
            {
                if (pos + 4 > size)
                    return false;

                value = static_cast<std::int32_t> ((std::uint32_t (data[pos]) << 24) | (std::uint32_t (data[pos + 1]) << 16)
                                                 | (std::uint32_t (data[pos + 2]) << 8) | std::uint32_t (data[pos + 3]));
                pos += 4;
                return true;
            };

            if (size >= 16 && std::memcmp (data, "#bundle", 8) == 0)
            {
                std::size_t pos = 16;

                while (pos + 4 <= size)
                {
                    std::int32_t length = 0;
                    readInt (pos, length);

                    if (length <= 0 || pos + static_cast<std::size_t> (length) > size)
                        break;

                    const auto inner = decodeForEcho (data + pos, static_cast<std::size_t> (length));
                    out.insert (out.end(), inner.begin(), inner.end());
                    pos += static_cast<std::size_t> (length);
                }

                return out;
            }

            std::size_t pos = 0;
            juce::String address, tags;

            if (! readPadded (pos, address) || ! readPadded (pos, tags) || ! tags.startsWith (","))
                return out;

            juce::Array<juce::var> values;

            for (int i = 1; i < tags.length(); ++i)
            {
                switch (tags[i])
                {
                    case 'f':
                    {
                        std::int32_t bits = 0;

                        if (! readInt (pos, bits))
                            return out;

                        float value = 0.0f;
                        std::memcpy (&value, &bits, sizeof (value));
                        values.add (static_cast<double> (value));
                        break;
                    }
                    case 'i':
                    {
                        std::int32_t value = 0;

                        if (! readInt (pos, value))
                            return out;

                        values.add (static_cast<int> (value));
                        break;
                    }
                    case 'T': values.add (true); break;
                    case 'F': values.add (false); break;
                    default: return out;
                }
            }

            out.emplace_back (address, std::move (values));
            return out;
        }

        void scheduleEcho (EchoValues values)
        {
            if (values.empty() || ! applyEcho)
                return;

            const std::lock_guard<std::mutex> guard (lock);
            echoThreads.emplace_back ([this, values = std::move (values)]
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (options.echoDelayMs);

                while (! oscStopping.load() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));

                if (! oscStopping.load())
                    applyEcho (values);
            });
        }

        void joinEchoes()
        {
            std::vector<std::thread> threads;

            {
                const std::lock_guard<std::mutex> guard (lock);
                threads.swap (echoThreads);
            }

            for (auto& t : threads)
                if (t.joinable())
                    t.join();
        }

        //==============================================================================
        juce::var hostInfo() const
        {
            auto* info = new juce::DynamicObject();
            info->setProperty ("NAME", options.name);
            info->setProperty ("OSC_PORT", boundOscPort());
            info->setProperty ("OSC_TRANSPORT", oscOverTcp() ? "TCP" : "UDP");

            if (options.oscIp.isNotEmpty())
                info->setProperty ("OSC_IP", options.oscIp);

            if (options.websocket)
                info->setProperty ("WS_PORT", boundHttpPort());

            auto* extensions = new juce::DynamicObject();

            for (const auto& name : options.extensions)
                extensions->setProperty (name, true);

            info->setProperty ("EXTENSIONS", juce::var (extensions));

            //  Whatever else the test wants said, verbatim; not declared for it.
            if (auto* extras = options.hostInfoExtras.getDynamicObject(); extras != nullptr && ! options.hostInfoExtras.isArray())
                for (const auto& property : extras->getProperties())
                    info->setProperty (property.name, property.value);

            return juce::var (info);
        }

        //==============================================================================
        //  HTTP, on the server's thread.

        /** `node` (a deep copy) with every leaf's ACCESS less its write bit, down the subtree. */
        static juce::var clampedToRead (juce::var node)
        {
            if (auto* object = node.getDynamicObject())
            {
                if (object->hasProperty ("TYPE"))
                    object->setProperty ("ACCESS", static_cast<int> (object->getProperty ("ACCESS")) & 1);
                else if (object->hasProperty ("ACCESS"))
                    object->setProperty ("ACCESS", static_cast<int> (object->getProperty ("ACCESS")) & 1);

                if (auto* contents = object->getProperty ("CONTENTS").getDynamicObject())
                    for (auto& child : contents->getProperties())
                        contents->setProperty (child.name, clampedToRead (child.value));
            }

            return node;
        }

        static SimpleWeb::StatusCode statusCodeFor (int status)
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

        static void reply (const std::shared_ptr<HttpServer::Response>& response, int status,
                           const juce::String& body)
        {
            const SimpleWeb::CaseInsensitiveMultimap fields {
                { "Content-Type", status == 200 ? "application/json" : "text/plain" }
            };
            response->write (statusCodeFor (status), body.toStdString(), fields);
        }

        bool handleHTTPRequest (std::shared_ptr<HttpServer::Response> response,
                                std::shared_ptr<HttpServer::Request> request) override
        {
            if (request->method != "GET")
            {
                reply (response, 405, "GET only\n");
                return true;
            }

            /*  The query is request->query_string, not part of request->path:
                Simple-Web-Server splits them at the `?` (go.dot found the hard
                way that searching the path for it finds nothing). */
            const auto path = normalisePath (juce::String (request->path));
            const auto query = juce::String (request->query_string);

            //  The proposal: "the path in the URL is functionally irrelevant".
            if (query == "HOST_INFO")
            {
                reply (response, 200, juce::JSON::toString (hostInfo(), true));
                return true;
            }

            if (options.hostInfoOnly)
            {
                reply (response, 404, "this device serves HOST_INFO only\n");
                return true;
            }

            int status = 200;
            juce::String body;

            {
                const std::lock_guard<std::mutex> guard (lock);
                auto node = findIn (root, path);

                /*  ACCESS as reported to this client (C6): a stranger to the
                    allow-list sees the leaves read-only, on a copy. */
                if (node.isObject() && options.allowList.has_value())
                {
                    /*  The server is dual-stack: an IPv4 client arrives as the
                        mapped IPv6 form (::ffff:a.b.c.d); the list is spelled IPv4. */
                    auto client = juce::String (request->remote_endpoint().address().to_string());

                    if (client.startsWithIgnoreCase ("::ffff:"))
                        client = client.fromFirstOccurrenceOf (":", false, false).fromLastOccurrenceOf (":", false, false);

                    if (! options.allowList->contains (client))
                        node = clampedToRead (node.clone());
                }

                if (! node.isObject())
                {
                    status = 404;
                    body = "no node at " + path + "\n";
                }
                else if (query.isEmpty())
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
                    //  VALUE of a node that may not be read: 204, as the proposal says.
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

            reply (response, status, body);
            return true;
        }

        //==============================================================================
        //  WebSocket, on the server's thread.
        void connectionOpened (const juce::String& id) override
        {
            if (! options.websocket)
            {
                server->closeConnection (id, 1000, "this device has no websocket");
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

        void dataReceived (const juce::String&, const juce::MemoryBlock& data) override
        {
            const std::lock_guard<std::mutex> guard (lock);
            const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
            wsOsc.emplace_back (bytes, bytes + data.getSize());
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

            if (name == "LISTEN" && findIn (root, path).isObject())
                listeners[path].insert (id);
            else if (name == "IGNORE")
                listeners[path].erase (id);
        }

        //==============================================================================
        //  Sending, from the test's thread.
        void sendBinaryTo (const juce::String& id, const juce::MemoryBlock& data)
        {
            if (server == nullptr)
                return;

            //  One lookup under the map's own lock, and a copy that keeps the
            //  connection alive while it is written to (go.dot's sendBinary).
            const auto connection = server->connectionMap[id];

            if (connection == nullptr)
                return;

            auto message = std::make_shared<WsServer::OutMessage>();
            message->write (static_cast<const char*> (data.getData()),
                            static_cast<std::streamsize> (data.getSize()));
            connection->send (message, nullptr, 130);   // 130: one binary frame
        }

        void broadcastText (const juce::String& text)
        {
            if (server == nullptr || ! options.websocket)
                return;

            //  Under the map's lock: juce_simpleweb's own send() walks the map
            //  without it, racing connections opening on the server's thread.
            const auto utf8 = text.toStdString();
            const juce::ScopedLock mapLock (server->connectionMap.getLock());

            for (decltype (server->connectionMap)::Iterator it (server->connectionMap); it.next();)
                it.getValue()->send (utf8);
        }

        void notify (const char* extension, const juce::var& data)
        {
            if (advertises (extension) || options.notifyUnlisted)
                broadcastText (command (extension, data));
        }

        void dropListenersAtOrUnder (const juce::String& path)
        {
            for (auto it = listeners.begin(); it != listeners.end();)
                it = isAtOrUnder (it->first, path) ? listeners.erase (it) : std::next (it);
        }

        //==============================================================================
        //  The feedback target (setFeedbackTarget): a UDP OSC copy of every value
        //  this device streams, sent from OSC_PORT's own socket.
        juce::String feedbackHost;          // under lock
        int feedbackPort = 0;               // under lock

        void sendFeedback (const juce::MemoryBlock& packet)
        {
            juce::String host;
            int port = 0;

            {
                const std::lock_guard<std::mutex> guard (lock);
                host = feedbackHost;
                port = feedbackPort;
            }

            if (port > 0 && osc != nullptr)
                (void) osc->write (host, port, packet.getData(), static_cast<int> (packet.getSize()));
        }
    };

    //==============================================================================
    MockDevice::MockDevice() : MockDevice (Options {}) {}
    MockDevice::MockDevice (Options options) : impl (std::make_unique<Impl> (std::move (options))) {}
    MockDevice::~MockDevice() { stop(); }

    bool MockDevice::loadTree (const juce::var& root)
    {
        if (! root.isObject())
            return false;

        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->root = root.clone();
        impl->listeners.clear();
        return true;
    }

    bool MockDevice::loadTreeFromFile (const juce::File& file)
    {
        juce::var parsed;

        if (! juce::JSON::parse (file.loadFileAsString(), parsed).wasOk())
            return false;

        return loadTree (parsed);
    }

    bool MockDevice::start()
    {
        if (impl->server != nullptr)
            return false;

        if (impl->oscOverTcp())
        {
            impl->oscListener = std::make_unique<juce::StreamingSocket>();

            if (! impl->oscListener->createListener (0))
            {
                impl->oscListener.reset();
                return false;
            }
        }
        else
        {
            impl->osc = std::make_unique<juce::DatagramSocket>();

            if (! impl->osc->bindToPort (0))
            {
                impl->osc.reset();
                return false;
            }
        }

        impl->oscStopping.store (false);
        impl->applyEcho = [this] (const Impl::EchoValues& values)
        {
            for (const auto& entry : values)
                (void) setValue (entry.first, entry.second);
        };
        impl->oscThread = std::thread ([this] { impl->readOsc(); });

        impl->server = std::make_unique<SimpleWebSocketServer>();
        impl->server->addHTTPRequestHandler (impl.get());
        impl->server->addWebSocketListener (impl.get());

        /*  SO_REUSEADDR on POSIX only (go.dot): a restarted server gets its port
            back past TIME_WAIT, while on Windows the same option would let two
            processes listen on one port. */
       #if defined (_WIN32)
        constexpr auto reuseAddress = false;
       #else
        constexpr auto reuseAddress = true;
       #endif

        impl->server->start (impl->options.httpPort, {}, {}, reuseAddress);

        /*  Wait for the bind AND the granted port: isConnected can be set before
            the callback that writes an ephemeral port has run (go.dot, macOS). */
        const auto ready = [this] { return impl->server->isConnected && impl->server->port != 0; };

        for (int i = 0; i < 2000 && ! ready(); ++i)
            juce::Thread::sleep (5);

        if (! ready())
        {
            stop();
            return false;
        }

        return true;
    }

    void MockDevice::stop()
    {
        if (impl->server != nullptr)
        {
            impl->server->removeHTTPRequestHandler (impl.get());
            impl->server->stop();
            impl->server->removeWebSocketListener (impl.get());
            impl->server.reset();
        }

        impl->oscStopping.store (true);

        if (impl->oscThread.joinable())
            impl->oscThread.join();

        impl->joinEchoes();

        impl->osc.reset();
        impl->oscListener.reset();

        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->listeners.clear();
        impl->connections.clear();
    }

    bool MockDevice::isRunning() const { return impl->server != nullptr; }
    int MockDevice::httpPort() const { return impl->boundHttpPort(); }
    int MockDevice::oscPort() const { return impl->boundOscPort(); }

    std::vector<std::vector<std::uint8_t>> MockDevice::receivedOsc() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->oscDatagrams;
    }

    std::vector<std::uint8_t> MockDevice::receivedOscStream() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->oscStream;
    }

    std::vector<std::chrono::steady_clock::time_point> MockDevice::receivedOscTimes() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->oscTimes;
    }

    void MockDevice::clearReceivedOsc()
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->oscDatagrams.clear();
        impl->oscTimes.clear();
        impl->oscStream.clear();
        impl->wsOsc.clear();
    }
    const MockDevice::Options& MockDevice::options() const { return impl->options; }
    juce::var MockDevice::hostInfo() const { return impl->hostInfo(); }

    juce::var MockDevice::node (const juce::String& path) const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return findIn (impl->root, normalisePath (path)).clone();
    }

    //==============================================================================
    bool MockDevice::setValueBundle (const std::vector<std::pair<juce::String, juce::Array<juce::var>>>& values)
    {
        std::vector<juce::MemoryBlock> messages;
        std::set<juce::String> ids;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            for (const auto& entry : values)
            {
                const auto path = normalisePath (entry.first);
                const auto node = findIn (impl->root, path);

                if (! node.isObject() || ! node.hasProperty ("TYPE"))
                    return false;

                node.getDynamicObject()->setProperty ("VALUE", juce::var (entry.second));
                messages.push_back (encodeOsc (path, node.getProperty ("TYPE", {}).toString(), entry.second));

                if (const auto it = impl->listeners.find (path); it != impl->listeners.end())
                    ids.insert (it->second.begin(), it->second.end());
            }
        }

        const auto bundle = encodeOscBundle (messages);

        for (const auto& id : ids)
            impl->sendBinaryTo (id, bundle);

        impl->sendFeedback (bundle);
        return true;
    }

    void MockDevice::setFeedbackTarget (const juce::String& host, int port)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->feedbackHost = host;
        impl->feedbackPort = port;
    }

    void MockDevice::setAllowList (std::optional<juce::StringArray> allowList)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->options.allowList = std::move (allowList);
    }

    void MockDevice::setName (const juce::String& name)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->options.name = name;
    }

    void MockDevice::setHostInfoExtras (const juce::var& extras)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->options.hostInfoExtras = extras;
    }

    void MockDevice::setExtensions (const juce::StringArray& extensions)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->options.extensions = extensions;
    }

    void MockDevice::closeAllConnections()
    {
        if (impl->server == nullptr)
            return;

        std::vector<std::shared_ptr<WsServer::Connection>> open;

        {
            const juce::ScopedLock mapLock (impl->server->connectionMap.getLock());

            for (decltype (impl->server->connectionMap)::Iterator it (impl->server->connectionMap); it.next();)
                open.push_back (it.getValue());
        }

        for (const auto& connection : open)
            connection->send_close (1000, "closed by the test");
    }

    std::vector<std::vector<std::uint8_t>> MockDevice::receivedWebsocketOsc() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->wsOsc;
    }

    bool MockDevice::setValue (const juce::String& rawPath, const juce::Array<juce::var>& values)
    {
        const auto path = normalisePath (rawPath);
        juce::String type;
        std::set<juce::String> ids;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            const auto node = findIn (impl->root, path);

            if (! node.isObject() || ! node.hasProperty ("TYPE"))
                return false;

            node.getDynamicObject()->setProperty ("VALUE", juce::var (values));
            type = node.getProperty ("TYPE", {}).toString();

            if (const auto it = impl->listeners.find (path); it != impl->listeners.end())
                ids = it->second;
        }

        const auto packet = encodeOsc (path, type, values);

        for (const auto& id : ids)
            impl->sendBinaryTo (id, packet);

        impl->sendFeedback (packet);
        return true;
    }

    bool MockDevice::rename (const juce::String& rawOld, const juce::String& rawNew)
    {
        const auto oldPath = normalisePath (rawOld);
        const auto newPath = normalisePath (rawNew);

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            const auto node = findIn (impl->root, oldPath);
            const auto oldParent = findIn (impl->root, parentOf (oldPath));
            const auto newParent = findIn (impl->root, parentOf (newPath));

            if (oldPath == "/" || ! node.isObject() || ! newParent.isObject()
                || findIn (impl->root, newPath).isObject() || isAtOrUnder (newPath, oldPath))
                return false;

            contentsOf (oldParent)->removeProperty (lastSegmentOf (oldPath));
            contentsOf (newParent)->setProperty (lastSegmentOf (newPath), node);
            rewriteFullPaths (node, newPath);

            //  The old path is gone; a client LISTENs again at the new one.
            impl->dropListenersAtOrUnder (oldPath);
        }

        auto* data = new juce::DynamicObject();
        data->setProperty ("OLD", oldPath);
        data->setProperty ("NEW", newPath);
        impl->notify ("PATH_RENAMED", juce::var (data));
        return true;
    }

    bool MockDevice::remove (const juce::String& rawPath)
    {
        const auto path = normalisePath (rawPath);

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            const auto parent = findIn (impl->root, parentOf (path));

            if (path == "/" || ! findIn (impl->root, path).isObject())
                return false;

            contentsOf (parent)->removeProperty (lastSegmentOf (path));
            impl->dropListenersAtOrUnder (path);
        }

        impl->notify ("PATH_REMOVED", path);
        return true;
    }

    bool MockDevice::retype (const juce::String& rawPath, const juce::String& newType)
    {
        const auto path = normalisePath (rawPath);

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            const auto node = findIn (impl->root, path);

            if (! node.isObject() || ! node.hasProperty ("TYPE"))
                return false;

            node.getDynamicObject()->setProperty ("TYPE", newType);
        }

        impl->notify ("PATH_CHANGED", path);
        return true;
    }

    bool MockDevice::add (const juce::String& rawPath, const juce::var& newNode)
    {
        const auto path = normalisePath (rawPath);

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            const auto parent = findIn (impl->root, parentOf (path));

            if (path == "/" || ! newNode.isObject() || ! parent.isObject()
                || findIn (impl->root, path).isObject())
                return false;

            const auto copy = newNode.clone();
            contentsOf (parent)->setProperty (lastSegmentOf (path), copy);
            rewriteFullPaths (copy, path);
        }

        impl->notify ("PATH_ADDED", path);
        return true;
    }

    int MockDevice::listenerCount (const juce::String& rawPath) const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        const auto it = impl->listeners.find (normalisePath (rawPath));
        return it == impl->listeners.end() ? 0 : static_cast<int> (it->second.size());
    }

    int MockDevice::connectionCount() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return static_cast<int> (impl->connections.size());
    }
}
