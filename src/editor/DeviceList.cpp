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

#include "DeviceList.h"

namespace oqa::editor
{
    namespace
    {
        juce::String agoText (juce::int64 ms)
        {
            if (ms < 0)
                return "never seen";

            if (ms < 2000)
                return "seen just now";

            if (ms < 120000)
                return "seen " + juce::String (ms / 1000) + " s ago";

            return "seen " + juce::String (ms / 60000) + " min ago";
        }
    }

    DeviceList::DeviceList (core::Instance& instanceIn)
        : instance (instanceIn)
    {
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (title);

        list.setRowHeight (26);
        addAndMakeVisible (list);

        pointButton.setTooltip ("Point the selected device at the next device clicked, or at the address typed below. Every instance follows.");
        pointButton.onClick = [this]
        {
            pointArmed = pointArmed == selected ? -1 : selected;
            updateButtons();
        };
        addAndMakeVisible (pointButton);

        identityButton.setTooltip ("The next read-only leaf clicked in the tree becomes this device's identity leaf: what tells it from another of the same name.");
        identityButton.onClick = [this]
        {
            const auto& chunk = instance.chunk();

            if (selected < 0 || selected >= static_cast<int> (chunk.devices.size()))
                return;

            if (chunk.devices[static_cast<std::size_t> (selected)].identityPath.has_value())
            {
                instance.setDeviceIdentity (selected, std::nullopt, std::nullopt);
                identityArmed = false;

                if (onIdentityArm != nullptr)
                    onIdentityArm (selected, false);
            }
            else
            {
                identityArmed = ! identityArmed;

                if (onIdentityArm != nullptr)
                    onIdentityArm (selected, identityArmed);
            }

            updateButtons();
        };
        addAndMakeVisible (identityButton);

        manualEntry.setTextToShowWhenEmpty ("host:port", juce::Colours::grey);
        manualEntry.onReturnKey = [this] { addManual(); };
        addAndMakeVisible (manualEntry);

        addButton.onClick = [this] { addManual(); };
        addAndMakeVisible (addButton);

        /*  The device's timing, by hand (6a-3): the Monitor and every editor
            share it, and every chunk takes it on its next save. */
        timetagsToggle.setTooltip ("The device schedules OSC bundles by their timetag: sends carry the intended playback time plus the offset. Off, it receives the immediate tag.");
        timetagsToggle.onClick = [this]
        {
            if (selected >= 0 && selected < static_cast<int> (instance.chunk().devices.size()))
                instance.setDeviceTiming (selected, timetagsToggle.getToggleState(), instance.chunk().devices[static_cast<std::size_t> (selected)].offsetMs);
        };
        addAndMakeVisible (timetagsToggle);

        offsetLabel.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (offsetLabel);
        offsetField.setFont (juce::FontOptions (11.0f));
        offsetField.setEditable (true, false, false);
        offsetField.setTooltip ("A signed offset in milliseconds for this device: applied to timetags when the device honours them, else as a send delay. OSC latency compensation, set once per rig.");
        offsetField.setColour (juce::Label::outlineColourId, juce::Colours::grey.withAlpha (0.5f));
        offsetField.onTextChange = [this]
        {
            if (selected >= 0 && selected < static_cast<int> (instance.chunk().devices.size()))
                instance.setDeviceTiming (selected, timetagsToggle.getToggleState(), offsetField.getText().getDoubleValue());
        };
        addAndMakeVisible (offsetField);

        recents.load();

        /*  The registry's Finder, one per process (5b): started for this
            editor, and told every recent address so it is probed and listed. */
        instance.finderEnsure();

        for (const auto& entry : recents.all())
            instance.finderAddManual (entry.host, entry.port);

        updateButtons();
    }

    DeviceList::~DeviceList() = default;

    void DeviceList::resized()
    {
        auto area = getLocalBounds();
        title.setBounds (area.removeFromTop (24));

        auto entry = area.removeFromBottom (28).reduced (0, 2);
        addButton.setBounds (entry.removeFromRight (50));
        entry.removeFromRight (4);
        manualEntry.setBounds (entry);

        auto buttons = area.removeFromBottom (26).reduced (0, 2);
        pointButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        buttons.removeFromLeft (4);
        identityButton.setBounds (buttons);

        auto timing = area.removeFromBottom (24).reduced (0, 2);
        timetagsToggle.setBounds (timing.removeFromLeft (juce::jmax (120, timing.getWidth() / 2)));
        offsetField.setBounds (timing.removeFromRight (56));
        offsetLabel.setBounds (timing.removeFromRight (60));

        list.setBounds (area.withTrimmedBottom (4));
    }

