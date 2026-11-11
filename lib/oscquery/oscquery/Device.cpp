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
    Where this comes from. The LISTEN/IGNORE command shape, the websocket
    started as "host:port" with no ws:// prefix, and re-sending every
    subscription in connectionOpened follow WFS-DIY's
    Plugin/Source/Shared/OscQueryClient.cpp (read at 17f9ad7f, nothing copied:
    it has no licence header, never parses HOST_INFO and decodes one message
    only). The frames-to-a-queue thread model is this file's own.
*/

#include <oscquery/Device.h>

#include <oscquery/net/HttpGet.h>

#include <juce_core/juce_core.h>
#include <juce_simpleweb/juce_simpleweb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace oqa::oscquery
{
    using Clock = std::chrono::steady_clock;

    const char* Device::stateName (State state) noexcept
    {
        switch (state)
        {
            case State::Unprobed:     return "unprobed";
            case State::Unreachable:  return "unreachable";
            case State::Reachable:    return "reachable";
            case State::TreeLoaded:   return "tree loaded";
            case State::Listening:    return "listening";
            case State::Reconnecting: return "reconnecting";
            case State::Closed:       return "closed";
        }

        return "?";
    }

    //==============================================================================
    namespace
    {
        /*  What the websocket thread hands the worker. Tagged with the
            generation of the websocket object it came from, so a close from a
            connection already replaced is not read as the new one failing. */
        struct Event
        {
            enum class Kind { opened, closed, text, binary, listenNow, ignoreNow, sendBinary };

            Kind kind = Kind::closed;
            std::uint64_t generation = 0;
            std::string text;
            std::vector<std::uint8_t> bytes;
        };

        std::string commandJson (const char* command, const std::string& path)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("COMMAND", command);
            object->setProperty ("DATA", juce::String (path));
            return juce::JSON::toString (juce::var (object), true).toStdString();
        }

        bool isAtOrUnder (const std::string& path, const std::string& prefix)
        {
            return path == prefix || (prefix != "/" && path.rfind (prefix + "/", 0) == 0)
                || (prefix == "/" && ! path.empty());
        }
    }

    //==============================================================================
    struct Device::Impl final : private SimpleWebSocketClientBase::Listener
    {
        explicit Impl (Options o) : options (std::move (o)) {}

        ~Impl() override
        {
            stop();
        }

        const Options options;
        Callbacks callbacks;

        //  Everything a snapshot reads, behind one lock.
        mutable std::mutex lock;
        State state = State::Unprobed;
        std::optional<HostInfo> info;
        Tree tree;
        bool treeLoaded = false;
        bool large = false;
        std::set<std::string> subscriptions;
        std::string lastError;
        Stats stats;
        std::atomic<std::uint64_t> revision { 0 };

        //  The worker and its queue.
        std::thread worker;
        std::atomic<bool> started { false };
        std::mutex queueLock;
        std::condition_variable wake;
        std::deque<Event> events;
        bool stopping = false;

        //  The websocket; touched on the worker thread only.
        std::unique_ptr<SimpleWebSocketClient> ws;
        std::atomic<std::uint64_t> generation { 0 };
        std::atomic<bool> wsOpen { false };
        bool wsEverOpened = false;

        /*  The paths LISTENed, and read once, since this open: a subscription
            made as the websocket opens is in the set onOpened sends and in a
            listenNow event behind it, and must not be read twice (found by the
            plugin's Phase 5b, which LISTENs once the device is confirmed,
            close to the open). Worker thread only. */
        std::set<std::string> onWire;

        //  Backoff; worker thread only.
        int backoffMs = 500;
        bool attemptPending = false;
        Clock::time_point nextAttempt;

        //==============================================================================
        void bump() noexcept { revision.fetch_add (1, std::memory_order_release); }

        void setState (State next)
        {
            std::function<void (State)> onState;

            {
                const std::lock_guard<std::mutex> guard (lock);

                if (state == next)
                    return;

                state = next;
                bump();
                onState = callbacks.onState;
            }

            if (onState)
                onState (next);
        }

        void fail (const std::string& why)
        {
            {
                const std::lock_guard<std::mutex> guard (lock);
                lastError = why;
                bump();
            }

            if (callbacks.onError)
                callbacks.onError (why);
        }

        //==============================================================================
        //  Blocking HTTP, any thread.
        bool probe()
        {
            const auto reply = net::httpGet (options.host, options.port, "/", "HOST_INFO", options.httpTimeoutMs);

            if (! reply.ok || reply.status != 200)
            {
                {
                    const std::lock_guard<std::mutex> guard (lock);
                    lastError = reply.ok ? "HOST_INFO answered " + std::to_string (reply.status)
                                         : reply.error;
                    bump();
                }

                setState (State::Unreachable);
                return false;
            }

            std::string error;
            auto parsed = parseHostInfo (reply.body, error);

            if (! parsed.has_value())
            {
                {
                    const std::lock_guard<std::mutex> guard (lock);
                    lastError = error;
                    bump();
                }

                setState (State::Unreachable);
                return false;
            }

            State next;

            {
                const std::lock_guard<std::mutex> guard (lock);
                info = std::move (*parsed);
                bump();

                next = (state == State::Unprobed || state == State::Unreachable) ? State::Reachable : state;
            }

            setState (next);
            return true;
        }

        bool fetchTree()
        {
            const auto reply = net::httpGet (options.host, options.port, "/", "", options.httpTimeoutMs);

            if (! reply.ok || reply.status != 200)
            {
                fail (reply.ok ? "GET / answered " + std::to_string (reply.status) : reply.error);
                return false;
            }

            std::string error;
            auto parsed = Tree::parse (reply.body, error);

            if (! parsed.has_value())
            {
                fail (error);
                return false;
            }

            State next;

            {
                const std::lock_guard<std::mutex> guard (lock);
                tree = std::move (*parsed);
                treeLoaded = true;
                large = tree.nodeCount() > options.largeTreeThreshold;
                bump();

                next = state == State::Reachable ? State::TreeLoaded : state;
            }

            setState (next);
            return true;
        }

        /*  GET of one subtree, merged in. 404 means the node is gone, which
            is reported as such rather than as a failure to read. */
        enum class Subtree { merged, gone, failed };

        Subtree fetchSubtree (const std::string& path)
        {
            const auto reply = net::httpGet (options.host, options.port, path, "", options.httpTimeoutMs);

            if (reply.ok && reply.status == 404)
                return Subtree::gone;

            if (! reply.ok || reply.status != 200)
            {
                fail (reply.ok ? "GET " + path + " answered " + std::to_string (reply.status) : reply.error);
                return Subtree::failed;
            }

            juce::var parsed;

            if (juce::JSON::parse (juce::String (reply.body), parsed).failed())
            {
                fail ("GET " + path + " did not answer JSON");
                return Subtree::failed;
            }

            std::string error;

            {
                const std::lock_guard<std::mutex> guard (lock);

                if (! tree.merge (parsed, path, error))
                {
                    lastError = error;
                    bump();
                    return Subtree::failed;
                }

                bump();
            }

            return Subtree::merged;
        }

        std::optional<osc::Values> readValue (const std::string& path)
        {
            const auto reply = net::httpGet (options.host, options.port, path, "VALUE", options.httpTimeoutMs);

            if (! reply.ok || reply.status != 200)
                return std::nullopt;

            juce::var parsed;

            if (juce::JSON::parse (juce::String (reply.body), parsed).failed() || ! parsed.hasProperty ("VALUE"))
                return std::nullopt;

            std::string type;

            {
                const std::lock_guard<std::mutex> guard (lock);

                if (const auto* node = tree.find (path))
                    type = node->type.value_or (std::string());
            }

            return valuesFromVar (parsed["VALUE"], type);
        }

        //==============================================================================
        //  The websocket's thread: nothing but enqueueing.
        void push (Event event)
        {
            {
                const std::lock_guard<std::mutex> guard (queueLock);
                events.push_back (std::move (event));
            }

            wake.notify_one();
        }

        Event tagged (Event::Kind kind, std::string text = {})
        {
            Event event;
            event.kind = kind;
            event.generation = generation.load();
            event.text = std::move (text);
            return event;
        }

        void connectionOpened() override                              { push (tagged (Event::Kind::opened)); }
        void connectionClosed (int, const juce::String&) override     { push (tagged (Event::Kind::closed)); }
        void connectionError (int, const juce::String&) override      { push (tagged (Event::Kind::closed)); }

        void messageReceived (const juce::String& message) override
        {
            auto event = tagged (Event::Kind::text);
            event.text = message.toStdString();
            push (std::move (event));
        }

        void dataReceived (const juce::MemoryBlock& data) override
        {
            auto event = tagged (Event::Kind::binary);
            const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
            event.bytes.assign (bytes, bytes + data.getSize());
            push (std::move (event));
        }

        //==============================================================================
        //  The worker.
        void start()
        {
            if (started.exchange (true))
                return;

            {
                const std::lock_guard<std::mutex> guard (queueLock);
                stopping = false;
                events.clear();
            }

            backoffMs = std::max (1, options.reconnectInitialMs);
            attemptPending = true;
            nextAttempt = Clock::now();

            worker = std::thread ([this] { run(); });
        }

        void stop()
        {
            if (! started.exchange (false))
                return;

            {
                const std::lock_guard<std::mutex> guard (queueLock);
                stopping = true;
            }

            wake.notify_all();

            if (worker.joinable())
                worker.join();

            closeWebsocket();
            setState (State::Closed);
        }

        void run()
        {
            for (;;)
            {
                std::deque<Event> batch;
                bool attemptNow = false;

                {
                    std::unique_lock<std::mutex> guard (queueLock);

                    const auto ready = [this]
                    {
                        return stopping || ! events.empty()
                            || (attemptPending && Clock::now() >= nextAttempt);
                    };

                    if (attemptPending)
                        wake.wait_until (guard, nextAttempt, ready);
                    else
                        wake.wait (guard, ready);

                    if (stopping)
                        return;

                    batch.swap (events);
                    attemptNow = attemptPending && Clock::now() >= nextAttempt;
                }

                for (auto& event : batch)
                    handle (event);

                if (attemptNow)
                {
                    attemptPending = false;
                    attempt();
                }
            }
        }

        void scheduleRetry()
        {
            nextAttempt = Clock::now() + std::chrono::milliseconds (backoffMs);
            backoffMs = std::min (backoffMs * 2, std::max (backoffMs, options.reconnectMaxMs));
            attemptPending = true;
        }

        /*  One go at getting to Listening (or as far as the device allows):
            probe if there is no answer yet or the connection broke, fetch the
            tree if there is none (or on reconnect, unless it is large), then
            open the websocket when the device offers one. */
        void attempt()
        {
            State current;
            bool haveTree;
            bool isLarge;

            {
                const std::lock_guard<std::mutex> guard (lock);
                current = state;
                haveTree = treeLoaded;
                isLarge = large;
            }

            /*  Back from a broken connection, or from a device that did not
                answer at all for a while: either way it may have restarted,
                and the tree held from before is not known to be what it
                serves now. Only the first was read again; a server down for
                a second, long enough for one probe to fail, came back with
                its old tree, a changed ACCESS unseen (Julien Bayle's retest
                of ec6725c, 2.7). */
            const bool reconnecting = current == State::Reconnecting
                                      || (current == State::Unreachable && haveTree);

            if (current == State::Unprobed || current == State::Unreachable || reconnecting)
            {
                if (! probe())
                {
                    scheduleRetry();
                    return;
                }
            }

            const bool refetch = reconnecting && options.refetchTreeOnReconnect && ! isLarge;

            if (! haveTree || refetch)
            {
                std::string hashBefore;

                if (refetch)
                {
                    const std::lock_guard<std::mutex> guard (lock);
                    hashBefore = tree.treeHash();
                }

                if (fetchTree() && refetch && callbacks.onTreeChanged)
                {
                    std::string hashAfter;

                    {
                        const std::lock_guard<std::mutex> guard (lock);
                        hashAfter = tree.treeHash();
                    }

                    if (hashAfter != hashBefore)
                        callbacks.onTreeChanged ("/");
                }
            }

            bool offered;
            std::string host;
            int port;

            {
                const std::lock_guard<std::mutex> guard (lock);
                offered = info.has_value() && info->offersWebsocket();
                host = info.has_value() ? info->websocketHost (options.host) : options.host;
                port = info.has_value() ? info->websocketPort (options.port) : options.port;
            }

            if (offered && options.openWebsocket)
            {
                openWebsocket (host, port);
            }
            else
            {
                State next;

                {
                    const std::lock_guard<std::mutex> guard (lock);
                    next = treeLoaded ? State::TreeLoaded : State::Reachable;
                }

                setState (next);
                backoffMs = std::max (1, options.reconnectInitialMs);
            }
        }

        void openWebsocket (const std::string& host, int port)
        {
            closeWebsocket();

            ++generation;
            ws = std::make_unique<SimpleWebSocketClient>();
            ws->addWebSocketListener (this);

            /*  The connect timeout is this client's, in seconds, with a floor of
                one; juce_simpleweb's default is 1000 s, which is no timeout. */
            const auto seconds = std::max (1, (options.httpTimeoutMs + 999) / 1000);
            ws->start (juce::String (host) + ":" + juce::String (port), seconds);
        }

        void closeWebsocket()
        {
            if (ws == nullptr)
                return;

            ws->removeWebSocketListener (this);
            ws->stop();
            ws.reset();
            wsOpen = false;
            onWire.clear();
        }

        //==============================================================================
        void handle (Event& event)
        {
            if (event.generation != generation)
                return;                             // from a websocket already replaced

            switch (event.kind)
            {
                case Event::Kind::opened:     onOpened(); break;
                case Event::Kind::closed:     onClosed(); break;
                case Event::Kind::text:       onText (event.text); break;
                case Event::Kind::binary:     onBinary (event.bytes); break;
                case Event::Kind::listenNow:  listenNow (event.text); break;
                case Event::Kind::ignoreNow:  ignoreNow (event.text); break;
                case Event::Kind::sendBinary: sendBinaryNow (event.bytes); break;
            }
        }

        /*  A subscription made while the websocket is open: LISTEN now, and
            the current value over HTTP, for the same reason as in onOpened. A
            subscription made before the open waits for onOpened to do both. */
        void listenNow (const std::string& path)
        {
            if (! wsOpen || ! onWire.insert (path).second)
                return;

            sendText (commandJson ("LISTEN", path));

            if (auto values = readValue (path))
                deliver (path, *values);
        }

        void ignoreNow (const std::string& path)
        {
            onWire.erase (path);
            sendText (commandJson ("IGNORE", path));
        }

        void onOpened()
        {
            wsOpen = true;
            backoffMs = std::max (1, options.reconnectInitialMs);

            std::set<std::string> paths;

            {
                const std::lock_guard<std::mutex> guard (lock);
                paths = subscriptions;

                if (wsEverOpened)
                    ++stats.reconnects;

                wsEverOpened = true;
                bump();
            }

            setState (State::Listening);

            /*  Every subscription again, then its value over HTTP: a device
                need not push the current value on LISTEN, and WFS-DIY does not,
                so the first value a lane sees would otherwise be the first
                change after the connection rather than where the fader is. */
            onWire = paths;

            for (const auto& path : paths)
                sendText (commandJson ("LISTEN", path));

            for (const auto& path : paths)
                if (auto values = readValue (path))
                    deliver (path, *values);
        }

        void onClosed()
        {
            wsOpen = false;
            onWire.clear();

            State current;

            {
                const std::lock_guard<std::mutex> guard (lock);
                current = state;
            }

            if (current == State::Closed)
                return;

            setState (State::Reconnecting);
            scheduleRetry();
        }

        void sendText (const std::string& text)
        {
            if (ws != nullptr && wsOpen)
                ws->send (juce::String (text));
        }

        void sendBinaryNow (const std::vector<std::uint8_t>& bytes)
        {
            if (ws != nullptr && wsOpen && ! bytes.empty())
                ws->send (reinterpret_cast<const char*> (bytes.data()), static_cast<int> (bytes.size()));
        }

        //==============================================================================
        void deliver (const std::string& path, const osc::Values& values)
        {
            {
                const std::lock_guard<std::mutex> guard (lock);
                ++stats.valuesReceived;

                if (auto* node = tree.find (path))
                {
                    node->hasValue = true;
                    node->value = values;
                }

                bump();
            }

            if (callbacks.onValue)
                callbacks.onValue (path, values);
        }

        void deliverPacket (const osc::Packet& packet)
        {
            if (packet.isBundle())
            {
                for (const auto& element : packet.elements)
                    deliverPacket (element);

                return;
            }

            deliver (packet.address, packet.args);
        }

        void onBinary (const std::vector<std::uint8_t>& bytes)
        {
            const auto result = osc::decode (bytes.data(), bytes.size());

            if (! result.ok)
            {
                {
                    const std::lock_guard<std::mutex> guard (lock);
                    ++stats.framesRefused;
                }

                fail ("a websocket frame was not OSC: " + result.error);
                return;
            }

            deliverPacket (result.packet);
        }

        /*  The device stopped streaming these (PATH_REMOVED, PATH_RENAMED): out
            of the subscriptions, and off the wire's record, so a LISTEN on the
            path once it is back goes out again. Worker thread (onText). */
        void dropSubscriptionsUnder (const std::string& path)
        {
            for (auto it = subscriptions.begin(); it != subscriptions.end();)
                it = isAtOrUnder (*it, path) ? subscriptions.erase (it) : std::next (it);

            for (auto it = onWire.begin(); it != onWire.end();)
                it = isAtOrUnder (*it, path) ? onWire.erase (it) : std::next (it);
        }

        void onText (const std::string& text)
        {
            const auto parsed = juce::JSON::parse (juce::String (text));
            const auto command = parsed.getProperty ("COMMAND", {}).toString().toStdString();
            const auto data = parsed.getProperty ("DATA", {});

            if (command == "PATH_CHANGED" || command == "PATH_ADDED")
            {
                if (! data.isString())
                    return;

                const auto path = data.toString().toStdString();
                countNotification();

                switch (fetchSubtree (path))
                {
                    case Subtree::merged:
                        if (callbacks.onTreeChanged)
                            callbacks.onTreeChanged (path);
                        break;

                    case Subtree::gone:
                        removePath (path);
                        break;

                    case Subtree::failed:
                        break;
                }
            }
            else if (command == "PATH_REMOVED")
            {
                if (! data.isString())
                    return;

                countNotification();
                removePath (data.toString().toStdString());
            }
            else if (command == "PATH_RENAMED")
            {
                const auto oldPath = data.getProperty ("OLD", {}).toString().toStdString();
                const auto newPath = data.getProperty ("NEW", {}).toString().toStdString();

                if (oldPath.empty() || newPath.empty())
                    return;

                countNotification();

                bool renamed;

                {
                    const std::lock_guard<std::mutex> guard (lock);
                    renamed = tree.rename (oldPath, newPath);
                    dropSubscriptionsUnder (oldPath);       // the device stopped streaming them
                    bump();
                }

                //  Not in the tree under the old name: read it under the new one.
                if (! renamed)
                    fetchSubtree (newPath);

                if (callbacks.onPathRenamed)
                    callbacks.onPathRenamed (oldPath, newPath);
            }
            else
            {
                const std::lock_guard<std::mutex> guard (lock);
                ++stats.unknownCommands;
                bump();
            }
        }

        void countNotification()
        {
            const std::lock_guard<std::mutex> guard (lock);
            ++stats.notifications;
            bump();
        }

        void removePath (const std::string& path)
        {
            {
                const std::lock_guard<std::mutex> guard (lock);
                tree.remove (path);
                dropSubscriptionsUnder (path);
                bump();
            }

            if (callbacks.onPathRemoved)
                callbacks.onPathRemoved (path);
        }
    };

    //==============================================================================
    Device::Device (Options options) : impl (std::make_unique<Impl> (std::move (options))) {}
    Device::~Device() = default;

    void Device::setCallbacks (Callbacks callbacks)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->callbacks = std::move (callbacks);
    }

    const Device::Options& Device::options() const noexcept { return impl->options; }

    bool Device::probe()                                        { return impl->probe(); }
    bool Device::fetchTree()                                    { return impl->fetchTree(); }
    bool Device::fetchSubtree (const std::string& path)         { return impl->fetchSubtree (path) == Impl::Subtree::merged; }
    std::optional<osc::Values> Device::readValue (const std::string& path) { return impl->readValue (path); }

    void Device::start()                    { impl->start(); }
    void Device::stop()                     { impl->stop(); }
    bool Device::isStarted() const noexcept { return impl->started.load(); }

    Device::Listen Device::listen (const std::string& path)
    {
        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (impl->info.has_value() && ! impl->info->has ("LISTEN"))
                return Listen::unsupported;

            impl->subscriptions.insert (path);
            impl->bump();
        }

        //  The worker owns the websocket, so the send goes through its queue.
        impl->push (impl->tagged (Event::Kind::listenNow, path));
        return Listen::accepted;
    }

    void Device::ignore (const std::string& path)
    {
        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (impl->subscriptions.erase (path) == 0)
                return;

            impl->bump();
        }

        impl->push (impl->tagged (Event::Kind::ignoreNow, path));
    }

    std::set<std::string> Device::subscriptions() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->subscriptions;
    }

    bool Device::sendOsc (const osc::Packet& packet, std::string& error)
    {
        auto encoded = osc::encode (packet, error);

        if (! encoded.has_value())
            return false;

        if (! impl->wsOpen.load())
        {
            error = "the websocket is not open";
            return false;
        }

        auto event = impl->tagged (Event::Kind::sendBinary);
        event.bytes = std::move (*encoded);
        impl->push (std::move (event));
        return true;
    }

    Device::State Device::state() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->state;
    }

    std::optional<HostInfo> Device::hostInfo() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->info;
    }

    bool Device::treeLoaded() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->treeLoaded;
    }

    bool Device::treeIsLarge() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->large;
    }

    Tree Device::tree() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->tree;
    }

    void Device::withTree (const std::function<void (const Tree&)>& read) const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        read (impl->tree);
    }

    std::string Device::lastError() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->lastError;
    }

    Device::Stats Device::stats() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->stats;
    }

    std::uint64_t Device::revision() const noexcept
    {
        return impl->revision.load (std::memory_order_acquire);
    }
}
