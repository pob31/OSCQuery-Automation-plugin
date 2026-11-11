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
    oscquery-dump: the library's example, and Phase 2's done-when ("a
    command-line example prints WFS-DIY's tree, subscribes to a path and prints
    its changes, with no address typed").

        oscquery-dump --browse [seconds]
            Devices found over DNS-SD (and probed), for a few seconds.

        oscquery-dump <host:port> [--json] [--listen <path>] [--listen-first] [--seconds N]
            HOST_INFO, then the tree; with --listen, the values of that path as
            the device streams them, until Ctrl-C (or N seconds). --listen-first
            picks the first leaf the device lets this client read, so a test of
            the stream needs no path typed anywhere.

    Exit codes follow sysexits: 64 usage, 65 the device answered something that
    is not a tree, 69 the device is unreachable.
*/

#include <oscquery/Device.h>
#include <oscquery/Finder.h>

#include <juce_events/juce_events.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace oqa::oscquery;

namespace
{
    std::atomic<bool> interrupted { false };

    void onSignal (int) { interrupted.store (true); }

    int usage()
    {
        std::cerr << "usage: oscquery-dump --browse [seconds]\n"
                     "       oscquery-dump <host:port> [--json] [--listen <path>] [--listen-first] [--seconds N]\n";
        return 64;
    }

    std::string rangeText (const Argument& argument)
    {
        if (! argument.range.has_value())
            return "";

        std::string text;

        if (argument.range->hasMinMax())
            text = "[" + osc::formatDouble (*argument.range->min) + ", " + osc::formatDouble (*argument.range->max) + "]";

        if (argument.range->hasVals())
            text += (text.empty() ? "" : " ") + std::string ("{") + osc::atomsOf (argument.range->vals) + "}";

        return text;
    }

    void printTree (const Tree& tree)
    {
        tree.forEach ([] (const Node& node)
                      {
                          std::cout << node.fullPath;

                          if (node.isContainer())
                          {
                              std::cout << (node.loaded ? "/" : "/ (not loaded)") << "\n";
                              return;
                          }

                          std::cout << "  " << *node.type;

                          if (node.access.has_value())
                              std::cout << "  access " << *node.access;

                          for (std::size_t i = 0; i < node.argumentCount(); ++i)
                          {
                              const auto argument = node.argument (i);
                              const auto range = rangeText (argument);

                              if (! range.empty())
                                  std::cout << "  " << range;

                              if (! argument.unit.empty())
                                  std::cout << " " << argument.unit;
                          }

                          if (node.hasValue)
                              std::cout << "  = " << osc::atomsOf (node.value);

                          if (node.critical)
                              std::cout << "  CRITICAL";

                          if (node.description.has_value())
                              std::cout << "  \"" << *node.description << "\"";

                          std::cout << "\n";
                      });
    }

    int browse (int seconds)
    {
        Finder finder;

        if (! finder.start ({}))
        {
            std::cerr << "no socket to browse from\n";
            return 69;
        }

        std::cout << "browsing (" << finder.browseMode() << ")";

        if (! finder.browseProblem().empty())
            std::cout << ": " << finder.browseProblem();

        std::cout << "\n";

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds);
        std::uint64_t shown = 0;

        while (std::chrono::steady_clock::now() < deadline && ! interrupted.load())
        {
            if (finder.revision() != shown)
            {
                shown = finder.revision();
                std::cout << "--\n";

                for (const auto& c : finder.candidates())
                    std::cout << (c.reachable ? "  ok    " : "  down  ") << c.name << "\t" << c.host << ":" << c.port
                              << (c.advertised ? "\tdns-sd" : "") << (c.manual ? "\ttyped" : "")
                              << (c.error.empty() ? "" : "\t" + c.error) << "\n";
            }

            std::this_thread::sleep_for (std::chrono::milliseconds (200));
        }

        finder.stop();
        return 0;
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juce;

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    if (argc < 2)
        return usage();

    const std::string first = argv[1];

    if (first == "--browse")
        return browse (argc > 2 ? std::max (1, std::atoi (argv[2])) : 5);

    const auto colon = first.rfind (':');

    if (first.empty() || first[0] == '-' || colon == std::string::npos)
        return usage();

    Device::Options options;
    options.host = first.substr (0, colon);
    options.port = std::atoi (first.c_str() + colon + 1);

    bool asJson = false;
    bool listenFirst = false;
    std::string listenPath;
    int seconds = 0;

    for (int i = 2; i < argc; ++i)
    {
        const std::string argument = argv[i];

        if (argument == "--json")
            asJson = true;
        else if (argument == "--listen-first")
            listenFirst = true;
        else if (argument == "--listen" && i + 1 < argc)
            listenPath = argv[++i];
        else if (argument == "--seconds" && i + 1 < argc)
            seconds = std::atoi (argv[++i]);
        else
            return usage();
    }

    Device device (options);

    if (! device.probe())
    {
        std::cerr << options.host << ":" << options.port << " is unreachable: " << device.lastError() << "\n";
        return 69;
    }

    const auto info = device.hostInfo();
    std::cout << "NAME " << info->name << "\nOSC " << info->oscTransport << " " << info->oscHost (options.host)
              << ":" << info->oscPort << "\nwebsocket " << (info->offersWebsocket() ? "offered" : "not offered");

    if (info->extensionsListed)
    {
        std::cout << "\nEXTENSIONS";

        for (const auto& extension : info->extensions)
            std::cout << " " << extension;
    }

    //  The two optional extensions, when the device declares them.
    if (info->deviceId.has_value())
        std::cout << "\nDEVICE_ID " << *info->deviceId;

    if (info->clock.has_value())
        std::cout << "\nCLOCK " << info->clock->source
                  << (info->clock->domain.has_value() ? " domain " + *info->clock->domain : std::string())
                  << (info->clock->scheduling ? ", scheduling by time tag" : ", applying on arrival");

    if (! info->meetsC1())
    {
        std::cout << "\nmissing (C1):";

        for (const auto& field : info->missing)
            std::cout << " " << field;
    }

    std::cout << "\n\n";

    if (! device.fetchTree())
    {
        std::cerr << "the tree could not be read: " << device.lastError() << "\n";
        return 65;
    }

    const auto tree = device.tree();

    if (asJson)
        std::cout << juce::JSON::toString (tree.toVar()).toStdString() << "\n";
    else
        printTree (tree);

    std::cout << "\n" << tree.nodeCount() << " nodes, " << tree.leafCount() << " leaves, tree hash " << tree.treeHash() << "\n";

    if (listenFirst && listenPath.empty())
    {
        for (const auto* leaf : tree.leaves())
            if (leaf->bindableForRecord())
            {
                listenPath = leaf->fullPath;
                break;
            }

        if (listenPath.empty())
        {
            std::cerr << "no leaf this client may read\n";
            return 0;
        }
    }

    if (listenPath.empty())
        return 0;

    Device::Callbacks callbacks;
    callbacks.onValue = [] (const std::string& path, const osc::Values& values)
    {
        std::cout << path << "  " << osc::atomsOf (values) << std::endl;
    };
    callbacks.onState = [] (Device::State state) { std::cout << "[" << Device::stateName (state) << "]" << std::endl; };
    callbacks.onError = [] (const std::string& message) { std::cerr << "! " << message << std::endl; };
    callbacks.onTreeChanged = [] (const std::string& path) { std::cout << "[changed " << path << "]" << std::endl; };
    callbacks.onPathRemoved = [] (const std::string& path) { std::cout << "[removed " << path << "]" << std::endl; };
    callbacks.onPathRenamed = [] (const std::string& a, const std::string& b) { std::cout << "[renamed " << a << " -> " << b << "]" << std::endl; };
    device.setCallbacks (callbacks);

    const auto accepted = device.listen (listenPath);

    if (accepted == Device::Listen::unsupported)
    {
        std::cout << "\nthis device does not list LISTEN; nothing to subscribe to\n";
        return 0;
    }

    std::cout << "\nlistening to " << listenPath << (seconds > 0 ? " for " + std::to_string (seconds) + " s" : " until Ctrl-C") << "\n";
    device.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (seconds > 0 ? seconds : 1000000);

    while (! interrupted.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (100));

    device.stop();
    return 0;
}
