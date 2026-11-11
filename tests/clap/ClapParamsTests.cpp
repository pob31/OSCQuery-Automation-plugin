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
    THE PARAMS EXTENSION, 5b-2, against the mock device: hidden while
    unbound, name and module and text from the binding, the rescan on a
    state load, events reaching the device from process and from flush, and
    the clear rule exactly as decided (missing then rebound elsewhere clears;
    a rename rebind, an unbind, a live rebind do not).

    The plugin is bound by loading a chunk that names the mock (the way a
    host restores a session); what it does is read from the host's records,
    from the mock's socket and from the plugin's own status extension.
*/

#include <doctest/doctest.h>

#include "TestHost.h"
#include "TestSupport.h"
#include "core/RegistryRig.h"
#include "support/HttpGet.h"

#include <core/StateChunk.h>
#include <shims/clap/StatusExtension.h>

#include <string>

using namespace oqatest::claphost;
using oqatest::Rig;
using oqatest::waitUntil;
using oqatest::bundlesReceived;
using oqatest::messagesIn;
using oqatest::isRangedFloat;
using oqa::oscquery::Node;

namespace
{
    constexpr const char* eightSlots = "io.github.pob31.oqa.Q008";

    /** A chunk for 8 slots naming the rig's mock, with `bind (chunk)` applied. */
    template <typename Bind>
    std::string chunkWith (Rig& rig, const char* deviceName, Bind&& bind)
    {
        oqa::core::StateChunk chunk (8);
        auto ref = rig.device (deviceName);
        ref.treeHash = rig.tree.treeHash();
        chunk.devices.push_back (ref);
        bind (chunk);
        return chunk.toJson();
    }

    bool loadChunk (PluginHandle& plugin, const std::string& json)
    {
        const auto* state = plugin.extension<clap_plugin_state_t> (CLAP_EXT_STATE);
        REQUIRE (state != nullptr);
        StringIStream in (json);
        return state->load (plugin.get(), &in.stream);
    }

    /** The plugin's status, through the extension of ours; the slot's standing word, or "" when unlisted. */
    std::string slotState (PluginHandle& plugin, int slot)
    {
        const auto* ext = plugin.extension<oqa_clap_status_t> (OQA_CLAP_EXT_STATUS);
        REQUIRE (ext != nullptr);
        const auto status = juce::JSON::parse (juce::String (ext->status_json (plugin.get())));

        if (const auto* slots = status.getProperty ("slots", {}).getArray())
            for (const auto& s : *slots)
                if (static_cast<int> (s.getProperty ("id", -1)) == slot)
                    return s.getProperty ("state", "").toString().toStdString();

        return {};
    }

    /*  Tick the host's timers until the slot's standing reads `state`, then
        through one more slow tick, so the plugin has recorded that standing
        itself (the clear rule reads its own record, refreshed every 250 ms). */
    bool waitSlotState (PluginHandle& plugin, int slot, const char* state)
    {
        const bool reached = waitUntil ([&]
        {
            plugin.host.runTimers (plugin.get());
            return slotState (plugin, slot) == state;
        });

        for (int i = 0; i < 5 && reached; ++i)
            plugin.host.runTimers (plugin.get());

        if (! reached)
        {
            const auto* ext = plugin.extension<oqa_clap_status_t> (OQA_CLAP_EXT_STATUS);
            MESSAGE ("slot " << slot << " never read \"" << state << "\"; status: " << ext->status_json (plugin.get()));
        }

        return reached;
    }

    clap_param_info_t infoOf (PluginHandle& plugin, uint32_t index)
    {
        const auto* params = plugin.extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);
        REQUIRE (params != nullptr);
        clap_param_info_t info {};
        REQUIRE (params->get_info (plugin.get(), index, &info));
        return info;
    }

    struct Ready
    {
        RecordingHost host;
        PluginHandle plugin;

        explicit Ready (const char* id = eightSlots) : plugin (host, id)
        {
            REQUIRE (plugin);
            REQUIRE (plugin->init (plugin.get()));
            REQUIRE (plugin->activate (plugin.get(), 48000.0, 1, 64));
            RecordingHost::AudioThreadScope audio (host);
            REQUIRE (plugin->start_processing (plugin.get()));
        }

        ~Ready()
        {
            {
                RecordingHost::AudioThreadScope audio (host);
                plugin->stop_processing (plugin.get());
            }
            plugin->deactivate (plugin.get());
        }
    };
}

