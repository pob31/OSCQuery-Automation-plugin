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

#include "PluginProcessor.h"

#include <core/rt/RtCheck.h>
#include "EditorWindow.h"

// The five plugin IDs are five slot counts (PRD, Binding model); the Monitor is a sixth with
// none (Phase 6c). Phase 3 builds one slot count.
static_assert (OQA_SLOT_COUNT == 8 || OQA_SLOT_COUNT == 16 || OQA_SLOT_COUNT == 32
                   || OQA_SLOT_COUNT == 64 || OQA_SLOT_COUNT == 128 || OQA_SLOT_COUNT == 0,
               "OQA_SLOT_COUNT must be one of the five variants, or 0 for the Monitor");

namespace oqa::shim
{
    namespace
    {
        std::string instanceLabel()
        {
            static std::atomic<int> counter { 0 };
            return std::string (JucePlugin_Name) + " #" + std::to_string (++counter);
        }
    }

    PluginProcessor::PluginProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                    .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          loaded (core::RegistryLoader::load()),
          core (loaded.api, slotCount, instanceLabel())
    {
        slots.reserve (static_cast<std::size_t> (slotCount));

        for (int i = 0; i < slotCount; ++i)
        {
            auto parameter = std::make_unique<SlotParameter> (i);
            slots.push_back (parameter.get());
            addParameter (parameter.release());
        }

        bindingsChanged();
        refreshSettings();

        /*  Record: the registry wakes us once per message a device streams;
            the values are pushed to the host on the message thread. */
        core.setReceiveWake ([this] { triggerAsyncUpdate(); });
        core.setCommandWake ([this] { commands.triggerAsyncUpdate(); });

        /*  Registered for the plugin's life (Phase 6): the plugin's own server
            and the Monitor keep this instance's row whether or not the host
            runs its audio; prepareToPlay connects again, which is safe. */
        core.connect();

        /*  Every 50 ms: gestures idle past the timeout are closed. Every fifth
            tick, a slow look at the core's revision, so a binding made by
            anything but this editor (a reload, a future headless path)
            renames too, and at the settings, for the timeout. */
        startTimer (50);
    }

    PluginProcessor::~PluginProcessor()
    {
        stopTimer();
        cancelPendingUpdate();
        commands.cancelPendingUpdate();
        closeGestures();
        core.disconnect();
    }

    const juce::String PluginProcessor::getName() const
    {
        return JucePlugin_Name;
    }

    bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
    {
        // Pass-through: whatever comes in goes out, so the two main buses must
        // match, and must not be disabled.
        const auto& in = layouts.getMainInputChannelSet();
        const auto& out = layouts.getMainOutputChannelSet();
        return ! out.isDisabled() && in == out;
    }

    //==============================================================================
    void PluginProcessor::prepareToPlay (double, int)
    {
        core.connect();
        core.setActive (true);
    }

    void PluginProcessor::releaseResources()
    {
        closeGestures();
        core.setActive (false);
    }

