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

    Origin: go.dot src/wfg/engine/oscquery/Discovery.h at 1c8aaa0a, namespace
    oqa::oscquery; instanceName() (Go.dot's own naming) removed; Options gains
    hostLabelPrefix for the "godot-" host label. Its comments still say Go.dot
    where they mean the announcing program. 2026-11-10: multicast answers and
    questions go through an AnswerPacer (a delay, duplicate suppression, one
    second's rest per record, a budget), which go.dot's do not.
*/

#pragma once

/*
    OSCQUERY SERVERS FOUND ON THE NETWORK (namespace draft §76, ARU, ARW, ASA).

    A thread with one UDP socket on port 5353, shared with the system's own
    responder, joined to 224.0.0.251 on every IPv4 interface: it asks for
    `_oscjson._tcp` - at the start, then after one, two, four... seconds up to
    a minute, and whenever somebody asks (`browse`, from `network.browse`) - and
    keeps what answers: an instance (PTR), its host and port (SRV), its notes
    (TXT), the host's address (A), each until its time to live runs out or a
    goodbye takes it. An instance whose host or port is missing is asked about
    once a second until both come. Complete instances are the list.

    WHAT IS FOUND IS NOT A DECISION (ARW): the list is a sentence a line,
    published as `network/found`, never a command or a line of the log. Picking
    one is the gesture, and that is `mount.describeAt`.

    WHEN 5353 CANNOT BE SHARED (ASA): a port of the system's choosing, the
    question sent to the group with "answer me directly" set (RFC 6762 §6.7),
    every five seconds; `mode` says `unicast`. A test binds no part of 5353:
    its options name port 0 and the responders to ask directly (`peers`).

    LEAVING ITSELF OUT: a service at one of this machine's addresses on
    `ownPort` - Go.dot's own HTTP port - is this Go.dot, not a device.

    AND GO.DOT ANNOUNCED (ARU, ASA), on the same socket, when `announceAs`
    names it: "Go.dot on STAGE-PC" as `_oscjson._tcp` on the HTTP port and as
    `_osc._udp` on the OSC port, with PTR, SRV, TXT (`txtvers=1`) and an A
    record for each of this machine's addresses, under a host name of its own
    (`godot-stage-pc.local`) so the system's own records are not contradicted.
    Sent twice at the start, a second apart; answered whenever asked - to the
    group, or straight back to a querier asking from another port or asking
    so (RFC 6762 §5.4, §6.7); withdrawn at the stop with a time to live of
    nought. It does not probe for a name already in use (§8.1): a second
    Go.dot on one machine announces under its port, and two machines of one
    name are a rig nobody can tell apart anyway.

    THE HANDLER OF EVERYTHING IS THE THREAD: the socket is read and written on
    it alone, the maps are its own, and what anybody else reads - the list, the
    mode, the problem - is copied out under one lock, a revision saying when it
    moved.
*/

#include <oscquery/mdns/AnswerPacer.h>
#include <oscquery/mdns/Mdns.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace juce { class DatagramSocket; }

namespace oqa::oscquery
{
    class Discovery
    {
    public:
        struct Options
        {
            int port = mdns::port;                               ///< nought: one of the system's, no group joined
            std::vector<std::pair<std::string, int>> peers {};   ///< responders asked directly as well
            std::string service = "_oscjson._tcp.local";
            int ownPort = 0;                                     ///< this Go.dot's HTTP port, left out of the list

            std::string announceAs {};                           ///< the instance to announce, or empty for none
            int oscPort = 0;                                     ///< the OSC port announced beside it

            /*  The announcer's own host name is this prefix and the machine's
                name, under .local, so the system's own records are not
                contradicted (go.dot: "godot-"). */
            std::string hostLabelPrefix { "oscquery-" };
        };

        /** A complete instance: its name, where it answers, what it says of itself. */
        struct Found
        {
            std::string name {};
            std::string host {};
            int queryPort = 0;
            std::vector<std::string> txt {};
        };

        Discovery();
        ~Discovery();

        Discovery (const Discovery&) = delete;
        Discovery& operator= (const Discovery&) = delete;

        /*  Binds, joins and starts asking. False when no socket could be had
            at all; a 5353 that cannot be shared is not that - it is `unicast`. */
        bool start (Options options);

        /** Says goodbye where it announced, stops, joins. The destructor calls it. */
        void stop();

        /** Any thread: ask now, and again after one, two, four seconds. */
        void browse();

        /*  `off` before a start, `browsing` on 5353 with the group joined,
            `unicast` asking from a port of its own. */
        std::string mode() const;
        std::string problem() const;
        std::vector<Found> found() const;

        /*  `network/found`: a line each, name, a tab, address, a tab, query
            port, sorted by name. */
        std::string foundText() const;

        /** Moves whenever the list, the mode or the problem does. */
        std::uint64_t revision() const noexcept { return stamp.load (std::memory_order_acquire); }

        /** The port bound, for a test that bound nought. */
        int boundPort() const noexcept { return bound.load (std::memory_order_relaxed); }

    private:
        using Clock = std::chrono::steady_clock;

        struct Instance
        {
            mdns::Name name {};
            Clock::time_point expires {};
            bool hasService = false;
            mdns::Name host {};
            int port = 0;
            Clock::time_point serviceExpires {};
            std::vector<std::string> txt {};
            Clock::time_point askedAt {};
        };

        struct Host
        {
            std::vector<std::string> addresses {};
            Clock::time_point expires {};
            Clock::time_point askedAt {};
        };

        /*  WHAT GO.DOT ANNOUNCES: the names, built once at the start. */
        struct Own
        {
            mdns::Name jsonService {};
            mdns::Name oscService {};
            mdns::Name jsonInstance {};
            mdns::Name oscInstance {};
            mdns::Name host {};
        };

        void run();
        void openInterfaces();
        void ask (const std::vector<mdns::Question>& questions, bool toGroup);
        void heard (const std::vector<std::uint8_t>& bytes, const std::string& fromIp, int fromPort);
        void followUp (Clock::time_point now);
        void sweep (Clock::time_point now);
        void publish();
        void say (std::string modeNow, std::string problemNow);

        bool announcing() const noexcept { return ! options.announceAs.empty(); }
        std::vector<mdns::Record> addressRecords (std::uint32_t ttl, bool withLoopback) const;
        void answer (const mdns::Message& asked, const std::string& fromIp, int fromPort);
        void sendDueAnswers (Clock::time_point now);
        void announce (std::uint32_t ttl);
        void sendToGroup (const std::vector<std::uint8_t>& bytes);
        std::string preferred (const std::vector<std::string>& addresses) const;

        Options options;
        mdns::Name service;
        std::unique_ptr<juce::DatagramSocket> socket;
        std::vector<std::string> interfaces;
        std::vector<std::string> ownAddresses;
        bool onGroup = false;

        /*  When a multicast answer may go out, and the budget of datagrams to
            the group (OSCQuery Automation Plugin, 2026-11-10; not in go.dot):
            see AnswerPacer.h for the flood this ends. The browser's thread. */
        mdns::AnswerPacer pacer;
        juce::Random delays;

        std::map<std::string, Instance> instances;     ///< by the instance's name, lower case
        std::map<std::string, Host> hosts;             ///< by the host's name, lower case

        Clock::time_point nextQuery {};
        std::chrono::seconds interval { 1 };

        Own own;
        int announcementsLeft = 0;
        Clock::time_point nextAnnouncement {};

        mutable std::mutex guard;
        std::vector<Found> list;
        std::string modeWord = "off";
        std::string problemWords;
        std::atomic<std::uint64_t> stamp { 0 };
        std::atomic<bool> browseWanted { false };
        std::atomic<int> bound { 0 };

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };
        std::thread thread;
    };
}