//==============================================================================
TEST_CASE ("clap params: every slot is a 0-1 parameter, hidden and (unbound) until bound; a bound one shows its name, module and device units")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && ! n.unit.empty() && ! n.unit[0].empty(); });

    Ready r;
    const auto* params = r.plugin.extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);
    REQUIRE (params != nullptr);
    CHECK (params->count (r.plugin.get()) == 8);

    for (uint32_t i = 0; i < 8; ++i)
    {
        const auto info = infoOf (r.plugin, i);
        CHECK (info.id == i);
        CHECK ((info.flags & CLAP_PARAM_IS_HIDDEN) != 0);
        CHECK ((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
        CHECK ((info.flags & CLAP_PARAM_IS_STEPPED) == 0);
        CHECK (std::string (info.name) == "(unbound)");
        CHECK (info.min_value == 0.0);
        CHECK (info.max_value == 1.0);
    }

    /*  Text for an unbound slot is the value itself; it reads back. */
    char text[64] = {};
    REQUIRE (params->value_to_text (r.plugin.get(), 2, 0.25, text, sizeof (text)));
    double back = -1;
    REQUIRE (params->text_to_value (r.plugin.get(), 2, text, &back));
    CHECK (back == doctest::Approx (0.25));

    r.host.forget();
    std::string why;
    REQUIRE (loadChunk (r.plugin, chunkWith (rig, "params-info", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[2].bind (gain, 0, 0, why)); })));

    /*  The load told the host at once: info (name, module, hidden) and values. */
    CHECK ((r.host.rescanFlags() & CLAP_PARAM_RESCAN_INFO) != 0);
    CHECK ((r.host.rescanFlags() & CLAP_PARAM_RESCAN_VALUES) != 0);

    const auto bound = infoOf (r.plugin, 2);
    CHECK ((bound.flags & CLAP_PARAM_IS_HIDDEN) == 0);
    CHECK (std::string (bound.name) == gain.fullPath.substr (1));

    const auto container = gain.fullPath.substr (0, gain.fullPath.rfind ('/'));
    CHECK (std::string (bound.module) == "params-info" + container);
    CHECK ((infoOf (r.plugin, 3).flags & CLAP_PARAM_IS_HIDDEN) != 0);

    /*  Device units, the unit after, and back through the same scaling. */
    const auto& arg = gain.argument (0);
    const auto expectedUnits = *arg.range->min + 0.5 * (*arg.range->max - *arg.range->min);
    REQUIRE (params->value_to_text (r.plugin.get(), 2, 0.5, text, sizeof (text)));
    const juce::String shown (text);
    CHECK (shown.endsWith (juce::String (gain.unit[0])));
    CHECK (shown.getDoubleValue() == doctest::Approx (expectedUnits).epsilon (0.001));
    REQUIRE (params->text_to_value (r.plugin.get(), 2, text, &back));
    CHECK (back == doctest::Approx (0.5).epsilon (0.001));
}

TEST_CASE ("clap params: a parameter event reaches the device from process, and from flush while inactive; the wrong event space is ignored")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1; });

    Ready r;
    std::string why;
    REQUIRE (loadChunk (r.plugin, chunkWith (rig, "params-events", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[0].bind (gain, 0, 0, why)); })));
    REQUIRE (waitSlotState (r.plugin, 0, "ok"));

    /*  The first block posts everything bound once the plan is live; then
        a move. */
    ProcessRig block;
    REQUIRE (waitUntil ([&] { block.run (r.plugin); return messagesIn (bundlesReceived (rig.mock)) >= 1; }));
    rig.mock.clearReceivedOsc();

    block.in.paramValue (0, 0.75, 17);
    CHECK (block.run (r.plugin) == CLAP_PROCESS_CONTINUE);
    block.in.clear();
    REQUIRE (waitUntil ([&] { return messagesIn (bundlesReceived (rig.mock)) >= 1; }));

    const auto* params = r.plugin.extension<clap_plugin_params_t> (CLAP_EXT_PARAMS);
    double value = 0;
    REQUIRE (params->get_value (r.plugin.get(), 0, &value));
    CHECK (value == doctest::Approx (0.75));

    /*  An event in another space is not ours. */
    rig.mock.clearReceivedOsc();
    block.in.paramValue (0, 0.1);
    block.in.events.back().header.space_id = 1234;
    block.run (r.plugin);
    block.in.clear();
    REQUIRE (params->get_value (r.plugin.get(), 0, &value));
    CHECK (value == doctest::Approx (0.75));

    /*  Inactive: the host flushes instead of processing, and the device
        still hears the move. */
    {
        RecordingHost::AudioThreadScope audio (r.host);
        r.plugin->stop_processing (r.plugin.get());
    }
    r.plugin->deactivate (r.plugin.get());

    InEvents in;
    OutEvents out;
    in.paramValue (0, 0.25);
    params->flush (r.plugin.get(), &in.list, &out.list);
    REQUIRE (params->get_value (r.plugin.get(), 0, &value));
    CHECK (value == doctest::Approx (0.25));

    REQUIRE (r.plugin->activate (r.plugin.get(), 48000.0, 1, 64));
    REQUIRE (waitSlotState (r.plugin, 0, "ok"));
    {
        RecordingHost::AudioThreadScope audio (r.host);
        REQUIRE (r.plugin->start_processing (r.plugin.get()));
    }
}

