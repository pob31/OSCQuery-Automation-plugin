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

    Origin: go.dot src/wfg/engine/oscquery/Discovery.cpp at 1c8aaa0a, namespace and
    includes renamed; instanceName() removed; the host label's prefix and the
    5353 problem text no longer name Go.dot. 2026-11-10: answer(), ask() and
    sendToGroup() paced by an AnswerPacer (see AnswerPacer.h for why).
*/

#include <oscquery/mdns/Discovery.h>

#include <oscquery/mdns/MulticastJoin.h>

#include <juce_core/juce_core.h>

#include <algorithm>

namespace oqa::oscquery
{
    namespace
    {
        /*  A name as a key: dotted and in lower case, since DNS compares
            names without regard to case. Two instances whose names differ
            only in where a dot falls inside a label would share one - which
            no responder names twice. */
        std::string keyOf (const mdns::Name& name)
        {
            auto key = mdns::dotted (name);

            for (auto& c : key)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char> (c - 'A' + 'a');

            return key;
        }

        std::chrono::seconds secondsOf (std::uint32_t ttl)
        {
            return std::chrono::seconds (static_cast<std::chrono::seconds::rep> (ttl));
        }

        /*  THIS MACHINE'S IPv4 ADDRESSES, one an interface, the loopback left
            out: the interfaces the group is joined on and asked from. A link
            without a DHCP server - a Dante network, often - has an address
            in 169.254 and is kept: devices are found there too. */
        std::vector<std::string> ipv4Interfaces()
        {
            std::vector<std::string> out;

            for (const auto& address : juce::IPAddress::getAllAddresses (false))
            {
                /*  Windows lists the multicast groups it has joined among a
                    machine's addresses (serve's own client lines say so); a
                    group is not an interface. */
                if (address.isNull() || address.address[0] >= 224)
                    continue;

                const auto text = address.toString().toStdString();

                if (text.empty() || text.rfind ("127.", 0) == 0)
                    continue;

                out.push_back (text);
            }

            std::sort (out.begin(), out.end());
            out.erase (std::unique (out.begin(), out.end()), out.end());
            return out;
        }

        /*  A DNS label of a computer's name: lower case, letters, digits and
            dashes - what a host name may hold. */
        std::string hostLabelOf (const std::string& computer)
        {
            std::string label;

            for (const auto c : computer)
            {
                if (c >= 'A' && c <= 'Z')
                    label += static_cast<char> (c - 'A' + 'a');
                else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                    label += c;
                else if (! label.empty() && label.back() != '-')
                    label += '-';
            }

            while (! label.empty() && label.back() == '-')
                label.pop_back();

            return label.empty() ? std::string ("machine") : label;
        }

        mdns::Name under (const std::string& label, const mdns::Name& service)
        {
            mdns::Name name { label };
            name.insert (name.end(), service.begin(), service.end());
            return name;
        }

        bool sameNetwork (const std::string& a, const std::string& b)
        {
            const auto prefixOf = [] (const std::string& address) { return address.substr (0, address.rfind ('.')); };
            return prefixOf (a) == prefixOf (b);
        }

        std::string linesOf (const std::vector<Discovery::Found>& found)
        {
            std::string text;

            for (const auto& each : found)
                text += (text.empty() ? "" : "\n") + each.name + "\t" + each.host + "\t" + std::to_string (each.queryPort);

            return text;
        }
    }

    Discovery::Discovery() = default;

    Discovery::~Discovery()
    {
        stop();
    }

    namespace
    {
        /*  One record as the pacer knows it: the same text for the same record
            whoever sends it. The port is part of an SRV's, so two servers under
            one name are two records. */
        std::string pacerKey (const mdns::Record& record)
        {
            return keyOf (record.name) + "|" + std::to_string (record.type) + "|" + keyOf (record.target)
                 + (record.type == mdns::type::srv ? "|" + std::to_string (record.port) : std::string());
        }
    }

    bool Discovery::start (Options optionsToUse)
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        options = std::move (optionsToUse);
        service = mdns::nameOf (options.service);
        onGroup = false;

        std::string problemNow;