    void DeviceList::updateButtons()
    {
        const auto& chunk = instance.chunk();
        const bool haveSession = selected >= 0 && selected < static_cast<int> (chunk.devices.size());
        const bool hasIdentity = haveSession && chunk.devices[static_cast<std::size_t> (selected)].identityPath.has_value();

        pointButton.setEnabled (haveSession);
        pointButton.setButtonText (pointArmed >= 0 && pointArmed == selected ? "Point: click a device" : "Point...");
        pointButton.setToggleState (pointArmed >= 0 && pointArmed == selected, juce::dontSendNotification);

        identityButton.setEnabled (haveSession);
        identityButton.setButtonText (hasIdentity ? "Clear identity" : (identityArmed ? "Identity: click a leaf" : "Identity..."));
        identityButton.setToggleState (identityArmed && ! hasIdentity, juce::dontSendNotification);

        updateTiming();
    }

    void DeviceList::updateTiming()
    {
        const auto& chunk = instance.chunk();
        const bool haveSession = selected >= 0 && selected < static_cast<int> (chunk.devices.size());
        timetagsToggle.setEnabled (haveSession);
        offsetField.setEnabled (haveSession);

        if (! haveSession)
        {
            timetagsToggle.setToggleState (false, juce::dontSendNotification);
            offsetField.setText ({}, juce::dontSendNotification);
            return;
        }

        const auto& ref = chunk.devices[static_cast<std::size_t> (selected)];
        timetagsToggle.setToggleState (ref.honoursTimetags, juce::dontSendNotification);

        if (! offsetField.isBeingEdited())
            offsetField.setText (juce::String (ref.offsetMs, 1), juce::dontSendNotification);
    }

    void DeviceList::identityDone()
    {
        identityArmed = false;
        updateButtons();
    }

    void DeviceList::addManual()
    {
        const auto text = manualEntry.getText().trim();
        const auto colon = text.lastIndexOfChar (':');

        if (colon <= 0)
            return;

        const auto host = text.substring (0, colon).trim();
        const auto port = text.substring (colon + 1).getIntValue();

        if (host.isEmpty() || port <= 0 || port > 65535)
            return;

        manualEntry.clear();

        /*  Armed: the typed address is where the selected device now is. */
        if (pointArmed >= 0)
        {
            point (pointArmed, host, port);
            return;
        }

        instance.finderAddManual (host.toStdString(), port);
    }

    void DeviceList::point (int deviceIndex, const juce::String& host, int port)
    {
        const auto& chunk = instance.chunk();

        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (chunk.devices.size()))
            return;

        const auto& ref = chunk.devices[static_cast<std::size_t> (deviceIndex)];
        instance.pointDevice (deviceIndex, host.toStdString(), port);
        rememberRecent (juce::String (ref.name), host, port, ref.identityPath, ref.identityValue);
        pointArmed = -1;
        selected = deviceIndex;
        updateButtons();