TEST_CASE ("clap params: the clear rule: a missing slot rebound elsewhere clears its lane; a rename rebind, an unbind and a live rebind do not")
{
    Rig rig;
    const auto& gain = rig.leafWhere ([] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && ! n.unit.empty() && ! n.unit[0].empty(); });
    const auto& pan  = rig.leafWhere ([&] (const Node& n) { return isRangedFloat (n) && n.argumentCount() == 1 && n.fullPath != gain.fullPath; });

    Ready r;
    std::string why;

    /*  One device name per subcase: the registry is process-wide and keeps a
        session device at the address it first learnt until pointed, so a
        name reused with a fresh mock on another port would never be reached. */
    SUBCASE ("missing, then bound to another path: cleared")
    {
        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-missing", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[1].bind (gain, 0, 0, why)); })));
        REQUIRE (waitSlotState (r.plugin, 1, "ok"));

        rig.waitHeard();
        REQUIRE (rig.mock.remove (juce::String (gain.fullPath)));
        REQUIRE (waitSlotState (r.plugin, 1, "missing"));
        r.host.forget();

        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-missing", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[1].bind (pan, 0, 0, why)); })));
        REQUIRE (r.host.clears.size() == 1);
        CHECK (r.host.clears[0].first == 1);
        CHECK (r.host.clears[0].second == CLAP_PARAM_CLEAR_AUTOMATIONS);
    }

    SUBCASE ("renamed, then bound to where it went: kept")
    {
        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-renamed", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[1].bind (gain, 0, 0, why)); })));
        REQUIRE (waitSlotState (r.plugin, 1, "ok"));

        const auto moved = gain.fullPath + "_moved";
        rig.waitHeard();
        REQUIRE (rig.mock.rename (juce::String (gain.fullPath), juce::String (moved)));
        REQUIRE (waitSlotState (r.plugin, 1, "renamed"));
        r.host.forget();

        /*  The renamed leaf, as the device now serves it. */
        std::string error;
        const auto reply = oqatest::get (rig.mock.httpPort(), moved);
        REQUIRE (reply.status == 200);
        auto renamed = oqa::oscquery::Tree::parseNode (juce::JSON::parse (juce::String (reply.body)), moved, error);
        REQUIRE_MESSAGE (renamed.has_value(), error);

        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-renamed", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[1].bind (*renamed, 0, 0, why)); })));
        CHECK (r.host.clears.empty());
        REQUIRE (waitSlotState (r.plugin, 1, "ok"));
    }

    SUBCASE ("missing, then unbound: kept; live, then rebound: kept")
    {
        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-unbind", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[1].bind (gain, 0, 0, why)); })));
        REQUIRE (waitSlotState (r.plugin, 1, "ok"));
        r.host.forget();

        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-unbind", [&] (oqa::core::StateChunk& c) { REQUIRE (c.slots[1].bind (pan, 0, 0, why)); })));
        CHECK (r.host.clears.empty());
        REQUIRE (waitSlotState (r.plugin, 1, "ok"));

        rig.waitHeard();
        REQUIRE (rig.mock.remove (juce::String (pan.fullPath)));
        REQUIRE (waitSlotState (r.plugin, 1, "missing"));

        REQUIRE (loadChunk (r.plugin, chunkWith (rig, "clear-rule-unbind", [] (oqa::core::StateChunk&) {})));
        CHECK (r.host.clears.empty());
        CHECK ((infoOf (r.plugin, 1).flags & CLAP_PARAM_IS_HIDDEN) != 0);
    }
}
