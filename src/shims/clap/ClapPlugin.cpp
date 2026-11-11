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

#include "ClapPlugin.h"

#include <core/rt/RtCheck.h>

#include <algorithm>
#include <cstring>

namespace oqa::clapshim
{
    namespace
    {
        std::string instanceLabel (const clap_plugin_descriptor_t* descriptor)
        {
            static std::atomic<int> counter { 0 };
            return std::string (descriptor->name) + " (CLAP) #" + std::to_string (++counter);
        }
    }

    ClapPlugin& ClapPlugin::self (const clap_plugin_t* plugin) noexcept
    {
        return static_cast<ClapPlugin&> (PluginBase::from (plugin));
    }

    ClapPlugin::ClapPlugin (const clap_plugin_descriptor_t* descriptor, const clap_host_t* host, int slotCount)
        : PluginBase (descriptor, host),
          loaded (core::RegistryLoader::load()),
          count (slotCount),
          core (loaded.api, slotCount, instanceLabel (descriptor)),
          values (new std::atomic<float>[static_cast<std::size_t> (slotCount)]),
          scratch (static_cast<std::size_t> (slotCount), 0.0f),
          standing (static_cast<std::size_t> (slotCount)),
          gestures (slotCount),
          endPending (new std::atomic<bool>[static_cast<std::size_t> (slotCount)]),
          statusExtension {
              [] (const clap_plugin_t* p) -> const char* { auto& me = self (p); me.statusText = me.core.statusJson(); return me.statusText.c_str(); },
              [] (const clap_plugin_t* p) -> const char* { auto& me = self (p); me.settingsText = me.core.settingsJson(); return me.settingsText.c_str(); },
              [] (const clap_plugin_t* p) -> const char* { auto& me = self (p); me.processText = me.core.processStatusJson(); return me.processText.c_str(); },
          }
    {
        for (int i = 0; i < slotCount; ++i)
        {
            values[static_cast<std::size_t> (i)].store (0.0f, std::memory_order_relaxed);
            endPending[static_cast<std::size_t> (i)].store (false, std::memory_order_relaxed);
        }

        /*  Record: the registry wakes us once per message a device streams,
            on a thread of its own; the host is asked to flush, and the values
            are drained into out-events there or in the next process. */
        core.setReceiveWake ([this]
        {
            if (_host.canUseParams() && ! isProcessing())
                _host.paramsRequestFlush();
        });

        /*  Commands from the plugin's own server (6b-3): the host is asked for
            its main thread; the 250 ms tick is the fallback. */
        core.setCommandWake ([this] { _host.requestCallback(); });
    }

    void ClapPlugin::onMainThread() noexcept
    {
        applyCommands();
    }

    void ClapPlugin::applyCommands() noexcept
    {
        if (core.pollCommands() > 0)
            bindingsChanged();
    }

    ClapPlugin::~ClapPlugin()
    {
        stopJuce();

        if (timerId != CLAP_INVALID_ID && _host.canUseTimerSupport())
            _host.timerSupportUnregister (timerId);

        core.disconnect();
    }

    //==============================================================================
    bool ClapPlugin::init() noexcept
    {
        startJuce();
        core.connect();
        recordStanding();
        refreshSettings();
        shownRevision = core.revision();

        if (_host.canUseTimerSupport())
            if (! _host.timerSupportRegister (timerPeriodMs, &timerId))
                timerId = CLAP_INVALID_ID;

        return true;
    }

    bool ClapPlugin::activate (double, std::uint32_t, std::uint32_t) noexcept
    {
        core.connect();
        core.setActive (true);
        return true;
    }

    void ClapPlugin::deactivate() noexcept
    {
        /*  A gesture a device left open ends: the host hears the END at the
            next flush, which it is asked for. Not processing here, so the
            tracker is ours to touch. */
        if (gestures.closeAll ([] (int slot, void* context) { static_cast<ClapPlugin*> (context)->endPending[static_cast<std::size_t> (slot)].store (true, std::memory_order_relaxed); }, this) > 0)
        {
            openGestures.store (0, std::memory_order_relaxed);

            if (_host.canUseParams())
                _host.paramsRequestFlush();
        }

        /*  Registered for the plugin's life (Phase 6): the instance stays in
            the registry, inactive, so the server's paths and the Monitor's row
            stay; destroy() disconnects. */
        core.setActive (false);
    }

    const void* ClapPlugin::extension (const char* id) noexcept
    {
        if (id != nullptr && std::strcmp (id, OQA_CLAP_EXT_STATUS) == 0)
            return &statusExtension;

        return nullptr;
    }

