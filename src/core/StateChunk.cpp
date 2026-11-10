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

#include <core/StateChunk.h>

#include <cmath>
#include <set>

namespace oqa::core
{
    namespace
    {
        /*  The field names, in one place. The chunk is read by other clients
            (export, Phase 5), so a rename here is a format change. */
        namespace key
        {
            constexpr const char* version         = "version";
            constexpr const char* devices         = "devices";
            constexpr const char* slots           = "slots";
            constexpr const char* values          = "values";
            constexpr const char* instance        = "instance";

            constexpr const char* name            = "name";
            constexpr const char* lastHost        = "last_host";
            constexpr const char* lastPort        = "last_port";
            constexpr const char* treeHash        = "tree_hash";
            constexpr const char* identityPath    = "identity_path";
            constexpr const char* identityValue   = "identity_value";
            constexpr const char* deviceId        = "device_id";
            constexpr const char* honoursTimetags = "honours_timetags";
            constexpr const char* timetagsByHand  = "timetags_by_hand";
            constexpr const char* offsetMs        = "offset_ms";

            constexpr const char* hostParamId     = "host_param_id";
            constexpr const char* deviceIndex     = "device_index";
            constexpr const char* path            = "path";
            constexpr const char* pathPattern     = "path_pattern";
            constexpr const char* argIndex        = "arg_index";
            constexpr const char* type            = "type";
            constexpr const char* range           = "range";
            constexpr const char* min             = "min";
            constexpr const char* max             = "max";
            constexpr const char* scaling         = "scaling";
            constexpr const char* direction       = "direction";
            constexpr const char* displayName     = "display_name";
            constexpr const char* rangeOverridden = "range_overridden";
            constexpr const char* unit            = "unit";
            constexpr const char* critical        = "critical";
        }

        const std::set<std::string> deviceKeys {
            key::name, key::lastHost, key::lastPort, key::treeHash, key::identityPath,
            key::identityValue, key::deviceId, key::honoursTimetags, key::timetagsByHand, key::offsetMs
        };

        const std::set<std::string> slotKeys {
            key::hostParamId, key::deviceIndex, key::path, key::pathPattern, key::argIndex, key::type, key::range,
            key::scaling, key::direction, key::displayName, key::rangeOverridden, key::unit, key::critical
        };

        const std::set<std::string> topKeys { key::version, key::devices, key::slots, key::values, key::instance };

        /*  juce::var::isObject() is true for an array too, and the DynamicObject
            behind an array is null (learnt in Phase 2). */
        juce::DynamicObject* objectOf (const juce::var& v)
        {
            return v.isArray() ? nullptr : v.getDynamicObject();
        }

        std::string stringOf (const juce::var& v)
        {
            return v.toString().toStdString();
        }

        std::optional<std::string> optionalStringOf (const juce::var& v)
        {
            if (v.isVoid() || v.isUndefined() || (! v.isString() && ! v.isInt() && ! v.isDouble()))
                return std::nullopt;

            return stringOf (v);
        }

        bool finiteNumber (const juce::var& v)
        {
            return (v.isInt() || v.isInt64() || v.isDouble()) && std::isfinite (static_cast<double> (v));
        }

        void keepExtras (const juce::DynamicObject& object, const std::set<std::string>& known,
                         juce::NamedValueSet& extras)
        {
            for (const auto& property : object.getProperties())
                if (known.count (property.name.toString().toStdString()) == 0)
                    extras.set (property.name, property.value);
        }

        void writeExtras (juce::DynamicObject& object, const juce::NamedValueSet& extras)
        {
            for (const auto& property : extras)
                object.setProperty (property.name, property.value);
        }

