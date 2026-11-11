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

#include <oscquery/HostInfo.h>

namespace oqa::oscquery
{
    namespace
    {
        std::string stringOf (const juce::var& v)
        {
            return v.toString().toStdString();
        }

        /*  juce::var answers isObject() for an array as well, and the
            DynamicObject behind an array is null: a JSON object is one that
            is not an array. */
        bool isJsonObject (const juce::var& v) noexcept
        {
            return v.isObject() && ! v.isArray() && v.getDynamicObject() != nullptr;
        }

        std::optional<int> intOf (const juce::var& v)
        {
            if (v.isInt() || v.isInt64() || v.isDouble())
                return static_cast<int> (v);

            /*  A port written as a string is wrong and unambiguous; read it,
                because a device that does this is a device that does this. */
            if (v.isString())
            {
                const auto text = v.toString().trim();

                if (text.isNotEmpty() && text.containsOnly ("0123456789"))
                    return text.getIntValue();
            }

            return std::nullopt;
        }
    }

    //==============================================================================
    juce::var ClockInfo::toVar() const
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("SOURCE", juce::String (source));

        if (domain.has_value())
            object->setProperty ("DOMAIN", juce::String (*domain));

        object->setProperty ("SCHEDULING", scheduling);
        return juce::var (object);
    }

    //==============================================================================
    bool HostInfo::has (std::string_view extension) const
    {
        return extensions.find (std::string (extension)) != extensions.end();
    }

    std::string HostInfo::oscHost (const std::string& httpHost) const
    {
        return oscIp.has_value() && ! oscIp->empty() ? *oscIp : httpHost;
    }

    std::string HostInfo::websocketHost (const std::string& httpHost) const
    {
        return wsIp.has_value() && ! wsIp->empty() ? *wsIp : httpHost;
    }

    int HostInfo::websocketPort (int httpPort) const noexcept
    {
        return wsPort.has_value() && *wsPort > 0 ? *wsPort : httpPort;
    }

    bool HostInfo::offersWebsocket() const
    {
        return has ("LISTEN") || has ("PATH_CHANGED") || has ("PATH_ADDED")
            || has ("PATH_REMOVED") || has ("PATH_RENAMED");
    }

    //==============================================================================
    std::optional<HostInfo> parseHostInfo (const juce::var& object, std::string& error)
    {
        if (! isJsonObject (object))
        {
            error = "HOST_INFO is not a JSON object";
            return std::nullopt;
        }

        HostInfo info;
        info.raw = object;

        if (object.hasProperty ("NAME") && object["NAME"].isString())
            info.name = stringOf (object["NAME"]);
        else
            info.missing.emplace_back ("NAME");

        if (const auto port = intOf (object.getProperty ("OSC_PORT", {})))
            info.oscPort = *port;
        else
            info.missing.emplace_back ("OSC_PORT");

        if (object.hasProperty ("OSC_TRANSPORT") && object["OSC_TRANSPORT"].isString())
            info.oscTransport = object["OSC_TRANSPORT"].toString().trim().toUpperCase().toStdString();
        else
            info.missing.emplace_back ("OSC_TRANSPORT");

        if (object.hasProperty ("OSC_IP") && object["OSC_IP"].isString())
            info.oscIp = stringOf (object["OSC_IP"]);

        if (object.hasProperty ("WS_IP") && object["WS_IP"].isString())
            info.wsIp = stringOf (object["WS_IP"]);

        if (const auto port = intOf (object.getProperty ("WS_PORT", {})))
            info.wsPort = *port;

        /*  EXTENSIONS is an object of name -> bool in the proposal. An array of
            names is read too: it is what somebody writing a server from memory
            produces, and the meaning is not in doubt. */
        const auto extensions = object.getProperty ("EXTENSIONS", {});

        if (isJsonObject (extensions))
        {
            info.extensionsListed = true;

            for (const auto& property : extensions.getDynamicObject()->getProperties())
                if (static_cast<bool> (property.value))
                    info.extensions.insert (property.name.toString().toStdString());
        }
        else if (extensions.isArray())
        {
            info.extensionsListed = true;

            for (const auto& name : *extensions.getArray())
                if (name.isString())
                    info.extensions.insert (stringOf (name));
        }

        /*  The optional extensions, each only when declared (the header says
            why). DEVICE_ID is trimmed of surrounding blanks and otherwise
            taken as it is: the draft makes it a UUID, a client compares text. */
        if (info.has (deviceIdExtension) && object[deviceIdExtension].isString())
        {
            const auto id = object[deviceIdExtension].toString().trim();

            if (id.isNotEmpty())
                info.deviceId = id.toStdString();
        }

        if (const auto clock = object.getProperty (clockExtension, {});
            info.has (clockExtension) && isJsonObject (clock) && clock["SOURCE"].isString())
        {
            const auto source = clock["SOURCE"].toString().trim().toLowerCase();

            if (source.isNotEmpty())
            {
                ClockInfo read;
                read.source = source.toStdString();

                /*  A PTP domain is a number and an NTP server a name; the
                    draft makes DOMAIN a string, and a number is read as one. */
                if (const auto domain = clock.getProperty ("DOMAIN", {});
                    domain.isString() || domain.isInt() || domain.isInt64())
                    read.domain = stringOf (domain);

                //  Only a boolean true schedules: anything else claims nothing.
                const auto scheduling = clock.getProperty ("SCHEDULING", false);
                read.scheduling = scheduling.isBool() && static_cast<bool> (scheduling);

                info.clock = std::move (read);
            }
        }

        return info;
    }

    std::optional<HostInfo> parseHostInfo (std::string_view json, std::string& error)
    {
        juce::var parsed;
        const auto result = juce::JSON::parse (juce::String (std::string (json)), parsed);

        if (result.failed())
        {
            error = "HOST_INFO is not JSON: " + result.getErrorMessage().toStdString();
            return std::nullopt;
        }

        return parseHostInfo (parsed, error);
    }
}
