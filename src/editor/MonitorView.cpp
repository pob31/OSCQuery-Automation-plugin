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

#include "MonitorView.h"
#include "SlotPresentation.h"

namespace oqa::editor
{
    namespace
    {
        juce::Colour shadeColour (presentation::Shade shade)
        {
            switch (shade)
            {
                case presentation::Shade::grey:      return juce::Colours::grey;
                case presentation::Shade::amber:     return juce::Colours::orange;
                case presentation::Shade::red:       return juce::Colours::red;
                case presentation::Shade::orangeRed: return juce::Colours::orangered;
                case presentation::Shade::green:     return juce::Colours::limegreen;
            }

            return juce::Colours::grey;
        }

        void drawCellText (juce::Graphics& g, const juce::String& text, int width, int height, float alpha = 1.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (alpha));
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (text, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
        }
    }

    //==============================================================================
    /** The devices table: a row per device the registry drives. */
    class MonitorView::DeviceRows final : public juce::TableListBoxModel
    {
    public:
        enum Column { name = 1, address, state, rates, timetags, offset, error };

        explicit DeviceRows (MonitorView& owner) : view (owner) {}

        int getNumRows() override { return static_cast<int> (view.shown.devices.size()); }

        void paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected) override
        {
            if (row < 0 || row >= getNumRows())
                return;

            const auto& d = view.shown.devices[static_cast<std::size_t> (row)];
            g.fillAll (selected ? juce::Colours::white.withAlpha (0.08f) : juce::Colours::transparentBlack);
            g.setColour (d.reachable ? juce::Colours::limegreen : juce::Colours::orangered);
            g.fillEllipse (6.0f, height / 2.0f - 4.0f, 8.0f, 8.0f);
            juce::ignoreUnused (width);
        }

