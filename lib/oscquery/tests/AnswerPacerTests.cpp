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
    WHEN A MULTICAST ANSWER MAY GO OUT (oscquery/mdns/AnswerPacer.h; RFC 6762
    §6, §7.4), driven with times of the test's choosing: no socket, no network.
    The last case is the flood of 2026-11-10 in small: two hundred responders
    with the same thing to say, asked at once.
*/

#include <doctest/doctest.h>

#include <oscquery/mdns/AnswerPacer.h>

#include <chrono>
#include <string>
#include <vector>

using oqa::oscquery::mdns::AnswerPacer;
using namespace std::chrono_literals;

namespace
{
    const AnswerPacer::Clock::time_point t0 { 1000s };
    const std::vector<std::uint8_t> datagram { 1, 2, 3 };
    const std::string ptr = "_oscjson._tcp.local|12|plugin._oscjson._tcp.local";
}

TEST_CASE ("answer pacer: an answer waits its delay, then goes out once")
{
    AnswerPacer pacer;

    CHECK (pacer.want ({ ptr }, datagram, t0, 50ms));
    CHECK (pacer.waiting() == 1);
    REQUIRE (pacer.next().has_value());
    CHECK (*pacer.next() == t0 + 50ms);

    CHECK (pacer.due (t0 + 49ms).empty());

    const auto out = pacer.due (t0 + 50ms);
    REQUIRE (out.size() == 1);
    CHECK (out.front() == datagram);
    CHECK (pacer.waiting() == 0);
    CHECK_FALSE (pacer.next().has_value());
    CHECK (pacer.due (t0 + 60ms).empty());
}

TEST_CASE ("answer pacer: a record is not multicast twice within a second, and is again after it")
{
    AnswerPacer pacer;

    REQUIRE (pacer.want ({ ptr }, datagram, t0, 20ms));
    REQUIRE (pacer.due (t0 + 20ms).size() == 1);

    //  Asked again and again within the second: nothing more to say.
    CHECK_FALSE (pacer.want ({ ptr }, datagram, t0 + 100ms, 20ms));
    CHECK_FALSE (pacer.want ({ ptr }, datagram, t0 + 900ms, 20ms));
    CHECK (pacer.waiting() == 0);

    //  A second after it went out, it may go out again.
    CHECK (pacer.want ({ ptr }, datagram, t0 + 1020ms, 20ms));
    CHECK (pacer.due (t0 + 1040ms).size() == 1);
}

TEST_CASE ("answer pacer: the same answer wanted while it waits is one answer")
{
    AnswerPacer pacer;

    CHECK (pacer.want ({ "a", "b" }, datagram, t0, 100ms));
    CHECK_FALSE (pacer.want ({ "b", "a" }, datagram, t0 + 10ms, 100ms));      // the same records, in any order
    CHECK (pacer.want ({ "a" }, datagram, t0 + 10ms, 100ms));                  // another answer
    CHECK (pacer.waiting() == 2);
    CHECK_FALSE (pacer.want ({}, datagram, t0, 0ms));                          // nothing to say is nothing to send
}

TEST_CASE ("answer pacer: what another responder said while ours waited is not said again")
{
    AnswerPacer pacer;

    REQUIRE (pacer.want ({ ptr }, datagram, t0, 100ms));
    pacer.heard (ptr, t0 + 40ms);                       // somebody was quicker
    CHECK (pacer.due (t0 + 100ms).empty());
    CHECK (pacer.waiting() == 0);

    //  And for the rest of that second it is not wanted at all.
    CHECK_FALSE (pacer.want ({ ptr }, datagram, t0 + 500ms, 20ms));

    //  An answer of two records, one of them heard: the other still has to be said.
    REQUIRE (pacer.want ({ ptr, "host.local|1|" }, datagram, t0 + 600ms, 50ms));
    CHECK (pacer.due (t0 + 650ms).size() == 1);
}

TEST_CASE ("answer pacer: no more answers wait than sixteen")
{
    AnswerPacer pacer;

    for (int i = 0; i < 40; ++i)
        (void) pacer.want ({ "record " + std::to_string (i) }, datagram, t0, 50ms);

    CHECK (pacer.waiting() == AnswerPacer::maxPending);
}

TEST_CASE ("answer pacer: the budget lets a burst of twenty through, then ten a second")
{
    AnswerPacer pacer;
    int sent = 0;

    for (int i = 0; i < 1000; ++i)
        sent += pacer.spend (t0) ? 1 : 0;

    CHECK (sent == 20);
    CHECK (pacer.dropped() == 980);

    //  Half a second later, five more; a long silence later, twenty and no more.
    sent = 0;

    for (int i = 0; i < 100; ++i)
        sent += pacer.spend (t0 + 500ms) ? 1 : 0;

    CHECK (sent == 5);

    sent = 0;

    for (int i = 0; i < 100; ++i)
        sent += pacer.spend (t0 + 60s) ? 1 : 0;

    CHECK (sent == 20);
}

TEST_CASE ("answer pacer: two hundred responders asked at once say it once, not two hundred times")
{
    /*  Every responder hears the question at t0 and picks a delay between 20
        and 120 ms; every datagram sent is heard by all of them at once (one
        machine, the multicast looped back). What 2026-11-10 measured without
        this: every one of them answering every question. */
    constexpr int responders = 200;
    std::vector<AnswerPacer> pacers (responders);

    for (int i = 0; i < responders; ++i)
        REQUIRE (pacers[static_cast<std::size_t> (i)].want ({ ptr }, datagram, t0, std::chrono::milliseconds (20 + (i * 37) % 101)));

    int datagrams = 0;

    for (int ms = 0; ms <= 200; ++ms)
    {
        const auto now = t0 + std::chrono::milliseconds (ms);

        for (auto& pacer : pacers)
        {
            const auto out = pacer.due (now);

            if (out.empty())
                continue;

            ++datagrams;

            for (auto& listener : pacers)
                listener.heard (ptr, now);
        }
    }

    CHECK (datagrams == 1);

    //  Asked again within the second, by another newcomer: silence.
    for (auto& pacer : pacers)
        CHECK_FALSE (pacer.want ({ ptr }, datagram, t0 + 400ms, 50ms));
}