        if (onDeviceSelected != nullptr)
            onDeviceSelected (selected);
    }

    void DeviceList::rememberRecent (const juce::String& name, const juce::String& host, int port,
                                     const std::optional<std::string>& identityPath, const std::optional<std::string>& identityValue)
    {
        oscquery::Recents::Entry entry;
        entry.name = name.toStdString();
        entry.host = host.toStdString();
        entry.port = port;
        entry.identityPath = identityPath;
        entry.identityValue = identityValue;
        recents.touch (entry);
    }

    //==============================================================================
    void DeviceList::refresh (const juce::var& registryStatus)
    {
        rows.clear();

        /*  This session's devices, with the registry's word (5b: where each
            stands with the device at its address), and under an unresolved
            one the candidates found under its name. */
        const auto& chunk = instance.chunk();
        const auto* devices = registryStatus.getProperty ("devices", {}).getArray();

        for (std::size_t i = 0; i < chunk.devices.size(); ++i)
        {
            const auto& ref = chunk.devices[i];
            Row row;
            row.source = Row::Source::session;
            row.name = juce::String (ref.name);
            row.host = juce::String (ref.lastHost);
            row.port = ref.lastPort;
            row.identity = ref.identityValue.has_value() ? juce::String (*ref.identityValue) : juce::String();
            row.deviceIndex = static_cast<int> (i);

            juce::var d;

            if (devices != nullptr && static_cast<int> (i) < devices->size())
                d = (*devices)[static_cast<int> (i)];

            if (! d.isObject())
            {
                row.state = "unresolved";
                rows.push_back (row);
                continue;
            }

            row.reachable = static_cast<bool> (d.getProperty ("reachable", false));
            const auto registryHost = d.getProperty ("host", "").toString();
            const auto registryPort = static_cast<int> (d.getProperty ("port", 0));

            if (registryHost.isNotEmpty() && registryPort > 0)
            {
                row.host = registryHost;
                row.port = registryPort;
            }

            const auto resolution = d.getProperty ("resolution", "").toString();
            const auto state = d.getProperty ("state", "").toString();
            const bool treeLoaded = static_cast<bool> (d.getProperty ("tree_loaded", false));
            const auto seen = static_cast<juce::int64> (d.getProperty ("last_seen_ms", -1));

            if (resolution == "confirmed" || resolution == "pointed")
            {
                row.state = state == "unreachable" ? "unreachable, " + agoText (seen)
                          : (treeLoaded ? (resolution == "pointed" ? "pointed, ready" : "ready")
                                        : (row.reachable ? "reading tree" : state));
            }
            else if (resolution == "mismatch")
            {
                row.state = "answers as '" + d.getProperty ("answered_name", "").toString() + "'";
            }
            else if (resolution == "searching")
            {
                row.state = "searching, " + agoText (seen);
            }
            else if (resolution == "unresolved")
            {
                const auto* candidates = d.getProperty ("candidates", {}).getArray();
                row.state = "unresolved: " + juce::String (candidates != nullptr ? candidates->size() : 0) + " found";
            }
            else
            {
                row.state = row.reachable ? "identifying" : state;
            }

            /*  A device the registry has since found or been pointed at
                elsewhere is a recent at that address too. */
            if ((resolution == "confirmed" || resolution == "pointed") && registryHost.isNotEmpty() && registryPort > 0)
            {
                const auto known = recents.find (ref.name, ref.identityValue);

                if (! known.has_value() || known->host != registryHost.toStdString() || known->port != registryPort)
                    rememberRecent (row.name, registryHost, registryPort, ref.identityPath, ref.identityValue);
            }

            rows.push_back (row);

            if (resolution == "unresolved")
            {
                if (const auto* candidates = d.getProperty ("candidates", {}).getArray())
                {
                    for (const auto& c : *candidates)
                    {
                        Row candidate;
                        candidate.source = Row::Source::candidate;
                        /*  Found by its DEVICE_ID, a candidate may answer to
                            another NAME than the session's (6x-4): its own is shown. */
                        const auto ownName = c.getProperty ("name", "").toString();
                        candidate.name = "   " + (ownName.isNotEmpty() ? ownName : row.name);
                        candidate.host = c.getProperty ("host", "").toString();
                        candidate.port = static_cast<int> (c.getProperty ("port", 0));
                        candidate.identity = c.getProperty ("identity", "").toString();
                        candidate.reachable = true;
                        candidate.deviceIndex = static_cast<int> (i);
                        const bool identityMatch = static_cast<bool> (c.getProperty ("identity_match", false));
                        const bool hashKnown = static_cast<bool> (c.getProperty ("hash_known", false));
                        const bool hashMatch = static_cast<bool> (c.getProperty ("hash_match", false));
                        const bool idKnown = static_cast<bool> (c.getProperty ("id_known", false));
                        const bool idMatch = static_cast<bool> (c.getProperty ("id_match", false));

                        /*  A candidate found by the identifier was not asked for
                            its identity leaf: nothing is said of it. */
                        candidate.state = juce::String ("candidate")
                                        + (idKnown ? (idMatch ? ", same DEVICE_ID" : ", other DEVICE_ID") : juce::String())
                                        + (ref.identityPath.has_value() && ! idMatch ? (identityMatch ? ", identity matches" : ", identity differs") : juce::String())
                                        + (hashKnown ? (hashMatch ? ", same tree" : ", other tree") : juce::String());
                        rows.push_back (candidate);
                    }
                }
            }
        }

        const auto alreadyListed = [this] (const juce::String& host, int port, const juce::String& name)
        {
            for (const auto& r : rows)
                if ((r.host == host && r.port == port) || (name.isNotEmpty() && r.name == name && r.source == Row::Source::session))
                    return true;

            return false;
        };

        /*  The registry's Finder: found and typed, probed. */
        if (const auto* found = registryStatus.getProperty ("found", {}).getArray())
        {
            for (const auto& f : *found)
            {
                const auto host = f.getProperty ("host", "").toString();
                const auto port = static_cast<int> (f.getProperty ("port", 0));
                const bool reachable = static_cast<bool> (f.getProperty ("reachable", false));
                const bool probed = static_cast<bool> (f.getProperty ("probed", false));
                const bool manual = static_cast<bool> (f.getProperty ("manual", false));
                const auto name = reachable ? f.getProperty ("name", "").toString() : juce::String();

                if (alreadyListed (host, port, name))
                    continue;

                Row row;
                row.source = manual ? Row::Source::manual : Row::Source::found;
                row.name = name.isNotEmpty() ? name : host + ":" + juce::String (port);
                row.host = host;
                row.port = port;
                row.reachable = reachable;
                row.joinable = reachable;
                row.state = reachable ? (manual ? "typed" : "found") : (probed ? "unreachable" : "probing");
                rows.push_back (row);
            }
        }

        for (const auto& entry : recents.all())
        {
            const auto host = juce::String (entry.host);

            if (alreadyListed (host, entry.port, juce::String (entry.name)))
                continue;

            Row row;
            row.source = Row::Source::recent;
            row.name = juce::String (entry.name);
            row.host = host;
            row.port = entry.port;
            row.identity = entry.identityValue.has_value() ? juce::String (*entry.identityValue) : juce::String();
            row.reachable = false;
            row.state = "recent";
            rows.push_back (row);
        }

        list.updateContent();
        list.repaint();
        updateButtons();
    }

    int DeviceList::getNumRows()
    {
        return static_cast<int> (rows.size());
    }

    void DeviceList::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
    {
        if (rowNumber < 0 || rowNumber >= static_cast<int> (rows.size()))
            return;

        const auto& row = rows[static_cast<std::size_t> (rowNumber)];
        const auto& colours = getLookAndFeel();

        if (rowIsSelected)
            g.fillAll (colours.findColour (juce::ListBox::backgroundColourId).brighter (0.2f));

        const bool session = row.source == Row::Source::session;
        const auto dot = session ? (row.reachable ? juce::Colours::limegreen : juce::Colours::orangered)
                       : row.source == Row::Source::candidate ? juce::Colours::gold
                       : (row.reachable ? juce::Colours::cornflowerblue : juce::Colours::grey);

        g.setColour (dot);
        g.fillEllipse (6.0f, height / 2.0f - 4.0f, 8.0f, 8.0f);

        const auto name = row.identity.isNotEmpty() ? row.name + "  (id " + row.identity + ")" : row.name;
        const int stateWidth = juce::jlimit (90, 220, width / 3);
        const int addressWidth = juce::jmax (60, (width - 20 - stateWidth) * 2 / 5);
        const int nameWidth = juce::jmax (40, width - 20 - stateWidth - addressWidth);

        /*  An unreachable session device is greyed (PRD's failure table), its last-seen time in its state. */
        g.setColour (colours.findColour (juce::ListBox::textColourId).withAlpha (row.reachable || row.source == Row::Source::candidate ? 1.0f : 0.6f));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (name, 20, 0, nameWidth, height, juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (row.host + ":" + juce::String (row.port), 20 + nameWidth, 0, addressWidth, height, juce::Justification::centredLeft, true);
        g.drawText (row.state, width - stateWidth - 4, 0, stateWidth, height, juce::Justification::centredRight, true);
    }

    void DeviceList::listBoxItemClicked (int rowNumber, const juce::MouseEvent&)
    {
        if (rowNumber < 0 || rowNumber >= static_cast<int> (rows.size()))
            return;

        auto row = rows[static_cast<std::size_t> (rowNumber)];

        /*  A candidate beneath an unresolved device: this is where it is. */
        if (row.source == Row::Source::candidate)
        {
            point (row.deviceIndex, row.host, row.port);
            return;
        }

        /*  Armed to point: a found, typed or recent row is the new address. */
        if (pointArmed >= 0 && row.source != Row::Source::session)
        {
            point (pointArmed, row.host, row.port);
            return;
        }

        if (row.source != Row::Source::session)
        {
            /*  Joining the session, once the device has said its NAME: the
                chunk gets the device, the registry starts it, and the recents
                remember it. A row still probing, or unreachable, waits. */
            if (! row.joinable && row.source != Row::Source::recent)
            {
                list.deselectAllRows();
                return;
            }

            core::DeviceRef ref;
            ref.name = row.name.toStdString();
            ref.lastHost = row.host.toStdString();
            ref.lastPort = row.port;

            if (row.source == Row::Source::recent)
                if (const auto known = recents.find (ref.name, std::nullopt))
                {
                    ref.identityPath = known->identityPath;
                    ref.identityValue = known->identityValue;
                }

            row.deviceIndex = instance.addDevice (ref);
            rememberRecent (row.name, row.host, row.port, ref.identityPath, ref.identityValue);
        }

        selected = row.deviceIndex;
        pointArmed = -1;
        updateButtons();

        if (onDeviceSelected != nullptr)
            onDeviceSelected (selected);
    }
}