    void PluginProcessor::processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&)
    {
        /*  Audio: nothing. Input and output share the buffer and have the same
            layout, so leaving it alone is the pass-through. */
        const core::rt::ScopedRealtimeCheck region;

        for (std::size_t i = 0; i < slots.size(); ++i)
            values[i] = slots[i]->getValue();

        /*  The transport, where the host says: starting, every slot is told
            to its device once (Instance::processBlock). A host with no play
            head simply never starts. */
        bool playing = false;

        if (auto* head = getPlayHead())
            if (const auto position = head->getPosition())
                playing = position->getIsPlaying();

        core.setPlaying (playing);
        core.processBlock (values.data(), slotCount);
    }

    //==============================================================================
    bool PluginProcessor::hasEditor() const { return true; }

    juce::AudioProcessorEditor* PluginProcessor::createEditor()
    {
        const auto coreNote = loaded.shared ? "Shared core library: " + juce::String (loaded.path) : juce::String (loaded.note);

        /*  The Monitor (zero slots) opens the registry's view; a slot plugin
            its editor. */
        if constexpr (slotCount == 0)
        {
            using Root = editor::MonitorRoot;
            return new EditorWindow (*this, std::make_unique<Root> (editor::MonitorHost { core, coreNote }),
                                     { Root::defaultWidth, Root::defaultHeight, Root::minimumWidth, Root::minimumHeight, Root::maximumWidth, Root::maximumHeight });
        }
        else
        {
            using Root = editor::EditorRoot;
            editor::Host host {
                core,
                [this] { bindingsChanged(); },
                [this] (int slot) { return slot >= 0 && slot < slotCount ? slots[static_cast<std::size_t> (slot)]->getValue() : 0.0f; },
                coreNote
            };

            return new EditorWindow (*this, std::make_unique<Root> (std::move (host)),
                                     { Root::defaultWidth, Root::defaultHeight, Root::minimumWidth, Root::minimumHeight, Root::maximumWidth, Root::maximumHeight });
        }
    }

    bool PluginProcessor::acceptsMidi() const { return false; }
    bool PluginProcessor::producesMidi() const { return false; }
    double PluginProcessor::getTailLengthSeconds() const { return 0.0; }

    int PluginProcessor::getNumPrograms() { return 1; }
    int PluginProcessor::getCurrentProgram() { return 0; }
    void PluginProcessor::setCurrentProgram (int) {}
    const juce::String PluginProcessor::getProgramName (int) { return {}; }
    void PluginProcessor::changeProgramName (int, const juce::String&) {}

    //==============================================================================
    void PluginProcessor::getStateInformation (juce::MemoryBlock& destination)
    {
        /*  The parameters' values go into the chunk too (Phase 5b), so the
            same bytes restore them in the CLAP, which has no host to keep
            them for it. */
        std::array<float, slotCount> current {};

        for (std::size_t i = 0; i < slots.size(); ++i)
            current[i] = slots[i]->getValue();

        core.setValues (current.data(), slotCount);

        const auto json = core.saveState();
        destination.replaceAll (json.data(), json.size());
    }

    void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
    {
        if (data == nullptr || sizeInBytes <= 0)
            return;

        std::string error;
        core.loadState (std::string_view (static_cast<const char*> (data), static_cast<std::size_t> (sizeInBytes)), error);

        const auto& saved = core.chunk().values;

        for (std::size_t i = 0; i < saved.size() && i < slots.size(); ++i)
            slots[i]->setValueNotifyingHost (saved[i]);

        bindingsChanged();
    }

    //==============================================================================
    void PluginProcessor::bindingsChanged()
    {
        const auto& chunk = core.chunk();

        for (auto* parameter : slots)
            parameter->follow (chunk.slots[parameter->slotIndex()]);

        shownRevision = core.revision();
        updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
    }

    void PluginProcessor::timerCallback()
    {
        /*  A gesture a device opened closes when the device goes quiet. */
        gestures.expire (juce::Time::getMillisecondCounterHiRes(), gestureTimeoutMs,
                         [this] (int slot) { slots[static_cast<std::size_t> (slot)]->endChangeGesture(); });

        if (++ticksSinceSlowWork < 5)
            return;

        ticksSinceSlowWork = 0;

        if (core.revision() != shownRevision)
            bindingsChanged();

        refreshSettings();
    }

    //==============================================================================
    void PluginProcessor::handleAsyncUpdate()
    {
        /*  The message thread. Each value the registry holds for one of our
            receiving slots: the gesture first, when this is its first value,
            then the value, as a hand on the fader would. */
        const auto now = juce::Time::getMillisecondCounterHiRes();

        /*  What each parameter reads this instant: a move of the host's that
            the audio thread has not seen yet must not be written over by a
            value from the device (Instance::pollReceived). */
        for (std::size_t i = 0; i < slots.size(); ++i)
            hostNow[i] = slots[i]->getValue();

        core.pollReceived (hostNow.data(), static_cast<int> (hostNow.size()), [this, now] (int slot, float normalised)
        {
            auto* parameter = slots[static_cast<std::size_t> (slot)];

            if (gestures.received (slot, now))
                parameter->beginChangeGesture();

            parameter->setValueNotifyingHost (normalised);
        });
    }

    void PluginProcessor::applyCommands()
    {
        if (core.pollCommands() > 0)
            bindingsChanged();
    }

    void PluginProcessor::closeGestures()
    {
        gestures.closeAll ([this] (int slot) { slots[static_cast<std::size_t> (slot)]->endChangeGesture(); });
    }

    void PluginProcessor::refreshSettings()
    {
        const auto settings = juce::JSON::parse (juce::String (core.settingsJson()));

        if (settings.hasProperty ("gesture_timeout_ms"))
            gestureTimeoutMs = juce::jlimit (10.0, 60000.0, static_cast<double> (settings["gesture_timeout_ms"]));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new oqa::shim::PluginProcessor();
}