        //==============================================================================
        bool readDevice (const juce::var& v, DeviceRef& out, std::string& why)
        {
            auto* object = objectOf (v);

            if (object == nullptr)
            {
                why = "a devices[] entry is not an object";
                return false;
            }

            out.name     = stringOf (object->getProperty (key::name));
            out.lastHost = stringOf (object->getProperty (key::lastHost));
            out.lastPort = static_cast<int> (object->getProperty (key::lastPort));
            out.treeHash = stringOf (object->getProperty (key::treeHash));

            out.identityPath  = optionalStringOf (object->getProperty (key::identityPath));
            out.identityValue = optionalStringOf (object->getProperty (key::identityValue));

            //  A string and not empty, or not there: an identifier is text.
            if (const auto id = object->getProperty (key::deviceId); id.isString() && id.toString().isNotEmpty())
                out.deviceId = stringOf (id);

            out.honoursTimetags = static_cast<bool> (object->getProperty (key::honoursTimetags));

            const auto byHand = object->getProperty (key::timetagsByHand);
            out.timetagsByHand = byHand.isBool() && static_cast<bool> (byHand);

            const auto offset = object->getProperty (key::offsetMs);
            out.offsetMs = finiteNumber (offset) ? static_cast<double> (offset) : 0.0;

            keepExtras (*object, deviceKeys, out.extras);
            return true;
        }

        juce::var writeDevice (const DeviceRef& device)
        {
            auto* object = new juce::DynamicObject();

            object->setProperty (key::name,     juce::String (device.name));
            object->setProperty (key::lastHost, juce::String (device.lastHost));
            object->setProperty (key::lastPort, device.lastPort);
            object->setProperty (key::treeHash, juce::String (device.treeHash));

            if (device.identityPath.has_value())
                object->setProperty (key::identityPath, juce::String (*device.identityPath));

            if (device.identityValue.has_value())
                object->setProperty (key::identityValue, juce::String (*device.identityValue));

            if (device.deviceId.has_value() && ! device.deviceId->empty())
                object->setProperty (key::deviceId, juce::String (*device.deviceId));

            object->setProperty (key::honoursTimetags, device.honoursTimetags);

            if (device.timetagsByHand)
                object->setProperty (key::timetagsByHand, true);
            object->setProperty (key::offsetMs, device.offsetMs);

            writeExtras (*object, device.extras);
            return juce::var (object);
        }

