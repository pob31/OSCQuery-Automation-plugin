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
    WHEN A MULTICAST ANSWER MAY GO OUT (RFC 6762 §6 and §7.4). Found the hard
    way on 2026-11-10: the responder answered every question it heard, at once,
    to the group. clap-validator loads the plugin in a couple of hundred
    processes within a second, each one announcing the plugin's server and each
    one answering every other's question, and a machine sent 11,600 multicast
    datagrams in that second (docs/bench/2026-11-10-mdns-flood.md). On a phone's
    hotspot, where multicast goes out at the slowest rate, that took the link
    down.

    Three rules, all the RFC's:

      - AN ANSWER WAITS A LITTLE (§6: 20 to 120 ms, chosen at random by the
        caller), so that several responders with the same thing to say do not
        say it in the same instant.
      - WHAT ANOTHER RESPONDER JUST SAID IS NOT SAID AGAIN (§7.4): a record
        heard in somebody's multicast response while ours waits counts as sent.
      - A RECORD IS NOT MULTICAST TWICE WITHIN A SECOND (§6). A querier that
        missed it asks again a second later, as every querier does.

    And one of this library's own, for whatever the three do not foresee: A
    BUDGET of datagrams to the group, ten a second with a burst of twenty,
    spent by answers and questions alike. What is over budget is dropped, and
    mDNS, where everything is repeated, loses nothing by it.

    Pure: no socket and no clock of its own, so the tests drive it with times
    of their choosing. Not thread-safe; the browser's thread owns it.
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oqa::oscquery::mdns
{
    class AnswerPacer
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr std::chrono::milliseconds quiet { 1000 };   ///< a record's rest after it was multicast
        static constexpr std::size_t maxPending = 16;
        static constexpr double budgetPerSecond = 10.0;
        static constexpr double budgetBurst = 20.0;

        /*  An answer wanted: `keys` name its records (one text each, the same
            for the same record whoever sends it), `bytes` are the datagram.
            False when there is nothing to send: every record went out or was
            heard within the last second, or the same answer already waits. */
        bool want (std::vector<std::string> keys, std::vector<std::uint8_t> bytes,
                   Clock::time_point now, std::chrono::milliseconds delay)
        {
            if (keys.empty() || allFresh (keys, now))
                return false;

            std::sort (keys.begin(), keys.end());

            for (const auto& waiting : pending)
                if (waiting.keys == keys)
                    return false;

            if (pending.size() >= maxPending)
                return false;

            pending.push_back ({ std::move (keys), std::move (bytes), now + delay });
            return true;
        }

        /** A record seen in a multicast response, ours or anybody's: it has been said. */
        void heard (const std::string& key, Clock::time_point now)
        {
            said[key] = now;
        }

        /*  The answers whose wait is over, to send now. One whose records were
            all said meanwhile is dropped instead (§7.4). What is returned is
            marked as said. */
        std::vector<std::vector<std::uint8_t>> due (Clock::time_point now)
        {
            std::vector<std::vector<std::uint8_t>> out;

            for (auto at = pending.begin(); at != pending.end();)
            {
                if (at->due > now)
                {
                    ++at;
                    continue;
                }

                if (! allFresh (at->keys, now))
                {
                    for (const auto& key : at->keys)
                        said[key] = now;

                    out.push_back (std::move (at->bytes));
                }

                at = pending.erase (at);
            }

            forget (now);
            return out;
        }

        /** When the next waiting answer is due, if one waits. */
        std::optional<Clock::time_point> next() const
        {
            std::optional<Clock::time_point> soonest;

            for (const auto& waiting : pending)
                if (! soonest.has_value() || waiting.due < *soonest)
                    soonest = waiting.due;

            return soonest;
        }

        std::size_t waiting() const noexcept { return pending.size(); }

        /*  One datagram to the group: true when the budget has it, and it is
            spent; false when the caller should drop the datagram. */
        bool spend (Clock::time_point now)
        {
            if (! budgetStarted)
            {
                budgetStarted = true;
                budgetAt = now;
                budget = budgetBurst;
            }

            const auto elapsed = std::chrono::duration<double> (now - budgetAt).count();

            if (elapsed > 0.0)
            {
                budget = std::min (budgetBurst, budget + elapsed * budgetPerSecond);
                budgetAt = now;
            }

            if (budget < 1.0)
            {
                ++droppedCount;
                return false;
            }

            budget -= 1.0;
            return true;
        }

        /** Datagrams the budget refused since the start. */
        std::uint64_t dropped() const noexcept { return droppedCount; }

    private:
        struct Pending
        {
            std::vector<std::string> keys;
            std::vector<std::uint8_t> bytes;
            Clock::time_point due;
        };

        bool allFresh (const std::vector<std::string>& keys, Clock::time_point now) const
        {
            for (const auto& key : keys)
            {
                const auto at = said.find (key);

                if (at == said.end() || now - at->second >= quiet)
                    return false;
            }

            return true;
        }

        void forget (Clock::time_point now)
        {
            for (auto at = said.begin(); at != said.end();)
                at = now - at->second >= quiet ? said.erase (at) : std::next (at);
        }

        std::vector<Pending> pending;
        std::map<std::string, Clock::time_point> said;
        bool budgetStarted = false;
        Clock::time_point budgetAt {};
        double budget = budgetBurst;
        std::uint64_t droppedCount = 0;
    };
}
