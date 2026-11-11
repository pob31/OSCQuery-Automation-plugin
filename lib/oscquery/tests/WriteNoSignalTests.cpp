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
    A WRITE TO A PEER THAT HAS GONE (oscquery/net/WriteNoSignal.h). The case
    passes by still being there at its end: with juce::StreamingSocket::write
    in writeNoSignal's place, the second or third write below raises SIGPIPE
    on macOS and Linux and the whole test program is gone, without a line.
*/

#include <doctest/doctest.h>

#include <oscquery/net/HttpGet.h>
#include <oscquery/net/TcpSender.h>
#include <oscquery/net/WriteNoSignal.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace oqa::oscquery;

namespace
{
    /** A listener on a port of the system's choosing; accepts one connection and hands it over. */
    struct Listener
    {
        juce::StreamingSocket socket;
        int port = 0;

        Listener()
        {
            REQUIRE (socket.createListener (0, "127.0.0.1"));
            port = socket.getBoundPort();
            REQUIRE (port > 0);
        }

        std::unique_ptr<juce::StreamingSocket> accept()
        {
            REQUIRE (socket.waitUntilReady (true, 5000) == 1);
            return std::unique_ptr<juce::StreamingSocket> (socket.waitForNextConnection());
        }
    };
}

TEST_CASE ("write no signal: writing to a peer that has closed fails with -1 and the process lives")
{
    Listener listener;

    juce::StreamingSocket client;
    REQUIRE (client.connect ("127.0.0.1", listener.port, 3000));

    auto accepted = listener.accept();
    REQUIRE (accepted != nullptr);

    //  While the peer is there, bytes go through.
    const std::string hello = "hello";
    CHECK (net::writeNoSignal (client, hello.data(), static_cast<int> (hello.size())) == static_cast<int> (hello.size()));

    //  The peer goes, as a device restarting does.
    accepted->close();
    accepted.reset();

    /*  The first write after that may still be taken by the system; the one
        after the reset comes back is the one that used to be fatal. */
    const std::vector<char> block (4096, 'x');
    bool failed = false;

    for (int attempt = 0; attempt < 200 && ! failed; ++attempt)
    {
        failed = net::writeNoSignal (client, block.data(), static_cast<int> (block.size())) < 0;
        juce::Thread::sleep (5);
    }

    CHECK (failed);

    //  Nothing sensible to write, nothing written.
    CHECK (net::writeNoSignal (client, nullptr, 4) == -1);

    juce::StreamingSocket never;
    CHECK (net::writeNoSignal (never, hello.data(), static_cast<int> (hello.size())) == -1);
}

TEST_CASE ("write no signal: the TCP sender survives its device closing the connection, and says the packet was not sent")
{
    Listener listener;

    net::TcpSender::Options options;
    options.host = "127.0.0.1";
    options.port = listener.port;
    net::TcpSender sender (options);

    std::string error;
    const std::vector<std::uint8_t> packet (512, 0x2f);
    REQUIRE_MESSAGE (sender.send (packet, error), error);

    auto accepted = listener.accept();
    REQUIRE (accepted != nullptr);
    accepted->close();
    accepted.reset();
    listener.socket.close();                    // and nothing listens there any more

    bool refused = false;

    for (int attempt = 0; attempt < 200 && ! refused; ++attempt)
    {
        refused = ! sender.send (packet, error);
        juce::Thread::sleep (5);
    }

    CHECK (refused);
    CHECK_FALSE (error.empty());
}

TEST_CASE ("write no signal: an HTTP GET to a server that closes without answering comes back as an error")
{
    Listener listener;

    /*  The server accepts and hangs up at once, again and again: whichever
        side of the request's write that falls on, the GET returns. */
    std::atomic<bool> stop { false };

    std::thread server ([&]
    {
        while (! stop.load())
        {
            if (listener.socket.waitUntilReady (true, 50) == 1)
                if (std::unique_ptr<juce::StreamingSocket> connection { listener.socket.waitForNextConnection() })
                    connection->close();
        }
    });

    for (int attempt = 0; attempt < 40; ++attempt)
    {
        const auto reply = net::httpGet ("127.0.0.1", listener.port, "/", "HOST_INFO", 500);
        CHECK_FALSE (reply.ok);
    }

    stop.store (true);
    server.join();
}