        //==============================================================================
        /*  One slots[] entry into the table. False, with the reason, when the
            entry cannot be placed at all; a field this cannot honour takes its
            default and adds a warning. */
        bool readSlot (const juce::var& v, StateChunk& chunk, std::string& why)
        {
            auto* object = objectOf (v);

            if (object == nullptr)
            {
                why = "a slots[] entry is not an object";
                return false;
            }

            const auto idVar = object->getProperty (key::hostParamId);

            if (! idVar.isInt() && ! idVar.isInt64())
            {
                why = "a slots[] entry has no host_param_id";
                return false;
            }

            const int id = static_cast<int> (idVar);

            if (id < 0 || id >= chunk.slots.size())
            {
                why = "slot " + std::to_string (id) + " is beyond this plugin's " + std::to_string (chunk.slots.size())
                    + " slots; its binding is dropped and the host keeps its curve";
                return false;
            }

            const auto path = stringOf (object->getProperty (key::path));

            if (path.empty())
            {
                why = "slot " + std::to_string (id) + " has no path";
                return false;
            }

            const int deviceIndex = static_cast<int> (object->getProperty (key::deviceIndex));

            if (deviceIndex < 0 || deviceIndex >= static_cast<int> (chunk.devices.size()))
            {
                why = "slot " + std::to_string (id) + " names device " + std::to_string (deviceIndex)
                    + ", which the chunk does not list; its binding is dropped";
                return false;
            }

            Slot& slot = chunk.slots[id];
            slot.hostParamId = id;
            slot.deviceIndex = deviceIndex;
            slot.path        = path;
            slot.argIndex    = static_cast<int> (object->getProperty (key::argIndex));

            const auto type = stringOf (object->getProperty (key::type));

            if (type.size() == 1 && isBindableTag (type[0]))
            {
                slot.type = type[0];
            }
            else
            {
                slot.type = 'f';
                chunk.warnings.push_back ("slot " + std::to_string (id) + ": type \"" + type
                                          + "\" is not one this plugin binds; read as f");
            }

            slot.rangeMin.reset();
            slot.rangeMax.reset();

            if (auto* range = objectOf (object->getProperty (key::range)))
            {
                const auto lo = range->getProperty (key::min);
                const auto hi = range->getProperty (key::max);

                if (finiteNumber (lo) && finiteNumber (hi))
                {
                    slot.rangeMin = static_cast<double> (lo);
                    slot.rangeMax = static_cast<double> (hi);
                }
                else
                {
                    chunk.warnings.push_back ("slot " + std::to_string (id) + ": range has no finite min and max; left unknown");
                }
            }

            slot.rangeOverridden = static_cast<bool> (object->getProperty (key::rangeOverridden));

            const auto scalingName = stringOf (object->getProperty (key::scaling));

            if (const auto scaling = scalingFromName (scalingName))
            {
                slot.scaling = *scaling;
            }
            else
            {
                slot.scaling = Scaling::linear;

                if (! scalingName.empty())
                    chunk.warnings.push_back ("slot " + std::to_string (id) + ": scaling \"" + scalingName
                                              + "\" is unknown here; read as linear");
            }

            const auto directionName = stringOf (object->getProperty (key::direction));

            if (const auto direction = directionFromName (directionName))
            {
                slot.direction = *direction;
            }
            else
            {
                slot.direction = Direction::send;

                if (! directionName.empty())
                    chunk.warnings.push_back ("slot " + std::to_string (id) + ": direction \"" + directionName
                                              + "\" is unknown here; read as send");
            }

            slot.displayName = stringOf (object->getProperty (key::displayName));
            slot.unit        = stringOf (object->getProperty (key::unit));
            slot.critical    = static_cast<bool> (object->getProperty (key::critical));
            slot.pathPattern = stringOf (object->getProperty (key::pathPattern));

            juce::NamedValueSet extras;
            keepExtras (*object, slotKeys, extras);

            if (extras.size() > 0)
                chunk.slotExtras[id] = std::move (extras);

            return true;
        }

        juce::var writeSlot (const Slot& slot, const juce::NamedValueSet* extras)
        {
            auto* object = new juce::DynamicObject();

            object->setProperty (key::hostParamId, slot.hostParamId);
            object->setProperty (key::deviceIndex, slot.deviceIndex);
            object->setProperty (key::path,        juce::String (slot.path));

            /*  Only where there is one: a chunk without patterns reads as before (6d-2). */
            if (! slot.pathPattern.empty())
                object->setProperty (key::pathPattern, juce::String (slot.pathPattern));

            object->setProperty (key::argIndex,    slot.argIndex);
            object->setProperty (key::type,        juce::String::charToString (static_cast<juce::juce_wchar> (slot.type)));

            if (slot.rangeMin.has_value() && slot.rangeMax.has_value())
            {
                auto* range = new juce::DynamicObject();
                range->setProperty (key::min, *slot.rangeMin);
                range->setProperty (key::max, *slot.rangeMax);
                object->setProperty (key::range, juce::var (range));
            }
            else
            {
                object->setProperty (key::range, juce::var());
            }

            object->setProperty (key::scaling,         juce::String (scalingName (slot.scaling)));
            object->setProperty (key::direction,       juce::String (directionName (slot.direction)));
            object->setProperty (key::displayName,     juce::String (slot.displayName));
            object->setProperty (key::rangeOverridden, slot.rangeOverridden);
            object->setProperty (key::unit,            juce::String (slot.unit));
            object->setProperty (key::critical,        slot.critical);

            if (extras != nullptr)
                writeExtras (*object, *extras);

            return juce::var (object);
        }
    }

    //==============================================================================
    StateChunk::StateChunk (int slotCount)
        : slots (slotCount)
    {
    }

    std::optional<StateChunk> StateChunk::fromJson (std::string_view json, int slotCount, std::string& error)
    {
        juce::var root;
        const auto result = juce::JSON::parse (juce::String (json.data(), json.size()), root);

        if (result.failed())
        {
            error = "not JSON: " + result.getErrorMessage().toStdString();
            return std::nullopt;
        }

        return fromVar (root, slotCount, error);
    }