        void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool) override
        {
            if (row < 0 || row >= getNumRows())
                return;

            const auto& d = view.shown.devices[static_cast<std::size_t> (row)];
            const float alpha = d.reachable ? 1.0f : 0.6f;

            switch (column)
            {
                case name:    g.setColour (juce::Colours::white.withAlpha (alpha));
                              g.setFont (juce::FontOptions (12.0f));
                              g.drawText (d.identity.isNotEmpty() ? d.name + "  (id " + d.identity + ")" : d.name, 18, 0, width - 22, height, juce::Justification::centredLeft, true);
                              break;
                case state:   drawCellText (g, d.stateWords(), width, height, alpha); break;
                case rates:   drawCellText (g, d.senderReady || d.receivedPerSecond > 0.0 ? d.ratesWords() : juce::String ("-"), width, height, alpha); break;
                case error:   g.setColour (juce::Colours::orange.withAlpha (alpha));
                              g.setFont (juce::FontOptions (12.0f));
                              g.drawText (d.errorWords(), 4, 0, width - 8, height, juce::Justification::centredLeft, true);
                              break;
                default: break;
            }
        }

        juce::String getCellTooltip (int row, int column) override
        {
            if (row < 0 || row >= getNumRows())
                return {};

            const auto& d = view.shown.devices[static_cast<std::size_t> (row)];

            switch (column)
            {
                case address:  return "host:port the registry drives this device at (" + d.resolution + "). Double-click to point it by hand; every instance follows.";
                case timetags: return "The device schedules OSC bundles by their timetag. Set by hand here, it stands for every instance and lands in each chunk on save."
                                    + (d.timetagsWords().isNotEmpty() ? " " + d.timetagsWords() : juce::String());
                case offset:   return "A signed offset in milliseconds for this device, applied to timetags or as a send delay.";
                case rates:    return juce::String (d.loopGuarded) + " echoes dropped by the loop guard, " + juce::String (d.rateLimited) + " held by the rate limiter, "
                                    + juce::String (d.datagramsDropped) + " datagrams dropped, " + juce::String (d.sendFailures) + " send failures.";
                default:       return {};
            }
        }

        juce::Component* refreshComponentForCell (int row, int column, bool, juce::Component* existing) override
        {
            if (row < 0 || row >= getNumRows())
            {
                delete existing;
                return nullptr;
            }

            const auto& d = view.shown.devices[static_cast<std::size_t> (row)];

            if (column == timetags)
            {
                auto* toggle = dynamic_cast<juce::ToggleButton*> (existing);

                if (toggle == nullptr)
                {
                    delete existing;
                    toggle = new juce::ToggleButton();
                }

                toggle->setToggleState (d.honoursTimetags, juce::dontSendNotification);
                toggle->onClick = [this, row, toggle]
                {
                    if (row < getNumRows() && view.actions.setDeviceTiming != nullptr)
                    {
                        const auto& device = view.shown.devices[static_cast<std::size_t> (row)];
                        view.actions.setDeviceTiming (device.name, device.identity, toggle->getToggleState(), device.offsetMs);
                    }
                };
                return toggle;
            }

            if (column == offset || column == address)
            {
                auto* label = dynamic_cast<juce::Label*> (existing);

                if (label == nullptr)
                {
                    delete existing;
                    label = new juce::Label();
                    label->setFont (juce::FontOptions (12.0f));
                    label->setEditable (false, true, false);
                }

                const auto text = column == offset ? juce::String (d.offsetMs, 1) : d.host + ":" + juce::String (d.port);

                if (! label->isBeingEdited())
                    label->setText (text, juce::dontSendNotification);

                label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (d.reachable ? 1.0f : 0.6f));
                label->onTextChange = [this, row, label, column]
                {
                    if (row >= getNumRows())
                        return;

                    const auto& device = view.shown.devices[static_cast<std::size_t> (row)];

                    if (column == offset && view.actions.setDeviceTiming != nullptr)
                        view.actions.setDeviceTiming (device.name, device.identity, device.honoursTimetags, label->getText().getDoubleValue());

                    if (column == address && view.actions.pointDevice != nullptr)
                    {
                        const auto typed = label->getText().trim();
                        const auto colon = typed.lastIndexOfChar (':');
                        const auto host = colon > 0 ? typed.substring (0, colon).trim() : typed;
                        const auto port = colon > 0 ? typed.substring (colon + 1).getIntValue() : device.port;

                        if (host.isNotEmpty() && port > 0 && port < 65536)
                        {
                            if (! view.actions.pointDevice (device.name, device.identity, host, port))
                                view.setNote ("could not point " + device.name + " at " + host + ":" + juce::String (port));
                        }
                        else
                        {
                            view.setNote ("an address is host:port");
                        }
                    }
                };
                return label;
            }

            delete existing;
            return nullptr;
        }

    private:
        MonitorView& view;
    };

    //==============================================================================
    /** The slots table: every bound slot of every instance. */
    class MonitorView::SlotRows final : public juce::TableListBoxModel
    {
    public:
        enum Column { instance = 1, slot, name, device, path, direction, state };

        explicit SlotRows (MonitorView& owner) : view (owner) {}

        int getNumRows() override { return view.shown.boundSlots(); }

        void paintRowBackground (juce::Graphics& g, int row, int, int height, bool selected) override
        {
            if (row < 0 || row >= getNumRows())
                return;

            g.fillAll (selected ? juce::Colours::white.withAlpha (0.08f) : juce::Colours::transparentBlack);

            const auto& r = view.shown.slotRows[static_cast<std::size_t> (row)];
            const auto& s = view.shown.slotOf (r);
            const auto& i = view.shown.instanceOf (r);

            presentation::Standing standing;
            standing.bound = true;
            standing.active = true;
            standing.state = s.state.toStdString();
            standing.detail = s.detail.toStdString();

            for (const auto& d : view.shown.devices)
                if (d.name == s.device)
                    standing.deviceReachable = d.reachable;

            g.setColour (shadeColour (presentation::shadeOf (standing)).withAlpha (i.active ? 1.0f : 0.5f));
            g.fillEllipse (6.0f, height / 2.0f - 4.0f, 8.0f, 8.0f);
        }

        void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool) override
        {
            if (row < 0 || row >= getNumRows())
                return;

            const auto& r = view.shown.slotRows[static_cast<std::size_t> (row)];
            const auto& s = view.shown.slotOf (r);
            const auto& i = view.shown.instanceOf (r);
            const float alpha = i.active ? 1.0f : 0.6f;

            switch (column)
            {
                case instance:  g.setColour (juce::Colours::white.withAlpha (alpha));
                                g.setFont (juce::FontOptions (12.0f));
                                g.drawText (i.label + (i.number != 1 ? "  ($instance " + juce::String (i.number) + ")" : juce::String()), 18, 0, width - 22, height, juce::Justification::centredLeft, true);
                                break;
                case slot:      drawCellText (g, juce::String (s.id + 1), width, height, alpha); break;
                case name:      drawCellText (g, s.displayName, width, height, alpha); break;
                case device:    drawCellText (g, s.device, width, height, alpha); break;
                case path:      drawCellText (g, s.path + (s.argIndex > 0 ? "[" + juce::String (s.argIndex) + "]" : juce::String()), width, height, alpha); break;
                case direction: drawCellText (g, s.direction, width, height, alpha); break;
                case state:     drawCellText (g, s.state.isEmpty() ? juce::String ("-") : s.state + (s.detail.isNotEmpty() ? ": " + s.detail : juce::String()), width, height, alpha); break;
                default: break;
            }
        }

        juce::String getCellTooltip (int row, int) override
        {
            if (row < 0 || row >= getNumRows())
                return {};

            const auto& r = view.shown.slotRows[static_cast<std::size_t> (row)];
            const auto& s = view.shown.slotOf (r);
            const auto& i = view.shown.instanceOf (r);
            return (i.active ? juce::String() : juce::String ("The host has stopped this instance's audio; the slot stays bound. "))
                 + (s.detail.isNotEmpty() ? s.detail : juce::String());
        }

    private:
        MonitorView& view;
    };

    //==============================================================================
    /** The settings form: one row per field of the per-user settings file. */
    class MonitorView::SettingsForm final : public juce::Component
    {
    public:
        explicit SettingsForm (MonitorView& owner) : view (owner)
        {
            addNumber ("default_rate_hz", "Rate limit (Hz)", "Per-path send rate limit, 0 for none. The PRD's default is 100.");
            addNumber ("resend_ms", "Resend (ms)", "Periodic resend of unchanged values for lossy links, 0 for none.");
            addNumber ("loop_guard_ms", "Loop guard (ms)", "A value the device echoes this soon after a send is not recorded.");
            addNumber ("gesture_timeout_ms", "Gesture timeout (ms)", "A recorded gesture closes after this much silence from the device.");
            addNumber ("receive_port", "Receive port", "The UDP port for devices without LISTEN, 0 for ephemeral. Type it into the device.");
            addToggle ("server_enabled", "Serve the slot table", "The plugin's own OSCQuery server, announced over DNS-SD: a script binds slots through it.");
            addNumber ("server_port", "Server HTTP port", "0 for an ephemeral port, announced; a pinned port that cannot be bound is an error, never a silent fall-back.");
            addNumber ("server_osc_port", "Server OSC port", "The server's own UDP endpoint for OSC writes; 0 for ephemeral.");
            addText ("server_allow", "Server writes from", "Who may write: empty for this machine only, 'any', or addresses separated by commas.");
            addText ("server_name", "Server NAME", "Empty for 'OSCQuery Automation @ <machine> (<host app>)'.");
            addToggle ("direct_send", "Direct send (bench only)", "Encode and send on the audio thread. For a bench measurement; it allocates and blocks where a plugin must not.");

            clearRecents.setButtonText ("Clear recents");
            clearRecents.setTooltip ("Forget every device this user has bound to, across sessions.");
            clearRecents.onClick = [this]
            {
                if (view.actions.clearRecents != nullptr)
                    view.setNote (juce::String (view.actions.clearRecents()) + " recent device(s) forgotten");
            };
            addAndMakeVisible (clearRecents);

            warning.setFont (juce::FontOptions (11.0f));
            warning.setColour (juce::Label::textColourId, juce::Colours::orange);
            warning.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (warning);

            setSize (260, heightNeeded());
        }

        void refresh (const juce::var& settings)
        {
            for (auto& field : fields)
            {
                auto& f = *field;

                if (f.toggle != nullptr)
                    f.toggle->setToggleState (static_cast<bool> (settings.getProperty (f.key, false)), juce::dontSendNotification);

                if (f.entry != nullptr && ! f.entry->isBeingEdited())
                {
                    const juce::var v = settings.getProperty (f.key, juce::var());
                    f.entry->setText (v.isDouble() ? juce::String (static_cast<double> (v), 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") : v.toString(), juce::dontSendNotification);
                }
            }

            warning.setText (static_cast<bool> (settings.getProperty ("direct_send", false))
                                 ? "Direct send is on: sends happen on the audio thread. Turn it off outside a bench."
                                 : juce::String(),
                             juce::dontSendNotification);
        }

        int heightNeeded() const { return static_cast<int> (fields.size()) * rowHeight + 36 + 40; }

        void resized() override
        {
            auto area = getLocalBounds().reduced (4, 0);

            for (auto& field : fields)
            {
                auto& f = *field;
                auto row = area.removeFromTop (rowHeight);
                f.label.setBounds (row.removeFromLeft (row.getWidth() / 2));

                if (f.entry != nullptr)
                    f.entry->setBounds (row.reduced (0, 3));

                if (f.toggle != nullptr)
                    f.toggle->setBounds (row.reduced (0, 2));
            }

            clearRecents.setBounds (area.removeFromTop (28).reduced (0, 3).removeFromLeft (120));
            warning.setBounds (area.removeFromTop (40));
        }

    private:
        static constexpr int rowHeight = 26;

        struct Field
        {
            juce::String key;
            juce::Label label;
            std::unique_ptr<juce::Label> entry;
            std::unique_ptr<juce::ToggleButton> toggle;
            bool number = false;
        };

        void apply (const juce::String& key, const juce::var& value)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty (key, value);
            juce::String error;

            if (view.actions.setSettings != nullptr && ! view.actions.setSettings (juce::JSON::toString (juce::var (o), true), error))
                view.setNote ("settings: " + error);
        }

        Field& addField (const juce::String& key, const juce::String& title, const juce::String& tip)
        {
            fields.push_back (std::make_unique<Field>());
            auto& f = *fields.back();
            f.key = key;
            f.label.setText (title, juce::dontSendNotification);
            f.label.setFont (juce::FontOptions (12.0f));
            f.label.setTooltip (tip);
            addAndMakeVisible (f.label);
            return f;
        }

        void addNumber (const juce::String& key, const juce::String& title, const juce::String& tip)
        {
            auto& f = addField (key, title, tip);
            f.number = true;
            f.entry = std::make_unique<juce::Label>();
            f.entry->setFont (juce::FontOptions (12.0f));
            f.entry->setEditable (true, false, false);
            f.entry->setTooltip (tip);
            f.entry->setColour (juce::Label::outlineColourId, juce::Colours::grey.withAlpha (0.5f));
            f.entry->onTextChange = [this, &f] { apply (f.key, f.entry->getText().getDoubleValue()); };
            addAndMakeVisible (*f.entry);
        }

        void addText (const juce::String& key, const juce::String& title, const juce::String& tip)
        {
            auto& f = addField (key, title, tip);
            f.entry = std::make_unique<juce::Label>();
            f.entry->setFont (juce::FontOptions (12.0f));
            f.entry->setEditable (true, false, false);
            f.entry->setTooltip (tip);
            f.entry->setColour (juce::Label::outlineColourId, juce::Colours::grey.withAlpha (0.5f));
            f.entry->onTextChange = [this, &f] { apply (f.key, f.entry->getText().trim()); };
            addAndMakeVisible (*f.entry);
        }

        void addToggle (const juce::String& key, const juce::String& title, const juce::String& tip)
        {
            auto& f = addField (key, title, tip);
            f.toggle = std::make_unique<juce::ToggleButton>();
            f.toggle->setTooltip (tip);
            f.toggle->onClick = [this, &f] { apply (f.key, f.toggle->getToggleState()); };
            addAndMakeVisible (*f.toggle);
        }

        MonitorView& view;
        std::vector<std::unique_ptr<Field>> fields;
        juce::TextButton clearRecents;
        juce::Label warning;

        /*  Field's two juce::Labels and the vector of unique_ptr: a Field never
            moves once made, so the lambdas' references hold. */
        JUCE_DECLARE_NON_COPYABLE (SettingsForm)
    };

    //==============================================================================
    MonitorView::MonitorView (Actions a)
        : actions (std::move (a)),
          deviceRows (std::make_unique<DeviceRows> (*this)),
          slotRows (std::make_unique<SlotRows> (*this)),
          devices ("monitor-devices", deviceRows.get()),
          slots ("monitor-slots", slotRows.get()),
          settings (std::make_unique<SettingsForm> (*this))
    {
        serverLabel.setFont (juce::FontOptions (12.0f));
        serverLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (serverLabel);

        infoLabel.setFont (juce::FontOptions (11.0f));
        infoLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        infoLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (infoLabel);

        noteLabel.setFont (juce::FontOptions (11.0f));
        noteLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
        addAndMakeVisible (noteLabel);

        for (auto* title : { &devicesTitle, &slotsTitle, &settingsTitle })
        {
            title->setFont (juce::FontOptions (13.0f, juce::Font::bold));
            addAndMakeVisible (*title);
        }

        auto& dh = devices.getHeader();
        dh.addColumn ("Device", DeviceRows::name, 180, 80, 400);
        dh.addColumn ("Address", DeviceRows::address, 130, 80, 240);
        dh.addColumn ("State", DeviceRows::state, 150, 80, 300);
        dh.addColumn ("Rates", DeviceRows::rates, 190, 100, 300);
        dh.addColumn ("Timetags", DeviceRows::timetags, 60, 50, 80, juce::TableHeaderComponent::notResizableOrSortable);
        dh.addColumn ("Offset ms", DeviceRows::offset, 70, 50, 100, juce::TableHeaderComponent::notResizableOrSortable);
        dh.addColumn ("Last error", DeviceRows::error, 220, 80, 600);
        devices.setRowHeight (22);
        devices.setMultipleSelectionEnabled (false);
        addAndMakeVisible (devices);

        auto& sh = slots.getHeader();
        sh.addColumn ("Instance", SlotRows::instance, 170, 80, 400);
        sh.addColumn ("#", SlotRows::slot, 34, 30, 50, juce::TableHeaderComponent::notResizableOrSortable);
        sh.addColumn ("Name", SlotRows::name, 140, 60, 300);
        sh.addColumn ("Device", SlotRows::device, 120, 60, 300);
        sh.addColumn ("Path", SlotRows::path, 220, 80, 600);
        sh.addColumn ("Direction", SlotRows::direction, 64, 50, 90);
        sh.addColumn ("State", SlotRows::state, 200, 80, 600);
        slots.setRowHeight (20);
        slots.setMultipleSelectionEnabled (false);
        addAndMakeVisible (slots);

        settingsViewport.setViewedComponent (settings.get(), false);
        settingsViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (settingsViewport);

        /*  The pattern row. */
        patternLabel.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (patternLabel);

        for (auto* field : { &patternSlot, &patternPath, &patternDevice })
        {
            field->setFont (juce::FontOptions (12.0f));
            field->setEditable (true, false, false);
            field->setColour (juce::Label::outlineColourId, juce::Colours::grey.withAlpha (0.5f));
            addAndMakeVisible (*field);
        }

        patternSlot.setText ("1", juce::dontSendNotification);
        patternSlot.setJustificationType (juce::Justification::centred);
        patternSlot.setTooltip ("The slot, counted from 1, in every instance.");
        patternPath.setTooltip ("A path with $instance, which each instance replaces by its own number; a plain path binds the same leaf everywhere.");
        patternDevice.setTooltip ("The device NAME to bind on; empty keeps each slot's device, or the instance's first.");
        patternApply.setTooltip ("Queue the bind for every slot plugin in this process, as a write to the plugin's own server would. The result shows in each row's state.");
        patternApply.onClick = [this] { applyPattern(); };
        addAndMakeVisible (patternApply);
    }

    void MonitorView::applyPattern()
    {
        const int slot = patternSlot.getText().getIntValue() - 1;
        const auto path = patternPath.getText().trim();
        const auto device = patternDevice.getText().trim();

        if (slot < 0 || path.isEmpty() || actions.commandInstance == nullptr)
        {
            setNote ("a slot number from 1 and a path are needed");
            return;
        }

        int queued = 0;

        for (const auto& i : shown.instances)
            if (i.slotCount > slot && actions.commandInstance (i.id, slot, "bind", path, device, 0.0))
                ++queued;

        setNote (juce::String (queued) + " instance(s) asked to bind slot " + juce::String (slot + 1) + " to " + path);
    }

    MonitorView::~MonitorView()
    {
        devices.setModel (nullptr);
        slots.setModel (nullptr);
    }

    void MonitorView::refresh (const MonitorModel& model, const juce::var& settingsVar)
    {
        const bool deviceCountChanged = model.devices.size() != shown.devices.size();
        const bool slotCountChanged = model.slotRows.size() != shown.slotRows.size();
        shown = model;

        serverLabel.setText (shown.server.words(), juce::dontSendNotification);

        juce::String info;
        info << juce::String (shown.instanceCount) << " instance" << (shown.instanceCount == 1 ? "" : "s") << ", "
             << juce::String (shown.boundSlots()) << " bound slot" << (shown.boundSlots() == 1 ? "" : "s") << ", "
             << juce::String (shown.devices.size()) << " device" << (shown.devices.size() == 1 ? "" : "s")
             << "; receive port " << (shown.receivePort > 0 ? juce::String (shown.receivePort) : juce::String ("not open"))
             << "; settings " << (shown.settingsFileLoaded ? "from " : "defaults, file ") << shown.settingsFilePath
             << (shown.settingsFileError.isNotEmpty() ? " (" + shown.settingsFileError + ")" : juce::String());
        infoLabel.setText (info, juce::dontSendNotification);

        if (deviceCountChanged)
            devices.updateContent();
        else
            devices.repaint();

        if (slotCountChanged)
            slots.updateContent();
        else
            slots.repaint();

        /*  The timing and address cells are components: told again, so a
            change by hand elsewhere shows here. */
        devices.updateContent();

        settings->refresh (settingsVar);
    }

    void MonitorView::setNote (const juce::String& note)
    {
        noteLabel.setText (note, juce::dontSendNotification);
    }

    void MonitorView::paint (juce::Graphics& g)
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void MonitorView::resized()
    {
        auto area = getLocalBounds().reduced (6);
        serverLabel.setBounds (area.removeFromTop (20));
        infoLabel.setBounds (area.removeFromTop (18));
        noteLabel.setBounds (area.removeFromBottom (18));
        area.removeFromTop (4);

        auto right = area.removeFromRight (280);
        settingsTitle.setBounds (right.removeFromTop (22));
        settingsViewport.setBounds (right);
        settings->setSize (right.getWidth() - (settingsViewport.isVerticalScrollBarShown() ? 10 : 0), settings->heightNeeded());
        area.removeFromRight (8);

        auto top = area.removeFromTop (juce::jmax (120, area.getHeight() / 3));
        devicesTitle.setBounds (top.removeFromTop (22));
        devices.setBounds (top);
        area.removeFromTop (6);

        auto slotsTop = area.removeFromTop (22);
        slotsTitle.setBounds (slotsTop.removeFromLeft (60));
        patternApply.setBounds (slotsTop.removeFromRight (130).reduced (0, 1));
        slotsTop.removeFromRight (4);
        patternLabel.setBounds (slotsTop.removeFromLeft (60));
        patternSlot.setBounds (slotsTop.removeFromLeft (36).reduced (0, 2));
        slotsTop.removeFromLeft (4);
        patternDevice.setBounds (slotsTop.removeFromRight (juce::jmin (160, slotsTop.getWidth() / 3)).reduced (0, 2));
        slotsTop.removeFromRight (4);
        patternPath.setBounds (slotsTop.reduced (0, 2));
        slots.setBounds (area);
    }
}
