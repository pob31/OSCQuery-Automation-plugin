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
    THE DEVICE LIST (PRD, UX, Device browser: "Device list from DNS-SD plus
    manual entries and recents; each with name, host, identity value when set,
    reachability dot"; Discovery: "Devices that fail the probe show as
    unreachable, not hidden").

    THREE SOURCES, ONE LIST. This session's devices first, with the registry's
    word on each (Phase 5b: identifying, ready, searching, unresolved with the
    candidates found under its name listed beneath it, answering as another
    name, pointed); then what the registry's Finder has (Phase 5b moved it out
    of the editor: one browser per process, read from the status's `found`):
    DNS-SD's and the typed entries, each probed with HOST_INFO before it is
    listed with its NAME; then the per-user recents. A row that is already a
    session device is shown once. Selecting a row that is not yet in the
    session adds it (that is how a device joins the chunk), once it has
    answered HOST_INFO, so the chunk holds the device's NAME and never a
    placeholder.

    POINTING (PRD, "Reopening a session", step 2: "the user points at it once
    ... and every instance follows"). *Point…* arms the selected session device:
    the next click on a found, typed or recent row, or Add on the manual field,
    points it there; a candidate row beneath an unresolved device points at
    that candidate in one click. *Identity…* arms the tree pane: the next
    read-only leaf clicked becomes the device's identity leaf (the editor does
    the setting); the button clears it when one is set.

    MANUAL ENTRY is a host:port field and a button, because multicast often
    does not cross a show network's VLANs (PRD), and because on this very
    machine nothing announced on the same host is found (Phase 2 bench).

    TIMING (Phase 6d-1; PRD, Transport: "whether a device honours timetags is
    a per-device setting the user ticks", "a signed offset in milliseconds per
    device"): a tick and a field for the selected device, set by hand and so
    process-wide (6a-3). An unreachable device's row is greyed, its last-seen
    time in its state.
*/

#include <core/Instance.h>

#include <oscquery/Recents.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace oqa::editor
{
    class DeviceList final : public juce::Component,
                             private juce::ListBoxModel
    {
    public:
        explicit DeviceList (core::Instance& instance);
        ~DeviceList() override;

        /** Called with the chunk's device index when a device is chosen. */
        std::function<void (int deviceIndex)> onDeviceSelected;

        /** The user wants the next leaf clicked to be (true) or no longer be (false) the selected device's identity. */
        std::function<void (int deviceIndex, bool arm)> onIdentityArm;

        /** Re-read the three sources and repaint; called by the editor's timer. */
        void refresh (const juce::var& registryStatus);

        void resized() override;

        /** The session device index selected, or -1. */
        int selectedDevice() const noexcept { return selected; }

        /** The editor set or cleared an identity: the arm is spent. */
        void identityDone();

    private:
        struct Row
        {
            enum class Source { session, candidate, found, manual, recent };
            Source source = Source::session;
            juce::String name;
            juce::String host;
            int port = 0;
            juce::String identity;     ///< the identity value, when set or read
            juce::String state;        ///< words for the row's right-hand side
            bool reachable = false;
            bool joinable = true;      ///< a found row that has answered HOST_INFO (its NAME is known)
            int deviceIndex = -1;      ///< the chunk's, when source == session or candidate
        };

        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent&) override;
        void addManual();
        void point (int deviceIndex, const juce::String& host, int port);
        void rememberRecent (const juce::String& name, const juce::String& host, int port,
                             const std::optional<std::string>& identityPath, const std::optional<std::string>& identityValue);
        void updateButtons();

        core::Instance& instance;
        oscquery::Recents recents;

        std::vector<Row> rows;
        int selected = -1;
        int pointArmed = -1;           ///< the session device the next click points, or -1
        bool identityArmed = false;

        juce::Label title { {}, "Devices" };
        juce::ListBox list { "devices", this };
        juce::TextButton pointButton { "Point..." };
        juce::TextButton identityButton { "Identity..." };
        juce::TextEditor manualEntry;
        juce::TextButton addButton { "Add" };

        juce::ToggleButton timetagsToggle { "honours timetags" };
        const juce::String timetagsTooltip { "The device schedules OSC bundles by their timetag: sends carry the intended playback time plus the offset. Off, it receives the immediate tag." };
        juce::Label offsetLabel { {}, "offset ms" };
        juce::Label offsetField;
        void updateTiming();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceList)
    };
}