    std::optional<StateChunk> StateChunk::fromVar (const juce::var& root, int slotCount, std::string& error)
    {
        auto* object = objectOf (root);

        if (object == nullptr)
        {
            error = "a chunk is a JSON object";
            return std::nullopt;
        }

        const auto versionVar = object->getProperty (key::version);

        if (! versionVar.isInt() && ! versionVar.isInt64())
        {
            error = "no version field: not a chunk of this plugin's";
            return std::nullopt;
        }

        StateChunk chunk (slotCount);
        chunk.version = static_cast<int> (versionVar);

        if (chunk.version < 1)
        {
            error = "version " + std::to_string (chunk.version) + " is not one this plugin ever wrote";
            return std::nullopt;
        }

        if (chunk.version > currentVersion)
            chunk.warnings.push_back ("chunk version " + std::to_string (chunk.version) + " is newer than this plugin's "
                                      + std::to_string (currentVersion) + "; what it understands is loaded");

        /*  Version 1 (and, until a reader for 2 exists, anything newer, read
            as 1 plus warnings). A later version adds its own branch here and
            its fixture under tests/state/. */

        if (const auto* devices = object->getProperty (key::devices).getArray())
        {
            for (const auto& entry : *devices)
            {
                DeviceRef device;
                std::string why;

                if (readDevice (entry, device, why))
                    chunk.devices.push_back (std::move (device));
                else
                    chunk.warnings.push_back (why);
            }
        }

        if (const auto* slots = object->getProperty (key::slots).getArray())
        {
            for (const auto& entry : *slots)
            {
                std::string why;

                if (! readSlot (entry, chunk, why))
                    chunk.warnings.push_back (why);
            }
        }

        if (const auto* values = object->getProperty (key::values).getArray())
        {
            /*  One per slot, in order. A longer list is a bigger variant's:
                what fits is kept, the rest is this plugin's to drop, said
                once. A shorter one leaves the slots beyond it unsaid. */
            const auto n = std::min (values->size(), slotCount);

            for (int i = 0; i < n; ++i)
            {
                const auto& v = (*values)[i];
                chunk.values.push_back (v.isDouble() || v.isInt() || v.isInt64()
                                            ? juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (v)))
                                            : 0.0f);
            }

            if (values->size() > slotCount)
                chunk.warnings.push_back ("values: " + std::to_string (values->size()) + " for a table of "
                                          + std::to_string (slotCount) + " slots; the rest dropped");
        }

        if (const auto n = object->getProperty (key::instance); n.isInt() || n.isInt64() || n.isDouble())
            chunk.instanceNumber = std::clamp (static_cast<int> (n), 1, 9999);

        keepExtras (*object, topKeys, chunk.extras);
        return chunk;
    }

    juce::var StateChunk::toVar() const
    {
        auto* object = new juce::DynamicObject();
        object->setProperty (key::version, currentVersion);

        juce::Array<juce::var> deviceArray;

        for (const auto& device : devices)
            deviceArray.add (writeDevice (device));

        object->setProperty (key::devices, deviceArray);

        juce::Array<juce::var> slotArray;

        for (const auto& slot : slots.all())
        {
            if (! slot.bound())
                continue;

            const auto found = slotExtras.find (slot.hostParamId);
            slotArray.add (writeSlot (slot, found == slotExtras.end() ? nullptr : &found->second));
        }

        object->setProperty (key::slots, slotArray);

        if (! values.empty())
        {
            juce::Array<juce::var> valueArray;

            for (const auto v : values)
                valueArray.add (static_cast<double> (v));

            object->setProperty (key::values, valueArray);
        }

        if (instanceNumber != 1)
            object->setProperty (key::instance, instanceNumber);

        writeExtras (*object, extras);
        return juce::var (object);
    }

    std::string StateChunk::toJson() const
    {
        return juce::JSON::toString (toVar(), false).toStdString();
    }
}
