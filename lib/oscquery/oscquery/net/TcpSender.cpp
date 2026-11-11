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

#include <oscquery/net/TcpSender.h>
#include <oscquery/net/WriteNoSignal.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <mutex>

namespace oqa::oscquery::net
{
    struct TcpSender::Impl
    {
        explicit Impl (Options o) : options (std::move (o)) {}

        const Options options;

        std::mutex lock;                               // the socket
        std::unique_ptr<juce::StreamingSocket> socket;

        std::atomic<std::uint64_t> sent { 0 };
        std::atomic<std::uint64_t> failed { 0 };

        //  Under `lock`.
        bool connectLocked (std::string& error)
        {
            if (socket != nullptr && socket->isConnected())
                return true;

            if (options.host.empty() || options.port <= 0 || options.port > 65535)
            {
                error = "no usable address";
                return false;
            }

            auto fresh = std::make_unique<juce::StreamingSocket>();

            if (! fresh->connect (juce::String (options.host), options.port, options.connectTimeoutMs))
            {
                error = "could not connect to " + options.host + ":" + std::to_string (options.port);
                return false;
            }

            socket = std::move (fresh);
            return true;
        }

        void dropLocked()
        {
            if (socket != nullptr)
            {
                socket->close();
                socket.reset();
            }
        }
    };

    //==============================================================================
    TcpSender::TcpSender (Options options) : impl (std::make_unique<Impl> (std::move (options))) {}

    TcpSender::~TcpSender()
    {
        disconnect();
    }

    const TcpSender::Options& TcpSender::options() const noexcept { return impl->options; }

    bool TcpSender::connect (std::string& error)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->connectLocked (error);
    }

    void TcpSender::disconnect()
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->dropLocked();
    }

    bool TcpSender::isConnected() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->socket != nullptr && impl->socket->isConnected();
    }

    bool TcpSender::send (const std::vector<std::uint8_t>& packet, std::string& error)
    {
        if (packet.empty())
        {
            error = "nothing to send";
            return false;
        }

        const auto framed = osc::frame (impl->options.framing, packet);

        const std::lock_guard<std::mutex> guard (impl->lock);

        if (! impl->connectLocked (error))
        {
            impl->failed.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        //  Never juce's own write: a device that closed the connection would end the process (WriteNoSignal.h).
        const auto written = writeNoSignal (*impl->socket, framed.data(), static_cast<int> (framed.size()));

        if (written != static_cast<int> (framed.size()))
        {
            /*  Dropped, so the next send connects afresh: a half-written frame
                would leave the device's deframer mid-packet, and the only way
                to resynchronise a stream nobody reads back is to start one. */
            impl->dropLocked();
            impl->failed.fetch_add (1, std::memory_order_relaxed);
            error = "the packet could not be written to " + impl->options.host + ":" + std::to_string (impl->options.port);
            return false;
        }

        impl->sent.fetch_add (1, std::memory_order_relaxed);
        return true;
    }

    bool TcpSender::send (const osc::Packet& packet, std::string& error)
    {
        auto encoded = osc::encode (packet, error);

        if (! encoded.has_value())
        {
            impl->failed.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        return send (*encoded, error);
    }

    std::uint64_t TcpSender::packetsSent() const noexcept { return impl->sent.load (std::memory_order_relaxed); }
    std::uint64_t TcpSender::failures() const noexcept    { return impl->failed.load (std::memory_order_relaxed); }
}
