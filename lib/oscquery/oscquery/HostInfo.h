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
    WHAT `GET /?HOST_INFO` SAYS, read the way the Core profile reads it (spec
    draft C1): NAME, OSC_PORT and OSC_TRANSPORT as the proposal defines them;
    OSC_IP, WS_IP and WS_PORT with their fallbacks (absent OSC_IP means the HTTP
    host, absent WS_PORT the HTTP port); EXTENSIONS as the one list of what the
    device serves, and a client that never assumes a feature it does not list.

    THE PROFILE ADDS NO FIELD (decided 2026-11-04): what the proposal defines
    is held here and the rest is kept in `raw`.

    TWO OPTIONAL EXTENSIONS BESIDE THE PROFILE (decided 2026-11-10; devplan
    Phase 6x; docs/spec/extensions-draft-0.1.md): DEVICE_ID, a string the
    device keeps for good, and CLOCK, what its time tags refer to and whether
    it schedules by them. Each is read ONLY when EXTENSIONS lists it: a key
    that is there without being declared is somebody else's key and stays in
    `raw`. A declared one that is malformed is ignored, never a refusal. A
    device with neither reads exactly as before.

    A device that lacks one of the three required fields is still parsed - the
    library copes with devices as they are - and `missing` names what C1 would
    fail it on, so the editor and the conformance tool can say so.
*/

#include <juce_core/juce_core.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace oqa::oscquery
{
    /*  The optional CLOCK extension, as declared: what disciplines the clock
        the device's time tags refer to ("ntp", "ptp" or "local", lower-cased
        as read; a value this library does not know is kept as given), which
        domain or server when the device says, and whether a bundle's future
        time tag is executed at that time (true) or the bundle applied on
        arrival (false). The same struct is what a server is told to serve. */
    struct ClockInfo
    {
        std::string source { "local" };
        std::optional<std::string> domain;
        bool scheduling = false;

        bool operator== (const ClockInfo&) const = default;

        /** The object HOST_INFO carries under CLOCK. */
        juce::var toVar() const;
    };

    /** The names the two optional extensions go by, in EXTENSIONS and as keys. */
    inline constexpr const char* deviceIdExtension = "DEVICE_ID";
    inline constexpr const char* clockExtension = "CLOCK";

    struct HostInfo
    {
        std::string name;
        int oscPort = 0;

        /** As given, upper-cased: "UDP" or "TCP". Defaults to UDP when absent. */
        std::string oscTransport { "UDP" };

        std::optional<std::string> oscIp;
        std::optional<std::string> wsIp;
        std::optional<int> wsPort;

        /** The names EXTENSIONS lists as true. */
        std::set<std::string> extensions;

        /** Whether EXTENSIONS was present at all. Absent, nothing optional is assumed. */
        bool extensionsListed = false;

        /** Of NAME, OSC_PORT and OSC_TRANSPORT, the ones the device did not give (C1). */
        std::vector<std::string> missing;

        /** DEVICE_ID, when EXTENSIONS lists it and it is a non-empty string.
            Opaque: compared as text, nothing read into it. */
        std::optional<std::string> deviceId;

        /** CLOCK, when EXTENSIONS lists it and it is an object with a SOURCE. */
        std::optional<ClockInfo> clock;

        /** The object as the device sent it. */
        juce::var raw;

        //==============================================================================
        /** True when EXTENSIONS lists `extension` as true. Never infers an unlisted one. */
        bool has (std::string_view extension) const;

        bool meetsC1() const noexcept           { return missing.empty(); }
        bool oscOverTcp() const noexcept        { return oscTransport == "TCP"; }

        /** Where OSC goes: OSC_IP, else the host the HTTP reply came from. */
        std::string oscHost (const std::string& httpHost) const;

        /** Where the websocket is: WS_IP / WS_PORT, else the HTTP host and port. */
        std::string websocketHost (const std::string& httpHost) const;
        int websocketPort (int httpPort) const noexcept;

        /*  Whether a websocket is worth opening at all: LISTEN or any PATH_*
            notification listed. A device that lists none is a Core device
            without websocket, and the proposal makes websockets optional. */
        bool offersWebsocket() const;
    };

    /*  Reads a HOST_INFO object. nullopt, with `error` set, only when the value
        is not a JSON object at all; anything missing inside is reported through
        HostInfo::missing instead. */
    std::optional<HostInfo> parseHostInfo (const juce::var& object, std::string& error);
    std::optional<HostInfo> parseHostInfo (std::string_view json, std::string& error);
}