    //==============================================================================
    void ClapPlugin::applyEvents (const clap_input_events_t* in) noexcept
    {
        if (in == nullptr)
            return;

        const auto n = in->size (in);

        for (std::uint32_t i = 0; i < n; ++i)
        {
            const auto* header = in->get (in, i);

            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE)
                continue;

            const auto* event = reinterpret_cast<const clap_event_param_value_t*> (header);

            if (event->param_id < static_cast<clap_id> (count))
                values[event->param_id].store (static_cast<float> (std::clamp (event->value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void ClapPlugin::postValues() noexcept
    {
        for (int i = 0; i < count; ++i)
            scratch[static_cast<std::size_t> (i)] = values[static_cast<std::size_t> (i)].load (std::memory_order_relaxed);

        core.processBlock (scratch.data(), count);
    }

    clap_process_status ClapPlugin::process (const clap_process_t* process) noexcept
    {
        const core::rt::ScopedRealtimeCheck region;

        //  The transport, for the one send of every slot when it starts (Instance::processBlock).
        core.setPlaying (process->transport != nullptr && (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0);

        applyEvents (process->in_events);
        drainReceived (process->out_events);

        /*  Audio: nothing. Where the host gave output buffers of its own,
            the input is copied over; in place, there is nothing to do. */
        if (process->audio_inputs_count > 0 && process->audio_outputs_count > 0)
        {
            const auto& in = process->audio_inputs[0];
            auto& out = process->audio_outputs[0];
            const auto channels = std::min (in.channel_count, out.channel_count);

            for (std::uint32_t c = 0; c < channels; ++c)
            {
                if (in.data32 == nullptr || out.data32 == nullptr || in.data32[c] == nullptr || out.data32[c] == nullptr)
                    continue;

                if (in.data32[c] != out.data32[c])
                    std::memcpy (out.data32[c], in.data32[c], sizeof (float) * process->frames_count);
            }
        }

        postValues();
        return CLAP_PROCESS_CONTINUE;
    }

    //==============================================================================
    std::uint32_t ClapPlugin::audioPortsCount (bool) const noexcept
    {
        return 1;
    }

    bool ClapPlugin::audioPortsInfo (std::uint32_t index, bool isInput, clap_audio_port_info_t* info) const noexcept
    {
        if (index != 0)
            return false;

        info->id = 0;
        std::snprintf (info->name, sizeof (info->name), "%s", isInput ? "Input" : "Output");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = 0;
        return true;
    }

    //==============================================================================
    bool ClapPlugin::stateSave (const clap_ostream_t* stream) noexcept
    {
        /*  The chunk, the parameters' values in it (Phase 5b: a CLAP keeps
            its own). Written whole, in as many calls as the stream takes. */
        for (int i = 0; i < count; ++i)
            scratch[static_cast<std::size_t> (i)] = values[static_cast<std::size_t> (i)].load (std::memory_order_relaxed);

        core.setValues (scratch.data(), count);
        const auto json = core.saveState();

        std::size_t written = 0;

        while (written < json.size())
        {
            const auto n = stream->write (stream, json.data() + written, json.size() - written);

            if (n <= 0)
                return false;

            written += static_cast<std::size_t> (n);
        }

        return true;
    }

    bool ClapPlugin::stateLoad (const clap_istream_t* stream) noexcept
    {
        std::string json;
        char buffer[4096];

        for (;;)
        {
            const auto n = stream->read (stream, buffer, sizeof (buffer));

            if (n < 0)
                return false;

            if (n == 0)
                break;

            json.append (buffer, static_cast<std::size_t> (n));
        }

        std::string error;

        if (! core.loadState (json, error))
            return false;

        const auto& saved = core.chunk().values;

        for (std::size_t i = 0; i < static_cast<std::size_t> (count); ++i)
            values[i].store (i < saved.size() ? saved[i] : 0.0f, std::memory_order_relaxed);

        /*  Names, modules and hidden flags follow the bindings; the values
            changed too, so the host reads them again. */
        bindingsChanged (CLAP_PARAM_RESCAN_VALUES);
        return true;
    }

    //==============================================================================
    void ClapPlugin::onTimer (clap_id) noexcept
    {
        /*  A gesture open while the host is not processing closes only in a
            flush, so one is asked for until it has. */
        if (openGestures.load (std::memory_order_relaxed) > 0 && ! isProcessing() && _host.canUseParams())
            _host.paramsRequestFlush();

      #if defined (__linux__)
        pumpJuceIfUnwatched();
      #endif

        if (++ticks % slowEveryTicks != 0)
            return;

        refreshSettings();
        applyCommands();

        /*  A binding made by anything but a state load (the editor, Phase
            5b-4; a future headless path) renames too. The clear rule reads
            the standing recorded at the previous look, so the record is
            refreshed after. */
        if (core.revision() != shownRevision)
            bindingsChanged();

        recordStanding();
    }
}
