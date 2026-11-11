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
    THE JUCE SHIM'S PROCESSOR: VST3 everywhere, AU on macOS, thin on purpose
    (devplan: "the editor is a client of the core; the three format shims are
    thin"). It owns one core::Instance and does four things with it:

      prepareToPlay      connect() to the registry, every time (Logic restarts
                         its hosting process; WFS-DIY learnt it the hard way)
      releaseResources   disconnect(); the destructor too, because a host need
      and the destructor not call releaseResources (JUCE's LV2 wrapper never does)
      processBlock       audio passes through untouched; the slots' values go
                         to the core, which posts what moved and ends the block
      record (Phase 4)   the registry wakes this processor once per message a
                         device streams for a receiving slot; on the message
                         thread the values are polled and pushed to the host
                         as parameter changes inside a gesture: opened by the
                         first value, closed by the timer after the idle
                         timeout (gesture_timeout_ms, default 200). Whether a
                         lane records is the host's mode (Write, Touch, Latch
                         or Read), as the PRD decides; the plugin only tells it
                         when a hand is on the fader.

    WHY THE MESSAGE THREAD for the push: JUCE's VST3 wrapper forwards
    setValueNotifyingHost to the host's performEdit on the calling thread, and
    VST3 wants its component handler called from the UI thread. An
    AsyncUpdater, triggered from the registry's wake, is the marshal.

    The state chunk is the core's JSON, as bytes. Names follow the bindings:
    whenever the core's revision moves, every SlotParameter is told its slot
    and the host is told the parameter info changed (VST3 kParamTitlesChanged,
    AU PropertyChanged; Samplitude, Live and Bitwig follow on drawn lanes,
    REAPER on reopen: the Phase 1 bench).

    The registry is the shared core library's when one is beside the plugin,
    this binary's own otherwise; the status says which.
*/

#include "SlotParameter.h"

#include <core/Instance.h>
#include <core/receive/Gesture.h>
#include <core/registry/RegistryLoader.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstdint>
#include <vector>

namespace oqa::shim
{
    class PluginProcessor final : public juce::AudioProcessor,
                                  private juce::Timer,
                                  private juce::AsyncUpdater
    {
    public:
        PluginProcessor();
        ~PluginProcessor() override;

        static constexpr int slotCount = OQA_SLOT_COUNT;

        const juce::String getName() const override;

        bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void releaseResources() override;

        void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
        using juce::AudioProcessor::processBlock;

        bool hasEditor() const override;
        juce::AudioProcessorEditor* createEditor() override;

        bool acceptsMidi() const override;
        bool producesMidi() const override;
        double getTailLengthSeconds() const override;

        int getNumPrograms() override;
        int getCurrentProgram() override;
        void setCurrentProgram (int index) override;
        const juce::String getProgramName (int index) override;
        void changeProgramName (int index, const juce::String& name) override;

        void getStateInformation (juce::MemoryBlock& destination) override;
        void setStateInformation (const void* data, int sizeInBytes) override;

        //==============================================================================
        /** The core, for the editor. Message thread. */
        core::Instance& instance() noexcept { return core; }
        const core::Instance& instance() const noexcept { return core; }

        /** The loader's decision for this binary: shared library or alone, and why. */
        const core::LoadedCore& loadedCore() const noexcept { return loaded; }

        /** Tell the parameters and the host that the bindings changed. Message thread. */
        void bindingsChanged();

        /** How many slots have a host gesture open right now, for the editor and the tests. */
        int openGestures() const noexcept { return gestures.openCount(); }

    private:
        void timerCallback() override;
        void handleAsyncUpdate() override;
        void closeGestures();
        void refreshSettings();
        void applyCommands();

        /*  Commands from the plugin's own server (Phase 6b-3): the registry
            wakes, this marshals to the message thread, where the core applies
            them and the host is told the bindings changed. */
        struct CommandUpdater final : public juce::AsyncUpdater
        {
            explicit CommandUpdater (PluginProcessor& p) : owner (p) {}
            void handleAsyncUpdate() override { owner.applyCommands(); }
            PluginProcessor& owner;
        };

        const core::LoadedCore& loaded;
        core::Instance core;
        std::vector<SlotParameter*> slots;
        std::array<float, slotCount> values {};
        std::array<float, slotCount> hostNow {};       // the message thread's: each parameter as it reads when values are received
        std::uint64_t shownRevision = 0;

        //  Record, on the message thread.
        core::GestureTracker gestures { slotCount };
        double gestureTimeoutMs = 200.0;
        int ticksSinceSlowWork = 0;

        CommandUpdater commands { *this };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
    };
}
