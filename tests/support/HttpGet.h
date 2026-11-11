/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later

    Origin: go.dot tests/OscQueryTests.cpp at 1c8aaa0a (lowerCased, HttpReply,
    sendRequest, get), moved into a header, GET only, oqatest namespace.
*/

#pragma once

/*
    One HTTP GET over a raw socket, reading the status line off the wire.

    Not juce::URL: it rewrites OSCQuery's bare `?HOST_INFO` into `?HOST_INFO=`,
    which no server matching the proposal recognises (WFS-DIY and go.dot both
    abandoned it for this), and go.dot saw it report 200 for a 204. Writing the
    request and reading the response is short and tells the truth.
*/

#include <juce_core/juce_core.h>

#if ! JUCE_WINDOWS
 #include <sys/socket.h>
 #include <sys/types.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace oqatest
{
    inline std::string lowerCased (std::string text)
    {
        std::transform (text.begin(), text.end(), text.begin(),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
        return text;
    }

    /** One HTTP reply: status, headers (a multimap, so a field sent twice is
        seen twice) and body. Status 0 means no reply at all. */
    /*  juce::StreamingSocket::write is a plain send(): to a peer that has
        closed it raises SIGPIPE on macOS and Linux, and the test program is
        gone without a line. The library's own is oscquery/net/WriteNoSignal.h,
        which says more; this is its twin, for a header that some test targets
        include without the library. */
    inline int writeNoSignal (juce::StreamingSocket& socket, const void* data, int size) noexcept
    {
      #if JUCE_WINDOWS
        return socket.write (data, size);
      #else
        const auto handle = socket.getRawSocketHandle();

        if (handle < 0 || ! socket.isConnected())
            return -1;

       #if defined (MSG_NOSIGNAL)
        return static_cast<int> (::send (handle, data, static_cast<std::size_t> (size), MSG_NOSIGNAL));
       #else
        const int on = 1;
        (void) ::setsockopt (handle, SOL_SOCKET, SO_NOSIGPIPE, &on, static_cast<socklen_t> (sizeof (on)));
        return static_cast<int> (::send (handle, data, static_cast<std::size_t> (size), 0));
       #endif
      #endif
    }

    struct HttpReply
    {
        int status = 0;
        std::string body;
        std::multimap<std::string, std::string> headers;

        /** The value of one header, empty if the reply carries no such field. */
        std::string header (const std::string& name) const
        {
            const auto key = lowerCased (name);
            const auto found = headers.lower_bound (key);
            return found != headers.end() && found->first == key ? found->second : std::string();
        }
    };

    /** GET `target` (path and query, as sent on the request line) from 127.0.0.1:port. */
    inline HttpReply get (int port, const std::string& target)
    {
        HttpReply reply;
        juce::StreamingSocket socket;

        if (! socket.connect ("127.0.0.1", port, 10000))
            return reply;

        const std::string request = "GET " + target + " HTTP/1.1\r\n"
                                    + "Host: 127.0.0.1\r\n"
                                    + "Connection: close\r\n"
                                    + "\r\n";

        if (writeNoSignal (socket, request.data(), static_cast<int> (request.size()))
              != static_cast<int> (request.size()))
            return reply;

        std::string response;
        char buffer[4096];

        for (;;)
        {
            if (socket.waitUntilReady (true, 10000) <= 0)
                break;

            const auto read = socket.read (buffer, static_cast<int> (sizeof (buffer)), false);

            if (read <= 0)
                break;

            response.append (buffer, static_cast<std::size_t> (read));
        }

        socket.close();

        //  "HTTP/1.1 204 No Content" -> 204
        const auto firstSpace = response.find (' ');

        if (firstSpace == std::string::npos)
            return reply;

        reply.status = std::atoi (response.c_str() + firstSpace + 1);

        const auto bodyStart = response.find ("\r\n\r\n");

        if (bodyStart != std::string::npos)
            reply.body = response.substr (bodyStart + 4);

        //  Header fields: name, colon, value without its leading whitespace
        //  (RFC 9110 §5.5). A line with no colon is skipped, not guessed at.
        const auto headerEnd = bodyStart == std::string::npos ? response.size() : bodyStart;
        auto lineStart = response.find ("\r\n");

        if (lineStart != std::string::npos)
            lineStart += 2;

        while (lineStart != std::string::npos && lineStart < headerEnd)
        {
            auto lineEnd = response.find ("\r\n", lineStart);

            if (lineEnd == std::string::npos || lineEnd > headerEnd)
                lineEnd = headerEnd;

            const auto colon = response.find (':', lineStart);

            if (colon != std::string::npos && colon < lineEnd)
            {
                auto value = response.substr (colon + 1, lineEnd - colon - 1);

                while (! value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.erase (value.begin());

                reply.headers.emplace (lowerCased (response.substr (lineStart, colon - lineStart)),
                                       std::move (value));
            }

            lineStart = lineEnd + 2;
        }

        return reply;
    }
}
