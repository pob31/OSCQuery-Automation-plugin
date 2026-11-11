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
    WHAT A SLOT'S ROW SAYS (devplan Phase 6d-1): the shade and the tooltip's
    words for every row of the PRD's failure table, from the slot's standing
    alone, headless; and Instance::clearStale, which unbinds the missing, the
    retyped and the renamed and nothing else; and the range going back to the
    device's own.
*/

#include <doctest/doctest.h>

#include "RegistryRig.h"
#include "TestSupport.h"

#include <core/Instance.h>
#include <core/registry/Registry.h>
#include <editor/SlotPresentation.h>

#include <string>

using namespace oqa::editor::presentation;
using namespace oqa::core;
using namespace oqa::oscquery;
using oqatest::Rig;
using oqatest::waitUntil;
using oqatest::isRangedFloat;

namespace
{
    const OqaCoreApi* api() { return oqatest::coreApi(); }

    const bool shutdownRegistered = (oqatest::registerShutdownHook ([] { Registry::shared().shutdown(); }), true);

    bool mentions (const std::string& text, const char* needle) { return text.find (needle) != std::string::npos; }

    std::string stateOf (const Instance& instance, int slot)
    {
        const auto status = juce::JSON::parse (juce::String (instance.statusJson()));

        if (const auto* slots = status.getProperty ("slots", {}).getArray())
            for (const auto& s : *slots)
                if (static_cast<int> (s.getProperty ("id", -1)) == slot)
                    return s.getProperty ("state", "").toString().toStdString();

        return {};
    }
}

//==============================================================================
TEST_CASE ("presentation: each row of the PRD's failure table has its shade and its words")
{
    Standing s;
    CHECK (shadeOf (s) == Shade::grey);
    CHECK (mentions (tooltipOf (s), "Unbound"));

    s.bound = true;
    CHECK (shadeOf (s) == Shade::amber);
    CHECK (mentions (tooltipOf (s), "Range unknown"));
    CHECK (mentions (tooltipOf (s), "min and max"));

    s.active = true;
    s.state = "ok";
    CHECK (shadeOf (s) == Shade::green);
    CHECK (tooltipOf (s).empty());

    s.state = "missing";
    s.detail = "the path is not in the tree";
    CHECK (shadeOf (s) == Shade::red);
    CHECK (mentions (tooltipOf (s), "Stale: the path is not in the tree"));
    CHECK (mentions (tooltipOf (s), "keeps its automation"));

    s.state = "renamed";
    s.renamedTo = "/elsewhere";
    CHECK (shadeOf (s) == Shade::red);
    CHECK (mentions (tooltipOf (s), "moved to /elsewhere"));

    s.state = "unauthorised";
    s.renamedTo.clear();
    s.detail = "ACCESS 1: read-only for this client";
    CHECK (shadeOf (s) == Shade::amber);
    CHECK (mentions (tooltipOf (s), "Unauthorised: ACCESS 1"));
    CHECK (mentions (tooltipOf (s), "allow-list"));

    s.state = "ok";
    s.detail.clear();
    s.deviceReachable = false;
    s.deviceLastSeenMs = 65000;
    CHECK (shadeOf (s) == Shade::orangeRed);
    CHECK (mentions (tooltipOf (s), "Unreachable"));
    CHECK (mentions (tooltipOf (s), "65 s ago"));

    s.deviceReachable = true;
    s.waitingForPort = true;
    CHECK (shadeOf (s) == Shade::amber);
    CHECK (mentions (tooltipOf (s), "receive port"));

    s.waitingForPort = false;
    s.loopGuarded = 3;
    s.rateLimited = 7;
    s.datagramsDropped = 2;
    CHECK (shadeOf (s) == Shade::green);
    CHECK (mentions (tooltipOf (s), "3 echoes"));
    CHECK (mentions (tooltipOf (s), "7 held by the rate limiter, 2 datagrams dropped"));

    CHECK (agoWords (-1) == "never");
    CHECK (agoWords (500) == "just now");
    CHECK (agoWords (300000) == "5 min ago");
}

TEST_CASE ("clear stale: the missing, the retyped and the renamed are unbound, the rest kept; a user's range goes back to the device's")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && ! n.unit.empty() && ! n.unit[0].empty(); });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });
    const Node* third = nullptr;

    for (const auto* leaf : rig.tree.leaves())
        if (isRangedFloat (*leaf) && leaf->argumentCount() == 1 && leaf->fullPath != gain.fullPath && leaf->fullPath != pan.fullPath)
            third = leaf;

    REQUIRE (third != nullptr);

    Instance a (api(), 8, "clear-stale");
    std::string why;
    const auto device = a.addDevice (rig.device ("clear-stale"));
    REQUIRE (a.bind (0, gain, 0, device, why));
    REQUIRE (a.bind (1, pan, 0, device, why));
    REQUIRE (a.bind (2, *third, 0, device, why));
    a.connect();
    rig.waitReady (a);
    REQUIRE (waitUntil ([&] { return stateOf (a, 0) == "ok" && stateOf (a, 1) == "ok" && stateOf (a, 2) == "ok"; }));

    CHECK (a.clearStale() == 0);

    rig.waitHeard();
    REQUIRE (rig.mock.remove (juce::String (gain.fullPath)));
    REQUIRE (rig.mock.rename (juce::String (pan.fullPath), juce::String (pan.fullPath + "_moved")));
    REQUIRE (waitUntil ([&] { return stateOf (a, 0) == "missing" && stateOf (a, 1) == "renamed"; }));

    CHECK (a.clearStale() == 2);
    CHECK_FALSE (a.chunk().slots[0].bound());
    CHECK_FALSE (a.chunk().slots[1].bound());
    CHECK (a.chunk().slots[2].bound());
    CHECK (a.clearStale() == 0);

    /*  The range: the user's, then the device's again. */
    const auto& arg = third->argument (0);
    a.setRange (2, -1.0, 1.0);
    CHECK (a.chunk().slots[2].rangeOverridden);
    a.clearRangeOverride (2, *arg.range->min, *arg.range->max);
    CHECK_FALSE (a.chunk().slots[2].rangeOverridden);
    CHECK (*a.chunk().slots[2].rangeMin == doctest::Approx (*arg.range->min));
    CHECK (*a.chunk().slots[2].rangeMax == doctest::Approx (*arg.range->max));
}