        /*  SHARED WITH THE SYSTEM'S RESPONDER: the constructor's address reuse
            is what shares a port on Windows and Linux, and port reuse is what
            does on macOS - set here, before the bind. */
        socket = std::make_unique<juce::DatagramSocket> (false);
        socket->setEnablePortReuse (true);

        if (options.port == mdns::port)
        {
            if (socket->bindToPort (mdns::port))
            {
                onGroup = true;
            }
            else
            {
                problemNow = "port 5353 is another program's alone: devices are asked from a port of this"
                             " client's own, and nothing is announced";
                socket = std::make_unique<juce::DatagramSocket> (false);

                if (! socket->bindToPort (0))
                {
                    say ("off", "no port to ask the network from");
                    socket.reset();
                    return false;
                }
            }
        }
        else if (! socket->bindToPort (options.port))
        {
            say ("off", "port " + std::to_string (options.port) + " could not be bound");
            socket.reset();
            return false;
        }

        bound.store (socket->getBoundPort(), std::memory_order_relaxed);
        interfaces.clear();
        openInterfaces();

        /*  A FRESH BROWSE (OSCQuery Automation Plugin, Phase 6x; not in go.dot):
            what an earlier start() heard is dropped. The options may name other
            peers or another service, and a record kept from before would be
            listed for its whole time to live (75 minutes for a PTR) by a
            browser that can no longer hear its withdrawal. What is still out
            there answers the first query, a moment from now. */
        instances.clear();
        hosts.clear();

        {
            const std::lock_guard<std::mutex> lock (guard);
            list.clear();
        }

        stamp.fetch_add (1, std::memory_order_acq_rel);

        say (onGroup ? "browsing" : "unicast", problemNow);

        if (announcing())
        {
            own.jsonService = mdns::nameOf ("_oscjson._tcp.local");
            own.oscService = mdns::nameOf ("_osc._udp.local");
            own.jsonInstance = under (options.announceAs, own.jsonService);
            own.oscInstance = under (options.announceAs, own.oscService);
            own.host = mdns::Name { options.hostLabelPrefix + hostLabelOf (juce::SystemStats::getComputerName().toStdString()), "local" };
            announcementsLeft = 2;
            nextAnnouncement = Clock::now();
        }

