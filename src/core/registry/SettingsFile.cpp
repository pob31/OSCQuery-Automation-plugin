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

#include <core/registry/SettingsFile.h>

#include <oscquery/Recents.h>

namespace oqa::core
{
    juce::File SettingsFile::defaultFile()
    {
        return oscquery::Recents::defaultFile().getSiblingFile ("OSCQuery Automation Plugin.registry.json");
    }

    juce::File SettingsFile::chosen()
    {
        if (const auto env = juce::SystemStats::getEnvironmentVariable ("OQA_SETTINGS_FILE", {}); env.isNotEmpty())
            return juce::File (env);

        return defaultFile();
    }

    juce::File SettingsFile::serverSeedFile (const juce::File& settingsFile)
    {
        if (settingsFile == juce::File())
            return {};

        return settingsFile.getSiblingFile (settingsFile.getFileName() + ".server-id");
    }

    std::string SettingsFile::serverSeed (const juce::File& settingsFile)
    {
        const auto file = serverSeedFile (settingsFile);

        if (file == juce::File())
            return {};

        if (file.existsAsFile())
            if (const auto kept = file.loadFileAsString().trim(); kept.isNotEmpty() && kept.length() <= 128)
                return kept.toStdString();

        const auto made = juce::Uuid().toDashedString();
        file.getParentDirectory().createDirectory();
        (void) file.replaceWithText (made + "\n");
        return made.toStdString();
    }

    std::optional<juce::var> SettingsFile::load (const juce::File& file, std::string& error)
    {
        error.clear();

        if (file == juce::File() || ! file.existsAsFile())
            return std::nullopt;

        juce::var parsed;
        const auto result = juce::JSON::parse (file.loadFileAsString(), parsed);

        if (result.failed())
        {
            error = file.getFullPathName().toStdString() + ": not JSON: " + result.getErrorMessage().toStdString();
            return std::nullopt;
        }

        if (! parsed.isObject() || parsed.isArray())
        {
            error = file.getFullPathName().toStdString() + ": not a JSON object";
            return std::nullopt;
        }

        return parsed;
    }

    bool SettingsFile::save (const juce::File& file, const juce::var& settings, std::string& error)
    {
        error.clear();

        if (file == juce::File())
        {
            error = "no settings file";
            return false;
        }

        if (const auto made = file.getParentDirectory().createDirectory(); made.failed())
        {
            error = file.getParentDirectory().getFullPathName().toStdString() + ": " + made.getErrorMessage().toStdString();
            return false;
        }

        if (! file.replaceWithText (juce::JSON::toString (settings, false)))
        {
            error = file.getFullPathName().toStdString() + ": could not be written";
            return false;
        }

        return true;
    }
}
