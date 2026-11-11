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

#include <core/Instance.h>
#include <core/PathPattern.h>

#include <oscquery/tree/Tree.h>

#include <cmath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace oqa::core
{
    namespace
    {
        constexpr float notReceived = std::numeric_limits<float>::quiet_NaN();

        /** Whether the table reaches as far as `member`: an older library's stops short. */
        template <typename Member>
        bool tableHas (const OqaCoreApi* api, Member OqaCoreApi::* member)
        {
            if (api == nullptr)
                return false;

            const auto offset = reinterpret_cast<const char*> (&(api->*member)) - reinterpret_cast<const char*> (api);
            return api->size >= static_cast<std::size_t> (offset) + sizeof (Member) && (api->*member) != nullptr;
        }
    }

    Instance::Instance (const OqaCoreApi* apiIn, int slotCountIn, std::string labelIn)
        : api (apiIn), count (std::max (0, slotCountIn)), label (std::move (labelIn)), state (count)
    {
        lastPosted.assign (static_cast<std::size_t> (count), 0.0f);
        received = std::make_unique<std::atomic<float>[]> (static_cast<std::size_t> (count));

        for (int i = 0; i < count; ++i)
            received[static_cast<std::size_t> (i)].store (notReceived, std::memory_order_relaxed);
    }

    Instance::~Instance()
    {
        disconnect();
    }

    //==============================================================================
    void Instance::connect()
    {
        if (api == nullptr)
            return;

        if (handle == nullptr)
        {
            handle = api->instanceRegister (label.c_str(), count);
            registerWake();
            registerCommandWake();
        }

        push();
    }

    void Instance::disconnect()
    {
        if (handle != nullptr && api != nullptr)
            api->instanceUnregister (handle);

        handle = nullptr;
    }

    //==============================================================================
    bool Instance::canCommand() const noexcept
    {
        return tableHas (api, &OqaCoreApi::instanceSetCommandWake) && tableHas (api, &OqaCoreApi::instancePollCommandsJson);
    }

    void Instance::commandWakeTrampoline (void* context)
    {
        auto* self = static_cast<Instance*> (context);

        if (self != nullptr && self->commandWake)
            self->commandWake();
    }

    void Instance::registerCommandWake()
    {
        if (handle != nullptr && canCommand())
            api->instanceSetCommandWake (handle, commandWake ? &Instance::commandWakeTrampoline : nullptr, this);
    }

    void Instance::setCommandWake (std::function<void()> wake)
    {
        commandWake = std::move (wake);
        registerCommandWake();
    }

    int Instance::pollCommands()
    {
        if (handle == nullptr || ! canCommand())
            return 0;

        char* text = api->instancePollCommandsJson (handle);

        if (text == nullptr)
            return 0;

        const auto list = juce::JSON::parse (juce::String (takeError (text)));
        int applied = 0;

        if (const auto* commands = list.getArray())
        {
            for (const auto& command : *commands)
            {
                std::string why;

                if (applyCommand (command, why))
                    ++applied;
                else
                    lastWarnings.push_back ("server write refused: " + why);
            }
        }

        return applied;
    }

    bool Instance::applyCommand (const juce::var& command, std::string& why)
    {
        const auto kind = command.getProperty ("kind", "").toString().toStdString();
        const auto slot = static_cast<int> (command.getProperty ("slot", -1));
        const auto text = command.getProperty ("text", "").toString().toStdString();
        const auto text2 = command.getProperty ("text2", "").toString().toStdString();
        const auto number = static_cast<double> (command.getProperty ("number", 0.0));

        if (kind == "instance")
        {
            setInstanceNumber (static_cast<int> (std::lround (number)));
            return true;
        }

        if (slot < 0 || slot >= count)
        {
            why = "slot " + std::to_string (slot) + " is not one of " + std::to_string (count);
            return false;
        }

        auto& current = state.slots[slot];

        if (kind == "unbind")
        {
            unbind (slot);
            pendingDevice.erase (slot);
            return true;
        }

        if (kind == "bind")
        {
            int device = -1;
            std::string deviceName = text2;

            if (deviceName.empty())
            {
                if (const auto pending = pendingDevice.find (slot); pending != pendingDevice.end())
                    deviceName = pending->second;
            }

            if (! deviceName.empty())
                device = deviceIndexFor (deviceName);
            else if (current.bound())
                device = current.deviceIndex;
            else if (! state.devices.empty())
                device = 0;

            if (device < 0)
            {
                why = deviceName.empty() ? std::string ("slot ") + std::to_string (slot) + ": no device to bind on; name one"
                                         : "device "" + deviceName + "" is known to nobody here";
                return false;
            }

            if (! bindPattern (slot, device, text, 0, why))
                return false;

            pendingDevice.erase (slot);
            return true;
        }

        if (kind == "device")
        {
            if (! current.bound())
            {
                pendingDevice[slot] = text;
                return true;
            }

            const auto device = deviceIndexFor (text);

            if (device < 0)
            {
                why = "device "" + text + "" is known to nobody here";
                return false;
            }

            return bindPath (slot, device, current.path, static_cast<std::size_t> (std::max (0, current.argIndex)), why);
        }

        if (! current.bound())
        {
            why = "slot " + std::to_string (slot) + " is unbound; bind it first";
            return false;
        }

        if (kind == "display_name")   { setDisplayName (slot, text); return true; }

        if (kind == "scaling")
        {
            if (const auto scaling = scalingFromName (text)) { setScaling (slot, *scaling); return true; }
            why = "scaling "" + text + "": linear, log or exp";
            return false;
        }

        if (kind == "direction")
        {
            if (const auto direction = directionFromName (text)) { setDirection (slot, *direction); return true; }
            why = "direction "" + text + "": send, receive or both";
            return false;
        }

        if (kind == "range_min" || kind == "range_max")
        {
            if (! std::isfinite (number))
            {
                why = "range: not a number";
                return false;
            }

            const auto min = current.rangeMin.value_or (number);
            const auto max = current.rangeMax.value_or (number);
            setRange (slot, kind == "range_min" ? number : min, kind == "range_max" ? number : max);
            return true;
        }

        why = "no such command "" + kind + """;
        return false;
    }

    bool Instance::bindPath (int slot, int deviceIndex, const std::string& path, std::size_t argument, std::string& why)
    {
        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()))
        {
            why = "no such device";
            return false;
        }

        const auto json = deviceTreeJson (deviceIndex, path);

        if (! json.has_value())
        {
            why = "the device "" + state.devices[static_cast<std::size_t> (deviceIndex)].name + "" has not served " + path + " (not reached, or no such path)";
            return false;
        }

        std::string error;
        const auto node = oscquery::Tree::parseNode (juce::JSON::parse (juce::String (*json)), path, error);

        if (! node.has_value())
        {
            why = path + ": " + error;
            return false;
        }

        const bool ok = node->argumentCount() > 1 ? bindAll (slot, *node, deviceIndex, why)
                                                  : bind (slot, *node, argument, deviceIndex, why);

        if (! ok)
            return false;

        if (const auto whole = deviceTreeJson (deviceIndex))
            if (const auto tree = oscquery::Tree::parse (*whole, error))
                setDeviceTreeHash (deviceIndex, tree->treeHash());

        return true;
    }

    int Instance::deviceIndexFor (const std::string& name)
    {
        for (std::size_t i = 0; i < state.devices.size(); ++i)
            if (state.devices[i].name == name)
                return static_cast<int> (i);

        /*  Not in the chunk: the registry's devices (another instance's) or
            the Finder's list name an address for it. */
        const auto status = juce::JSON::parse (juce::String (processStatusJson()));

        for (const char* list : { "devices", "found" })
        {
            if (const auto* entries = status.getProperty (list, {}).getArray())
            {
                for (const auto& d : *entries)
                {
                    if (d.getProperty ("name", "").toString().toStdString() != name)
                        continue;

                    DeviceRef ref;
                    ref.name = name;
                    ref.lastHost = d.getProperty ("host", "").toString().toStdString();
                    ref.lastPort = static_cast<int> (d.getProperty ("port", 0));

                    if (ref.lastHost.empty() || ref.lastPort <= 0)
                        continue;

                    if (const auto identity = d.getProperty ("identity_value", {}); identity.isString() && identity.toString().isNotEmpty())
                        ref.identityValue = identity.toString().toStdString();

                    return addDevice (ref);
                }
            }
        }

        return -1;
    }

    int Instance::clearStale()
    {
        if (handle == nullptr)
            return 0;

        const auto status = juce::JSON::parse (juce::String (statusJson()));
        std::vector<int> stale;

        if (const auto* slots = status.getProperty ("slots", {}).getArray())
        {
            for (const auto& s : *slots)
            {
                const auto word = s.getProperty ("state", "").toString();

                if (word == "missing" || word == "retyped" || word == "renamed")
                    stale.push_back (static_cast<int> (s.getProperty ("id", -1)));
            }
        }

        int cleared = 0;

        for (const auto slot : stale)
            if (slot >= 0 && slot < count && state.slots[slot].bound())
            {
                state.slots[slot].unbind();
                state.slotExtras.erase (slot);
                ++cleared;
            }

        if (cleared > 0)
            push();

        return cleared;
    }

    void Instance::clearRangeOverride (int slot, double deviceMin, double deviceMax)
    {
        if (slot < 0 || slot >= count || ! state.slots[slot].bound() || ! std::isfinite (deviceMin) || ! std::isfinite (deviceMax))
            return;

        auto& s = state.slots[slot];
        s.rangeMin = deviceMin;
        s.rangeMax = deviceMax;
        s.rangeOverridden = false;
        push();
    }

    bool Instance::bindPattern (int slot, int deviceIndex, const std::string& patternIn, std::size_t argument, std::string& why)
    {
        /*  A copy: the caller's reference may be the slot's own pathPattern,
            which bind() clears below (setInstanceNumber passes it). */
        const std::string pattern = patternIn;

        if (! hasInstanceToken (pattern))
            return bindPath (slot, deviceIndex, pattern, argument, why);

        if (! bindPath (slot, deviceIndex, substituteInstance (pattern, state.instanceNumber), argument, why))
            return false;

        /*  Every slot the path took (one, or consecutive for an fff) keeps the pattern. */
        if (slot >= 0 && slot < count)
            for (const int s : state.slots.groupOf (slot))
                if (s >= 0 && s < count)
                    state.slots[s].pathPattern = pattern;

        push();
        return true;
    }

    void Instance::setInstanceNumber (int number)
    {
        state.instanceNumber = std::clamp (number, 1, 9999);
        push();

        /*  Every patterned slot follows the number; one that cannot keeps
            its binding and the reason is kept past the pushes below. */
        std::vector<std::string> failures;
        std::vector<int> done;

        for (int slot = 0; slot < count; ++slot)
        {
            const auto s = state.slots[slot];   // a copy: bindPattern rewrites the slot

            if (s.pathPattern.empty() || ! s.bound() || std::find (done.begin(), done.end(), slot) != done.end())
                continue;

            for (const int g : state.slots.groupOf (slot))
                done.push_back (g);

            std::string why;

            if (! bindPattern (slot, s.deviceIndex, s.pathPattern, static_cast<std::size_t> (std::max (0, s.argIndex)), why))
                failures.push_back ("slot " + std::to_string (slot) + " kept at " + state.slots[slot].path + ": " + why);
        }

        lastWarnings.insert (lastWarnings.end(), failures.begin(), failures.end());
    }

    bool Instance::commandInstance (juce::int64 instanceId, int slot, const std::string& kind, const std::string& text, const std::string& text2, double number)
    {
        if (handle == nullptr || api == nullptr || instanceId < 0 || ! tableHas (api, &OqaCoreApi::instanceEnqueueCommandJson))
            return false;

        auto* o = new juce::DynamicObject();
        o->setProperty ("slot", slot);
        o->setProperty ("kind", juce::String (kind));
        o->setProperty ("text", juce::String (text));
        o->setProperty ("text2", juce::String (text2));
        o->setProperty ("number", number);
        const auto json = juce::JSON::toString (juce::var (o), true).toStdString();
        return api->instanceEnqueueCommandJson (static_cast<unsigned> (instanceId), json.c_str()) == 0;
    }

    void Instance::setActive (bool active)
    {
        if (handle != nullptr && api != nullptr && tableHas (api, &OqaCoreApi::instanceSetActive))
            api->instanceSetActive (handle, active ? 1 : 0);
    }

    //==============================================================================
    bool Instance::canReceive() const noexcept
    {
        return tableHas (api, &OqaCoreApi::instanceSetReceiver) && tableHas (api, &OqaCoreApi::instancePollReceived);
    }

    void Instance::wakeTrampoline (void* context)
    {
        /*  A registry thread, under the registry's lock: the instance is
            registered, so alive. The wake only signals, by contract. */
        auto* self = static_cast<Instance*> (context);

        if (self->receiveWake)
            self->receiveWake();
    }

    void Instance::registerWake()
    {
        if (handle != nullptr && canReceive())
            api->instanceSetReceiver (handle, receiveWake ? &Instance::wakeTrampoline : nullptr, this);
    }

    void Instance::setReceiveWake (std::function<void()> wake)
    {
        receiveWake = std::move (wake);
        registerWake();
    }

    int Instance::pollReceived (void (*apply) (int, float, void*), void* context) noexcept
    {
        if (handle == nullptr || ! canReceive())
            return 0;

        int slots[64];
        float values[64];
        int total = 0;

        for (;;)
        {
            const int n = api->instancePollReceived (handle, slots, values, 64);

            for (int i = 0; i < n; ++i)
            {
                if (slots[i] >= 0 && slots[i] < count)
                {
                    received[static_cast<std::size_t> (slots[i])].store (values[i], std::memory_order_relaxed);

                    if (apply != nullptr)
                        apply (slots[i], values[i], context);
                }
            }

            total += n;

            if (n < 64)
                return total;
        }
    }

    int Instance::pollReceived (const std::function<void (int, float)>& apply)
    {
        if (! apply)
            return pollReceived (nullptr, nullptr);

        auto trampoline = [] (int slot, float normalised, void* context)
        {
            (*static_cast<const std::function<void (int, float)>*> (context)) (slot, normalised);
        };

        return pollReceived (trampoline, const_cast<std::function<void (int, float)>*> (&apply));
    }

    std::optional<float> Instance::lastReceived (int slot) const noexcept
    {
        if (slot < 0 || slot >= count)
            return std::nullopt;

        const auto value = received[static_cast<std::size_t> (slot)].load (std::memory_order_relaxed);
        return std::isnan (value) ? std::nullopt : std::optional<float> (value);
    }

    //==============================================================================
    std::string Instance::takeError (char* text) const
    {
        if (text == nullptr)
            return {};

        std::string out (text);
        api->freeString (text);
        return out;
    }

    void Instance::push()
    {
        lastWarnings.clear();
        lastWarnings.insert (lastWarnings.end(), state.warnings.begin(), state.warnings.end());

        if (handle != nullptr)
        {
            char* error = nullptr;

            if (api->instanceSetBindings (handle, state.toJson().c_str(), &error) != 0)
                lastWarnings.push_back (takeError (error));

            /*  The registry's own words about the push. */
            if (char* status = api->instanceStatusJson (handle))
            {
                const auto var = juce::JSON::parse (juce::String (status));
                api->freeString (status);

                if (const auto* warnings = var.getProperty ("warnings", {}).getArray())
                    for (const auto& w : *warnings)
                        lastWarnings.push_back (w.toString().toStdString());
            }
        }

        postEverything.store (true, std::memory_order_release);
        bindingRevision.fetch_add (1, std::memory_order_acq_rel);
    }

    //==============================================================================
    bool Instance::loadState (std::string_view json, std::string& error)
    {
        auto loaded = StateChunk::fromJson (json, count, error);

        if (! loaded.has_value())
            return false;

        state = std::move (*loaded);
        push();
        return true;
    }

    std::string Instance::saveState()
    {
        /*  Where the registry has each device now: a confirmed or pointed
            address that is not the chunk's replaces it. */
        if (handle != nullptr)
        {
            const auto status = juce::JSON::parse (juce::String (statusJson()));

            if (const auto* devices = status.getProperty ("devices", {}).getArray())
            {
                for (int i = 0; i < devices->size() && i < static_cast<int> (state.devices.size()); ++i)
                {
                    const auto& d = (*devices)[i];

                    if (! d.isObject())
                        continue;

                    auto& ref = state.devices[static_cast<std::size_t> (i)];

                    /*  Timing set by hand anywhere in the process (6a-3) is
                        every chunk's on its next save, as the address is. */
                    if (static_cast<bool> (d.getProperty ("timing_by_hand", false)))
                    {
                        ref.honoursTimetags = static_cast<bool> (d.getProperty ("honours_timetags", ref.honoursTimetags));
                        ref.offsetMs = static_cast<double> (d.getProperty ("offset_ms", ref.offsetMs));
                        ref.timetagsByHand = true;
                    }
                    else if (d.getProperty ("timetags_origin", "").toString() == "device")
                    {
                        /*  The device's own word (the optional CLOCK, 6x-5):
                            saved as a chunk's value, not as a hand's, so the
                            device may say otherwise next time. */
                        ref.honoursTimetags = static_cast<bool> (d.getProperty ("honours_timetags", ref.honoursTimetags));
                    }

                    const auto resolution = d.getProperty ("resolution", "").toString();

                    if (resolution != "confirmed" && resolution != "pointed")
                        continue;

                    const auto host = d.getProperty ("host", "").toString().toStdString();
                    const auto port = static_cast<int> (d.getProperty ("port", 0));

                    if (host.empty() || port <= 0)
                        continue;

                    ref.lastHost = host;
                    ref.lastPort = port;

                    /*  The device's DEVICE_ID as the registry holds it (6x-4):
                        learnt from a device confirmed the old way, or from the
                        one the user pointed at. A session saved before the
                        field existed takes it here, on its first save. */
                    if (const auto id = d.getProperty ("device_id", {}); id.isString() && id.toString().isNotEmpty())
                        if (resolution == "pointed" || ! ref.deviceId.has_value())
                            ref.deviceId = id.toString().toStdString();
                }
            }
        }

        return state.toJson();
    }

    void Instance::setValues (const float* normalised, int valueCount)
    {
        state.values.assign (static_cast<std::size_t> (count), 0.0f);

        for (int i = 0; i < count && i < valueCount && normalised != nullptr; ++i)
            state.values[static_cast<std::size_t> (i)] = juce::jlimit (0.0f, 1.0f, normalised[i]);
    }

    int Instance::addDevice (const DeviceRef& device)
    {
        for (std::size_t i = 0; i < state.devices.size(); ++i)
            if (state.devices[i].name == device.name && state.devices[i].identityValue == device.identityValue)
            {
                const bool moved = state.devices[i].lastHost != device.lastHost || state.devices[i].lastPort != device.lastPort;
                state.devices[i].lastHost = device.lastHost;
                state.devices[i].lastPort = device.lastPort;

                if (! device.treeHash.empty())
                    state.devices[i].treeHash = device.treeHash;

                if (device.deviceId.has_value() && ! device.deviceId->empty())
                    state.devices[i].deviceId = device.deviceId;

                push();

                /*  The registry keeps the address it first had for a device
                    it knows; adding the device again elsewhere is pointing it. */
                if (moved && handle != nullptr && ! device.lastHost.empty() && device.lastPort > 0)
                    api->deviceSetAddress (device.name.c_str(), device.identityValue.value_or (std::string()).c_str(),
                                           device.lastHost.c_str(), device.lastPort);

                return static_cast<int> (i);
            }

        state.devices.push_back (device);
        push();
        return static_cast<int> (state.devices.size()) - 1;
    }

    bool Instance::pointDevice (int deviceIndex, const std::string& host, int port)
    {
        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()) || host.empty() || port <= 0)
            return false;

        auto& ref = state.devices[static_cast<std::size_t> (deviceIndex)];
        ref.lastHost = host;
        ref.lastPort = port;
        push();

        if (handle == nullptr)
            return true;

        return api->deviceSetAddress (ref.name.c_str(), ref.identityValue.value_or (std::string()).c_str(), host.c_str(), port) == 0;
    }

    void Instance::setDeviceIdentity (int deviceIndex, std::optional<std::string> path, std::optional<std::string> value)
    {
        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()))
            return;

        auto& ref = state.devices[static_cast<std::size_t> (deviceIndex)];

        if (path.has_value() && path->empty())
            path.reset();

        if (! path.has_value())
            value.reset();

        if (ref.identityPath == path && ref.identityValue == value)
            return;

        ref.identityPath = std::move (path);
        ref.identityValue = std::move (value);
        push();
    }

    bool Instance::canFind() const noexcept
    {
        return tableHas (api, &OqaCoreApi::finderEnsure) && tableHas (api, &OqaCoreApi::finderAddManual);
    }

    void Instance::finderEnsure()
    {
        if (canFind())
            api->finderEnsure();
    }

    void Instance::finderAddManual (const std::string& host, int port)
    {
        if (canFind() && ! host.empty() && port > 0)
            api->finderAddManual (host.c_str(), port);
    }

    void Instance::setDeviceTiming (int deviceIndex, bool honoursTimetags, double offsetMs)
    {
        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()))
            return;

        auto& ref = state.devices[static_cast<std::size_t> (deviceIndex)];
        ref.honoursTimetags = honoursTimetags;
        ref.offsetMs = std::isfinite (offsetMs) ? offsetMs : 0.0;
        ref.timetagsByHand = true;
        push();

        /*  By hand, process-wide: stands whatever chunk is pushed next. */
        if (handle != nullptr && api != nullptr && tableHas (api, &OqaCoreApi::deviceSetTiming))
            api->deviceSetTiming (ref.name.c_str(), ref.identityValue.value_or (std::string()).c_str(), honoursTimetags ? 1 : 0, ref.offsetMs);
    }

    bool Instance::setDeviceTimingByName (const std::string& name, const std::string& identityValue, bool honoursTimetags, double offsetMs)
    {
        if (handle == nullptr || api == nullptr || name.empty() || ! tableHas (api, &OqaCoreApi::deviceSetTiming))
            return false;

        return api->deviceSetTiming (name.c_str(), identityValue.c_str(), honoursTimetags ? 1 : 0, std::isfinite (offsetMs) ? offsetMs : 0.0) == 0;
    }

    bool Instance::pointDeviceByName (const std::string& name, const std::string& identityValue, const std::string& host, int port)
    {
        if (handle == nullptr || api == nullptr || name.empty() || host.empty() || port <= 0 || port > 65535)
            return false;

        return api->deviceSetAddress (name.c_str(), identityValue.c_str(), host.c_str(), port) == 0;
    }

    void Instance::setDeviceTreeHash (int deviceIndex, const std::string& hash)
    {
        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()))
            return;

        auto& device = state.devices[static_cast<std::size_t> (deviceIndex)];

        if (device.treeHash == hash)
            return;

        device.treeHash = hash;
        push();
    }

    bool Instance::bind (int slot, const oscquery::Node& node, std::size_t argument, int deviceIndex, std::string& why)
    {
        if (slot < 0 || slot >= count)
        {
            why = "no slot " + std::to_string (slot);
            return false;
        }

        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()))
        {
            why = "no device " + std::to_string (deviceIndex) + " in this instance";
            return false;
        }

        if (! state.slots[slot].bind (node, argument, deviceIndex, why))
            return false;

        state.slots[slot].pathPattern.clear();
        push();
        return true;
    }

    bool Instance::bindAll (int firstSlot, const oscquery::Node& node, int deviceIndex, std::string& why)
    {
        if (deviceIndex < 0 || deviceIndex >= static_cast<int> (state.devices.size()))
        {
            why = "no device " + std::to_string (deviceIndex) + " in this instance";
            return false;
        }

        if (! state.slots.bindAll (firstSlot, node, deviceIndex, why))
            return false;

        for (std::size_t i = 0; i < node.argumentCount(); ++i)
            if (const int s = firstSlot + static_cast<int> (i); s >= 0 && s < count)
                state.slots[s].pathPattern.clear();

        push();
        return true;
    }

    void Instance::unbind (int slot)
    {
        if (slot < 0 || slot >= count)
            return;

        state.slots[slot].unbind();
        state.slotExtras.erase (slot);
        push();
    }

    void Instance::setRange (int slot, double min, double max)
    {
        if (slot < 0 || slot >= count)
            return;

        state.slots[slot].setRange (min, max);
        push();
    }

    void Instance::setScaling (int slot, Scaling scaling)
    {
        if (slot < 0 || slot >= count)
            return;

        state.slots[slot].scaling = scaling;
        push();
    }

    void Instance::setDirection (int slot, Direction direction)
    {
        if (slot < 0 || slot >= count)
            return;

        state.slots[slot].direction = direction;
        push();
    }

    void Instance::setDisplayName (int slot, const std::string& name)
    {
        if (slot < 0 || slot >= count)
            return;

        state.slots[slot].displayName = name;
        push();
    }

    //==============================================================================
    void Instance::processBlock (const float* normalised, int valueCount) noexcept
    {
        if (handle == nullptr || normalised == nullptr)
            return;

        const auto n = std::min (valueCount, count);
        const bool everything = postEverything.exchange (false, std::memory_order_acq_rel);

        for (int i = 0; i < n; ++i)
        {
            const float value = normalised[i];
            auto& last = lastPosted[static_cast<std::size_t> (i)];

            if (everything || value != last)
            {
                last = value;

                /*  The host handing back what the device just sent (a slot
                    whose direction is both) is not a move to send. NaN, for a
                    slot that never received, compares false. */
                auto fromDevice = received[static_cast<std::size_t> (i)].load (std::memory_order_relaxed);

                if (! everything && std::abs (value - fromDevice) <= 1.0e-6f)
                    continue;

                api->instancePost (handle, i, value);

                /*  Once the host has sent something else, what the device said
                    before is no longer what it holds: forgotten, so that the
                    host moving BACK to that value is sent like any other move.
                    Kept, a toggle the device last sent as off could be switched
                    on and never off again (Julien Bayle's retest of ec6725c,
                    2.2). Only if no newer value arrived in between: the message
                    thread writes this too. */
                received[static_cast<std::size_t> (i)].compare_exchange_strong (fromDevice, notReceived, std::memory_order_relaxed);
            }
        }

        api->instanceEndBlock (handle);
    }

    //==============================================================================
    bool Instance::testSend (int slot, float normalised, std::string& error)
    {
        if (handle == nullptr)
        {
            error = "not connected to the registry";
            return false;
        }

        char* text = nullptr;

        if (api->instanceTestSend (handle, slot, normalised, &text) == 0)
            return true;

        error = takeError (text);
        return false;
    }

    std::optional<std::string> Instance::deviceTreeJson (int deviceIndex, const std::string& path) const
    {
        if (handle == nullptr)
            return std::nullopt;

        char* text = api->instanceDeviceTreeJson (handle, deviceIndex, path.c_str());

        if (text == nullptr)
            return std::nullopt;

        return takeError (text);
    }

    std::string Instance::processStatusJson() const
    {
        if (api == nullptr)
            return "{}";

        char* text = api->statusJson();
        return text != nullptr ? takeError (text) : std::string ("{}");
    }

    std::string Instance::statusJson() const
    {
        if (handle == nullptr)
            return "{}";

        return takeError (api->instanceStatusJson (handle));
    }

    std::string Instance::settingsJson() const
    {
        if (api == nullptr)
            return "{}";

        return takeError (api->settingsJson());
    }

    bool Instance::setSettingsJson (std::string_view json, std::string& error)
    {
        if (api == nullptr)
        {
            error = "no registry";
            return false;
        }

        char* text = nullptr;

        if (api->settingsSetJson (std::string (json).c_str(), &text) == 0)
            return true;

        error = takeError (text);
        return false;
    }
}