        nextQuery = Clock::now();
        interval = std::chrono::seconds (1);
        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);
        thread = std::thread ([this] { run(); });
        return true;
    }

    void Discovery::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        stopping.store (true, std::memory_order_relaxed);

        if (thread.joinable())
            thread.join();

        //  WITHDRAWN, on the socket's last breath: a time to live of nought (RFC 6762 §10.1).
        if (announcing() && socket != nullptr)
            announce (0);

        if (socket != nullptr)
            socket->shutdown();

        socket.reset();
        running.store (false, std::memory_order_relaxed);
    }

    void Discovery::browse()
    {
        browseWanted.store (true, std::memory_order_relaxed);
    }

    std::string Discovery::mode() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return modeWord;
    }

    std::string Discovery::problem() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return problemWords;
    }

    std::vector<Discovery::Found> Discovery::found() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return list;
    }

    std::string Discovery::foundText() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return linesOf (list);
    }

    void Discovery::say (std::string modeNow, std::string problemNow)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            modeWord = std::move (modeNow);
            problemWords = std::move (problemNow);
        }

        stamp.fetch_add (1, std::memory_order_acq_rel);
    }

    /*  THE GROUP JOINED ON EVERY INTERFACE NOT JOINED YET - at the start, and
        every thirty seconds after, so a network plugged in during the evening
        is asked too. Where no interface could be joined by address, the
        system's default is. */
    void Discovery::openInterfaces()
    {
        const auto now = ipv4Interfaces();
        ownAddresses = now;
        ownAddresses.push_back ("127.0.0.1");

        if (! onGroup)
        {
            interfaces = now;
            return;
        }

        const auto handle = socket->getRawSocketHandle();
        multicast::setHops (handle, 255);

        auto joinedAny = ! interfaces.empty();

        for (const auto& address : now)
        {
            if (std::find (interfaces.begin(), interfaces.end(), address) != interfaces.end())
                continue;

            if (multicast::join (handle, mdns::group, address))
            {
                interfaces.push_back (address);
                joinedAny = true;
            }
        }

        if (! joinedAny)
            socket->joinMulticast (mdns::group);
    }

    void Discovery::sendToGroup (const std::vector<std::uint8_t>& bytes)
    {
        const auto size = static_cast<int> (bytes.size());

        //  Within the budget of datagrams to the group, or not at all (AnswerPacer.h).
        if (onGroup && pacer.spend (Clock::now()))
        {
            if (interfaces.empty())
                socket->write (mdns::group, mdns::port, bytes.data(), size);

            for (const auto& address : interfaces)
            {
                multicast::sendFrom (socket->getRawSocketHandle(), address);
                socket->write (mdns::group, mdns::port, bytes.data(), size);
            }
        }

        for (const auto& [host, peerPort] : options.peers)
            socket->write (juce::String (host), peerPort, bytes.data(), size);
    }

    /*  AN A RECORD FOR EACH OF THIS MACHINE'S ADDRESSES - the loopback first
        when the one asking is on this machine, since that is the one it can
        always reach. */
    std::vector<mdns::Record> Discovery::addressRecords (std::uint32_t ttl, bool withLoopback) const
    {
        std::vector<mdns::Record> records;
        auto addresses = ownAddresses;

        std::erase (addresses, std::string ("127.0.0.1"));

        if (withLoopback || addresses.empty())
            addresses.insert (addresses.begin(), "127.0.0.1");

        for (const auto& text : addresses)
        {
            const juce::IPAddress parsed { juce::String (text) };
            mdns::Record record;
            record.name = own.host;
            record.type = mdns::type::a;
            record.cacheFlush = true;
            record.ttl = ttl;
            record.address = { parsed.address[0], parsed.address[1], parsed.address[2], parsed.address[3] };
            records.push_back (std::move (record));
        }

        return records;
    }

    void Discovery::announce (std::uint32_t ttl)
    {
        std::vector<mdns::Record> answers;
        const auto shared = ttl == 0 ? 0u : 4500u;
        const auto unique = ttl == 0 ? 0u : 120u;

        const auto add = [&answers] (mdns::Name name, std::uint16_t type, std::uint32_t life, bool flush)
        {
            mdns::Record record;
            record.name = std::move (name);
            record.type = type;
            record.ttl = life;
            record.cacheFlush = flush;
            answers.push_back (std::move (record));
            return &answers.back();
        };

        add (own.jsonService, mdns::type::ptr, shared, false)->target = own.jsonInstance;
        auto* json = add (own.jsonInstance, mdns::type::srv, unique, true);
        json->port = static_cast<std::uint16_t> (options.ownPort);
        json->target = own.host;
        add (own.jsonInstance, mdns::type::txt, shared, true)->txt = { "txtvers=1" };

        if (options.oscPort > 0)
        {
            add (own.oscService, mdns::type::ptr, shared, false)->target = own.oscInstance;
            auto* osc = add (own.oscInstance, mdns::type::srv, unique, true);
            osc->port = static_cast<std::uint16_t> (options.oscPort);
            osc->target = own.host;
            add (own.oscInstance, mdns::type::txt, shared, true)->txt = { "txtvers=1" };
        }

        sendToGroup (mdns::encodeResponse (answers, addressRecords (unique, false)));

        //  Just said: not said again to a question arriving within the second.
        const auto now = Clock::now();

        for (const auto& each : answers)
            pacer.heard (pacerKey (each), now);
    }

    /*  A QUESTION ABOUT GO.DOT, ANSWERED (ARU, ASA): the PTR of a service it
        offers with the rest as additional records; its instances' SRV and
        TXT; its host's A records; and the list of services for a browser that
        asks what there is. To the group, unless the question came from
        another port or asked for a direct answer. */
    void Discovery::answer (const mdns::Message& asked, const std::string& fromIp, int fromPort)
    {
        std::vector<mdns::Record> answers, additionals;
        const auto services = mdns::nameOf ("_services._dns-sd._udp.local");
        const auto local = fromIp.rfind ("127.", 0) == 0;
        bool wantsAddresses = false;
        bool direct = fromPort != mdns::port;

        const auto record = [] (mdns::Name name, std::uint16_t type, std::uint32_t ttl, bool flush)
        {
            mdns::Record made;
            made.name = std::move (name);
            made.type = type;
            made.ttl = ttl;
            made.cacheFlush = flush;
            return made;
        };

        const auto offer = [&] (const mdns::Name& offered, const mdns::Name& instance, int port, bool asAnswer)
        {
            auto ptr = record (offered, mdns::type::ptr, 4500, false);
            ptr.target = instance;
            auto srv = record (instance, mdns::type::srv, 120, true);
            srv.port = static_cast<std::uint16_t> (port);
            srv.target = own.host;
            auto txt = record (instance, mdns::type::txt, 4500, true);
            txt.txt = { "txtvers=1" };

            if (asAnswer)
                answers.push_back (std::move (ptr));

            additionals.push_back (std::move (srv));
            additionals.push_back (std::move (txt));
            wantsAddresses = true;
        };

        for (const auto& question : asked.questions)
        {
            direct = direct || question.unicastResponse;
            const auto any = question.type == mdns::type::any;

            if ((question.type == mdns::type::ptr || any) && mdns::sameName (question.name, own.jsonService))
                offer (own.jsonService, own.jsonInstance, options.ownPort, true);
            else if ((question.type == mdns::type::ptr || any) && options.oscPort > 0 && mdns::sameName (question.name, own.oscService))
                offer (own.oscService, own.oscInstance, options.oscPort, true);
            else if ((question.type == mdns::type::ptr || any) && mdns::sameName (question.name, services))
            {
                auto json = record (services, mdns::type::ptr, 4500, false);
                json.target = own.jsonService;
                answers.push_back (std::move (json));

                if (options.oscPort > 0)
                {
                    auto osc = record (services, mdns::type::ptr, 4500, false);
                    osc.target = own.oscService;
                    answers.push_back (std::move (osc));
                }
            }
            else if (mdns::sameName (question.name, own.jsonInstance) || mdns::sameName (question.name, own.oscInstance))
            {
                const auto json = mdns::sameName (question.name, own.jsonInstance);
                offer (json ? own.jsonService : own.oscService, question.name, json ? options.ownPort : options.oscPort, false);

                //  The instance's own records are the answer here, not additional ones.
                for (auto& each : additionals)
                    if (mdns::sameName (each.name, question.name))
                        answers.push_back (each);

                std::erase_if (additionals, [&question] (const mdns::Record& each) { return mdns::sameName (each.name, question.name); });
            }
            else if ((question.type == mdns::type::a || any) && mdns::sameName (question.name, own.host))
            {
                for (auto& each : addressRecords (120, local))
                    answers.push_back (std::move (each));
            }
        }

        if (answers.empty())
            return;

        if (wantsAddresses)
            for (auto& each : addressRecords (120, local))
                additionals.push_back (std::move (each));

        if (direct)
        {
            //  Straight back, the question echoed for a querier on another port (§6.7).
            const auto bytes = fromPort != mdns::port ? mdns::encodeResponse (answers, additionals, asked.id, asked.questions)
                                                      : mdns::encodeResponse (answers, additionals);
            socket->write (juce::String (fromIp), fromPort, bytes.data(), static_cast<int> (bytes.size()));
            return;
        }

        /*  To the group: after a short wait chosen at random, unless another
            responder says the same meanwhile, and never the same record twice
            within a second (RFC 6762 §6, §7.4; AnswerPacer.h). */
        std::vector<std::string> keys;

        for (const auto& each : answers)
            keys.push_back (pacerKey (each));

        (void) pacer.want (std::move (keys), mdns::encodeResponse (answers, additionals), Clock::now(),
                           std::chrono::milliseconds (20 + delays.nextInt (101)));
    }

    void Discovery::sendDueAnswers (Clock::time_point now)
    {
        for (const auto& bytes : pacer.due (now))
            sendToGroup (bytes);
    }

    std::string Discovery::preferred (const std::vector<std::string>& addresses) const
    {
        /*  A HOST ON SEVERAL NETWORKS answers with an address on each: the one
            on a network this machine is on is the one it can reach. */
        for (const auto& address : addresses)
            for (const auto& mine : interfaces)
                if (sameNetwork (address, mine))
                    return address;

        return addresses.empty() ? std::string {} : addresses.front();
    }

    void Discovery::ask (const std::vector<mdns::Question>& questions, bool toGroup)
    {
        if (questions.empty() || socket == nullptr)
            return;

        auto asked = questions;

        //  From a port other than 5353, answers come back to it directly (RFC 6762 §5.4, §6.7).
        for (auto& question : asked)
            question.unicastResponse = ! onGroup;

        const auto bytes = mdns::encodeQuery (asked);
        const auto size = static_cast<int> (bytes.size());

        if (toGroup && options.port == mdns::port && pacer.spend (Clock::now()))
        {
            if (onGroup && ! interfaces.empty())
            {
                for (const auto& address : interfaces)
                {
                    multicast::sendFrom (socket->getRawSocketHandle(), address);
                    socket->write (mdns::group, mdns::port, bytes.data(), size);
                }
            }
            else
            {
                socket->write (mdns::group, mdns::port, bytes.data(), size);
            }
        }

        for (const auto& [host, peerPort] : options.peers)
            socket->write (juce::String (host), peerPort, bytes.data(), size);
    }

    void Discovery::run()
    {
        std::vector<std::uint8_t> buffer (9000);
        auto lastSweep = Clock::now();
        auto lastInterfaces = Clock::now();

        while (! stopping.load (std::memory_order_relaxed))
        {
            const auto now = Clock::now();

            if (browseWanted.exchange (false, std::memory_order_relaxed))
            {
                nextQuery = now;
                interval = std::chrono::seconds (1);
            }

            /*  THE QUESTION, again and again less often: one, two, four
                seconds, up to a minute - a responder announces itself when it
                arrives, so the late question is the one for a quiet one. From
                a port of its own the answers last ten seconds at most, so the
                question is put every five. */
            if (now >= nextQuery)
            {
                ask ({ { service, mdns::type::ptr, false } }, true);
                nextQuery = now + (onGroup ? interval : std::chrono::seconds (5));
                interval = std::min<std::chrono::seconds> (interval * 2, std::chrono::seconds (60));
            }

            followUp (now);
            sendDueAnswers (now);

            //  ANNOUNCED TWICE AT THE START, a second apart (RFC 6762 §8.3).
            if (announcing() && onGroup && announcementsLeft > 0 && now >= nextAnnouncement)
            {
                announce (120);
                --announcementsLeft;
                nextAnnouncement = now + std::chrono::seconds (1);
            }

            if (now - lastSweep >= std::chrono::seconds (1))
            {
                sweep (now);
                lastSweep = now;
            }

            if (now - lastInterfaces >= std::chrono::seconds (30))
            {
                openInterfaces();
                lastInterfaces = now;
            }

            //  Not past the moment a waiting answer is due.
            int waitMs = 100;

            if (const auto due = pacer.next())
                waitMs = static_cast<int> (std::clamp<std::int64_t> (
                    std::chrono::duration_cast<std::chrono::milliseconds> (*due - Clock::now()).count() + 1, 1, 100));

            if (socket->waitUntilReady (true, waitMs) != 1)
                continue;

            juce::String fromIp;
            int fromPort = 0;
            const auto got = socket->read (buffer.data(), static_cast<int> (buffer.size()), false, fromIp, fromPort);

            if (got > 0)
                heard (std::vector<std::uint8_t> (buffer.begin(), buffer.begin() + got), fromIp.toStdString(), fromPort);
        }
    }

    void Discovery::heard (const std::vector<std::uint8_t>& bytes, const std::string& fromIp, int fromPort)
    {
        const auto message = mdns::decode (bytes);

        if (! message)
            return;

        //  A QUESTION: Go.dot's to answer, when it announces itself.
        if (! message->response)
        {
            if (announcing())
                answer (*message, fromIp, fromPort);

            return;
        }

        const auto now = Clock::now();
        const auto records = message->records();

        //  What a responder just said to the group, ours looped back included, is said (§7.4).
        if (announcing() && fromPort == mdns::port)
            for (const auto& record : message->answers)
                if (record.ttl > 0)
                    pacer.heard (pacerKey (record), now);

        //  The instances first, so an SRV in the same message finds its own.
        for (const auto& record : records)
        {
            if (record.type != mdns::type::ptr || ! mdns::sameName (record.name, service) || record.target.empty())
                continue;

            const auto key = keyOf (record.target);

            if (record.ttl == 0)
            {
                instances.erase (key);
                continue;
            }

            auto& instance = instances[key];
            instance.name = record.target;
            instance.expires = now + secondsOf (record.ttl);
        }

        for (const auto& record : records)
        {
            if (record.type == mdns::type::srv || record.type == mdns::type::txt)
            {
                const auto match = instances.find (keyOf (record.name));

                if (match == instances.end())
                    continue;

                auto& instance = match->second;

                if (record.type == mdns::type::srv)
                {
                    instance.hasService = record.ttl > 0;
                    instance.host = record.target;
                    instance.port = record.port;
                    instance.serviceExpires = now + secondsOf (record.ttl);
                }
                else
                {
                    instance.txt = record.txt;
                }
            }
            else if (record.type == mdns::type::a)
            {
                const auto key = keyOf (record.name);

                if (record.ttl == 0)
                {
                    hosts.erase (key);
                    continue;
                }

                auto& host = hosts[key];
                const auto address = record.addressText();

                if (std::find (host.addresses.begin(), host.addresses.end(), address) == host.addresses.end())
                    host.addresses.push_back (address);

                host.expires = now + secondsOf (record.ttl);
            }
        }

        publish();
    }

    /*  WHAT IS STILL MISSING, ASKED FOR: an instance with no host or port yet,
        a host with no address - once a second each, together in one message. */
    void Discovery::followUp (Clock::time_point now)
    {
        std::vector<mdns::Question> questions;

        for (auto& [key, instance] : instances)
        {
            if (! instance.hasService)
            {
                if (now - instance.askedAt >= std::chrono::seconds (1))
                {
                    instance.askedAt = now;
                    questions.push_back ({ instance.name, mdns::type::srv, false });
                    questions.push_back ({ instance.name, mdns::type::txt, false });
                }

                continue;
            }

            const auto hostKey = keyOf (instance.host);

            if (const auto known = hosts.find (hostKey); known != hosts.end() && ! known->second.addresses.empty())
                continue;

            auto& host = hosts[hostKey];

            if (now - host.askedAt >= std::chrono::seconds (1))
            {
                host.askedAt = now;
                questions.push_back ({ instance.host, mdns::type::a, false });
            }
        }

        ask (questions, true);
    }

    void Discovery::sweep (Clock::time_point now)
    {
        for (auto at = instances.begin(); at != instances.end();)
        {
            if (at->second.expires <= now)
            {
                at = instances.erase (at);
                continue;
            }

            if (at->second.hasService && at->second.serviceExpires <= now)
                at->second.hasService = false;

            ++at;
        }

        for (auto at = hosts.begin(); at != hosts.end();)
        {
            if (! at->second.addresses.empty() && at->second.expires <= now)
                at = hosts.erase (at);
            else
                ++at;
        }

        publish();
    }

    /*  THE LIST, made again from what is known, and handed over only when it
        differs: complete instances, by name, this Go.dot left out. */
    void Discovery::publish()
    {
        std::vector<Found> now;

        for (const auto& [key, instance] : instances)
        {
            if (! instance.hasService || instance.port <= 0)
                continue;

            const auto host = hosts.find (keyOf (instance.host));

            if (host == hosts.end() || host->second.addresses.empty())
                continue;

            const auto& addresses = host->second.addresses;
            const auto itself = options.ownPort > 0 && instance.port == options.ownPort
                             && std::any_of (addresses.begin(), addresses.end(), [this] (const std::string& address)
                                             {
                                                 return std::find (ownAddresses.begin(), ownAddresses.end(), address)
                                                          != ownAddresses.end();
                                             });

            if (itself)
                continue;

            Found entry;
            entry.name = instance.name.empty() ? key : instance.name.front();
            entry.host = preferred (addresses);
            entry.queryPort = instance.port;
            entry.txt = instance.txt;
            now.push_back (std::move (entry));
        }

        std::sort (now.begin(), now.end(), [] (const Found& a, const Found& b)
                   {
                       return a.name != b.name ? a.name < b.name : a.host < b.host;
                   });

        {
            const std::lock_guard<std::mutex> lock { guard };

            if (linesOf (now) == linesOf (list))
                return;

            list = std::move (now);
        }

        stamp.fetch_add (1, std::memory_order_acq_rel);
    }
}
