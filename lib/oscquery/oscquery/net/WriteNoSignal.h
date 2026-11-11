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
    A WRITE TO A STREAM SOCKET THAT CANNOT KILL THE PROCESS. On macOS and
    Linux, writing to a TCP socket whose peer has closed raises SIGPIPE, and
    what a process does with SIGPIPE unless it says otherwise is die, at once
    and without a word: no crash report, nothing logged.
    juce::StreamingSocket::write is a plain send() with no flag and sets no
    option against it, so every HTTP request this library made, and every OSC
    packet over TCP, could end the host application: a device restarting in
    the instant between the connection and the write was enough. Found on
    2026-11-11, when a test that restarts its device made that instant likely
    and the test program vanished once in a dozen runs on a Mac.

    A plugin does not own its process's signal handlers, so SIGPIPE is not
    ignored for everybody: it is not raised for THIS socket. Linux has a flag
    for the call (MSG_NOSIGNAL), macOS an option on the socket
    (SO_NOSIGPIPE); Windows has no such signal. A failed write then returns
    -1, which every caller here already treats as the connection being gone.

    Everything in this repository that writes to a juce::StreamingSocket goes
    through this, the tests' own sockets included.
*/

#include <juce_core/juce_core.h>

#if ! JUCE_WINDOWS
 #include <sys/socket.h>
 #include <sys/types.h>
#endif

namespace oqa::oscquery::net
{
    /** As juce::StreamingSocket::write: the bytes written, or -1. Never raises SIGPIPE. */
    inline int writeNoSignal (juce::StreamingSocket& socket, const void* data, int size) noexcept
    {
      #if JUCE_WINDOWS
        return socket.write (data, size);
      #else
        if (data == nullptr || size < 0 || ! socket.isConnected())
            return -1;

        const auto handle = socket.getRawSocketHandle();

        if (handle < 0)
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
}
