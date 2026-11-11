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

    Origin: go.dot src/wfg/engine/oscquery/OscQueryClient.cpp at 1c8aaa0a
    (OscQueryClient::get and its helpers), as a free function; the User-Agent
    names this project; readValue and valueFromReply left behind.
*/

#include <oscquery/net/HttpGet.h>
#include <oscquery/net/WriteNoSignal.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace oqa::oscquery::net
{
    namespace
    {
        /*  How long is left of a deadline, never below zero and never above
            what was asked for. Every socket call takes a timeout of its own, so
            a request with a 2000 ms budget that spent 1800 ms connecting has
            200 ms left to read in - rather than 2000 more. */
        int remaining (std::chrono::steady_clock::time_point deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds> (
                                deadline - std::chrono::steady_clock::now()).count();

            return static_cast<int> (std::clamp<std::int64_t> (left, 0, 600000));
        }

        /*  A chunked body, put back together.

            Rare against an OSCQuery server and not hypothetical: the framing is
            the server's choice, not the client's, and a reply that arrived with
            its size markers still in it would parse as broken JSON and be
            reported as a device that answered nonsense. */
        std::string deChunk (const std::string& body)
        {
            std::string out;
            std::size_t at = 0;

            while (at < body.size())
            {
                const auto lineEnd = body.find ("\r\n", at);

                if (lineEnd == std::string::npos)
                    break;

                const auto size = std::strtoul (body.substr (at, lineEnd - at).c_str(), nullptr, 16);

                if (size == 0)
                    break;

                const auto start = lineEnd + 2;

                if (start + size > body.size())
                    break;

                out.append (body, start, size);
                at = start + size + 2;                  // past the chunk's own CRLF
            }

            return out;
        }
    }

    //==============================================================================
    HttpReply httpGet (const std::string& host, int port, const std::string& path,
                       const std::string& query, int timeoutMs)
    {
        HttpReply reply;

        if (host.empty() || port <= 0 || port > 65535)
        {
            reply.error = "no usable address";
            return reply;
        }

        const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds (std::max (1, timeoutMs));

        juce::StreamingSocket socket;

        if (! socket.connect (juce::String (host), port, remaining (deadline)))
        {
            reply.error = "could not connect to " + host + ":" + std::to_string (port);
            return reply;
        }

        /*  THE QUERY GOES ON VERBATIM. OSCQuery asks with a bare key and a
            client that helpfully turned `?VALUE` into `?VALUE=` would be
            answered 400 by a conforming server, for ever, with nothing in the
            log to say why. */
        const auto target = query.empty() ? path : path + "?" + query;

        const std::string request = "GET " + target + " HTTP/1.1\r\n"
                                    "Host: " + host + ":" + std::to_string (port) + "\r\n"
                                    "User-Agent: OSCQuery-Automation-Plugin\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";

        //  Never juce's own write: a server that closed in the meantime would end the process (WriteNoSignal.h).
        if (writeNoSignal (socket, request.data(), static_cast<int> (request.size()))
              != static_cast<int> (request.size()))
        {
            reply.error = "the request could not be written";
            return reply;
        }

        std::string response;
        char buffer[4096];

        for (;;)
        {
            const auto left = remaining (deadline);

            if (left <= 0)
            {
                reply.error = "the device did not answer in time";
                return reply;
            }

            const auto ready = socket.waitUntilReady (true, left);

            if (ready < 0)
            {
                reply.error = "the connection broke while reading";
                return reply;
            }

            if (ready == 0)
            {
                reply.error = "the device did not answer in time";
                return reply;
            }

            const auto read = socket.read (buffer, static_cast<int> (sizeof (buffer)), false);

            /*  Zero is the far end closing, which with `Connection: close` is
                how a complete reply ends. It is the ordinary exit from this
                loop and not a failure. */
            if (read <= 0)
                break;

            response.append (buffer, static_cast<std::size_t> (read));

            /*  A guard rather than a policy: an OSCQuery description of a real
                rig is a couple of megabytes, and something answering an endless
                stream is not a device this is going to make sense of. */
            if (response.size() > 16u * 1024u * 1024u)
            {
                reply.error = "the device sent more than this is willing to read";
                return reply;
            }
        }

        socket.close();

        //  "HTTP/1.1 204 No Content" -> 204
        const auto firstSpace = response.find (' ');

        if (firstSpace == std::string::npos || response.rfind ("HTTP/", 0) != 0)
        {
            reply.error = "the device did not answer with HTTP";
            return reply;
        }

        reply.status = std::atoi (response.c_str() + firstSpace + 1);

        const auto headerEnd = response.find ("\r\n\r\n");

        if (headerEnd == std::string::npos)
        {
            reply.error = "the reply had no end of headers";
            return reply;
        }

        const auto headers = response.substr (0, headerEnd);
        auto body = response.substr (headerEnd + 4);

        /*  Case-insensitively, because a header name is and a device that
            writes `Transfer-Encoding` differently from the last one is not
            wrong. */
        auto lowered = headers;
        std::transform (lowered.begin(), lowered.end(), lowered.begin(),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });

        if (lowered.find ("transfer-encoding: chunked") != std::string::npos)
            body = deChunk (body);

        reply.body = std::move (body);
        reply.ok = true;
        return reply;
    }
}
