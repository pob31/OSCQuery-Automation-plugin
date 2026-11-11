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
    oqa-mock-device: the mock device as a program, for curl, for pointing a
    plugin or the conformance CLI at it by hand, and for anything outside the
    test suite that needs a device.

        oqa-mock-device (--fixture <tree.json> | --generate <leaves>) [--port N]
                        [--name "Some name"] [--no-websocket] [--extensions A,B,C]
                        [--host-info-only] [--notify-unlisted] [--tcp] [--osc-ip IP]
                        [--echo-ms N] [--device-id ID] [--clock-source ntp|ptp|local]
                        [--clock-domain D] [--clock-scheduling]

    --device-id serves and declares the optional DEVICE_ID; any --clock-* flag
    serves and declares the optional CLOCK (docs/spec/extensions-draft-0.1.md).
    Give them after --extensions, which replaces the list.

    Prints one line, "oqa-mock-device http <port> osc <port>", once it answers,
    then serves until interrupted (Ctrl-C or SIGTERM). --generate serves a
    synthetic tree of that many leaves (tests/support/TreeGen.h), for the
    large-tree check by hand.
*/

#include "MockDevice.h"
#include "../support/TreeGen.h"

#include <atomic>
#include <csignal>
#include <iostream>

namespace
{
    std::atomic<bool> interrupted { false };

    void onSignal (int) { interrupted = true; }

    int usage()
    {
        std::cerr << "usage: oqa-mock-device (--fixture <tree.json> | --generate <leaves>) [--port N] [--name NAME]\n"
                     "                       [--no-websocket] [--extensions A,B,C] [--host-info-only]\n"
                     "                       [--notify-unlisted] [--tcp] [--osc-ip IP] [--echo-ms N]\n"
                     "                       [--device-id ID] [--clock-source ntp|ptp|local] [--clock-domain D]\n"
                     "                       [--clock-scheduling]\n";
        return 64;
    }
}

int main (int argc, char** argv)
{
    juce::File fixture;
    int generate = 0;
    oqatest::MockDevice::Options options;

    juce::String deviceId, clockSource, clockDomain;
    bool clockScheduling = false, clockGiven = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const bool hasValue = i + 1 < argc;

        if (arg == "--fixture" && hasValue)
            fixture = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[++i]));
        else if (arg == "--generate" && hasValue)
            generate = juce::String (argv[++i]).getIntValue();
        else if (arg == "--port" && hasValue)
            options.httpPort = juce::String (argv[++i]).getIntValue();
        else if (arg == "--name" && hasValue)
            options.name = juce::String (argv[++i]);
        else if (arg == "--no-websocket")
            options.websocket = false;
        else if (arg == "--extensions" && hasValue)
        {
            options.extensions.clear();
            options.extensions.addTokens (juce::String (argv[++i]), ",", "");
            options.extensions.trim();
            options.extensions.removeEmptyStrings();
        }
        else if (arg == "--host-info-only")
            options.hostInfoOnly = true;
        else if (arg == "--notify-unlisted")
            options.notifyUnlisted = true;
        else if (arg == "--tcp")
            options.oscTransport = "TCP";
        else if (arg == "--osc-ip" && hasValue)
            options.oscIp = juce::String (argv[++i]);
        else if (arg == "--echo-ms" && hasValue)
            options.echoDelayMs = juce::String (argv[++i]).getIntValue();
        else if (arg == "--device-id" && hasValue)
            deviceId = juce::String (argv[++i]);
        else if (arg == "--clock-source" && hasValue)
        {
            clockSource = juce::String (argv[++i]);
            clockGiven = true;
        }
        else if (arg == "--clock-domain" && hasValue)
        {
            clockDomain = juce::String (argv[++i]);
            clockGiven = true;
        }
        else if (arg == "--clock-scheduling")
            clockScheduling = clockGiven = true;
        else
            return usage();
    }

    if (deviceId.isNotEmpty() || clockGiven)
    {
        auto* extras = new juce::DynamicObject();

        if (deviceId.isNotEmpty())
        {
            extras->setProperty ("DEVICE_ID", deviceId);
            options.extensions.addIfNotAlreadyThere ("DEVICE_ID");
        }

        if (clockGiven)
        {
            auto* clock = new juce::DynamicObject();
            clock->setProperty ("SOURCE", clockSource.isNotEmpty() ? clockSource : juce::String ("local"));

            if (clockDomain.isNotEmpty())
                clock->setProperty ("DOMAIN", clockDomain);

            clock->setProperty ("SCHEDULING", clockScheduling);
            extras->setProperty ("CLOCK", juce::var (clock));
            options.extensions.addIfNotAlreadyThere ("CLOCK");
        }

        options.hostInfoExtras = juce::var (extras);
    }

    if (fixture == juce::File() && generate <= 0)
        return usage();

    oqatest::MockDevice device (options);

    if (generate > 0)
    {
        device.loadTree (oqatest::generateTree (generate));
    }
    else if (! device.loadTreeFromFile (fixture))
    {
        std::cerr << "oqa-mock-device: cannot read a JSON tree from " << fixture.getFullPathName() << "\n";
        return 66;
    }

    if (! device.start())
    {
        std::cerr << "oqa-mock-device: cannot bind (HTTP port " << options.httpPort << ")\n";
        return 69;
    }

    std::cout << "oqa-mock-device http " << device.httpPort() << " osc " << device.oscPort() << std::endl;

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    while (! interrupted)
        juce::Thread::sleep (100);

    device.stop();
    return 0;
}
