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
    ONE PLUGIN INSTANCE'S CORE: its slots and devices (the state chunk), and
    its side of the registry. The format shim owns one and calls it from
    three places; the editor is a client of it.

      the host's lifecycle   connect() in prepareToPlay (again on every call:
                             Logic restarts its hosting process and every
                             instance re-initialises), disconnect() in
                             releaseResources and the destructor
      the audio thread       processBlock (values, count): posts each bound,
                             active, sending slot whose value moved, then ends
                             the block; no allocation, no lock, one wake per
                             device touched
      the editor             bind, unbind, ranges, curves, directions, names;
                             the Test button; the device's tree; the status
      record (Phase 4)       the registry wakes the shim once per message
                             received; the shim polls the values on the
                             message thread and pushes them to the host

    The registry is reached through an OqaCoreApi table: the shared core
    library's when the loader found one, this binary's own otherwise. The
    instance does not know which; the status says.

    Every change of a binding pushes the whole chunk to the registry, which
    rebuilds the instance's plan. The audio thread never reads the chunk: it
    posts normalised values and the registry's plan says where they go.
*/

#include <core/StateChunk.h>
#include <core/registry/RegistryApi.h>
#include <core/rt/RtCheck.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oqa::core
{
    class Instance
    {
    public:
        /** `api` outlives the instance: the loader's table, or staticCoreApi(). */
        Instance (const OqaCoreApi* api, int slotCount, std::string label);
        ~Instance();

        Instance (const Instance&) = delete;
        Instance& operator= (const Instance&) = delete;

        int slotCount() const noexcept { return count; }

        //==============================================================================
        //  The host's lifecycle.

        /** Register with the registry and hand over the bindings. Safe to repeat. */
        void connect();

        /** Leave the registry. Safe to repeat. The destructor's; a shim calls setActive (false) when audio stops. */
        void disconnect();

        bool isConnected() const noexcept { return handle != nullptr; }

        /*  Registered for the plugin's life (Phase 6): the host stopped or
            restarted this instance's audio. Nothing is sent for an inactive
            instance (nothing is posted); the status and the Monitor say so. */
        void setActive (bool active);

        //==============================================================================
        //  The state. Message thread.

        const StateChunk& chunk() const noexcept { return state; }

        /** Replace everything from a saved chunk; false with the reason when it is not one. */
        bool loadState (std::string_view json, std::string& error);

        /*  The chunk as JSON. A device the registry has since found or been
            pointed at elsewhere is saved at that address (Phase 5b: "each
            instance writes the new address into its own chunk on the next
            save"), so the session reopens where the device was last. */
        std::string saveState();

        /*  The host's current value of every slot, 0-1, for the next
            saveState() (Phase 5b: the chunk carries `values`). The shim calls
            it before saving; after loadState() the shim reads chunk().values
            back into its parameters, where the list is as long as the table
            or shorter. */
        void setValues (const float* normalised, int valueCount);

        /*  A device for slots to point at: its index in the chunk, reusing an
            entry with the same name and identity value. A known device added
            again at another address is pointed there, process-wide (5b). */
        int addDevice (const DeviceRef& device);

        /*  The device's timing, by hand (6a-3): the chunk's device and, through
            the registry, every instance's on their next save. */
        void setDeviceTiming (int deviceIndex, bool honoursTimetags, double offsetMs);

        /*  The same by NAME and identity value, for the Monitor (6c), whose
            chunk has no devices: process-wide, every instance's chunk takes
            it on save. False when the registry has no such device. */
        bool setDeviceTimingByName (const std::string& name, const std::string& identityValue, bool honoursTimetags, double offsetMs);

        /*  Point a device by NAME and identity value (the Monitor's "pointing
            once": PRD, Re-identification); every instance referring to it
            follows. False when the registry has no such device. */
        bool pointDeviceByName (const std::string& name, const std::string& identityValue, const std::string& host, int port);

        //==============================================================================
        //  Re-identification (Phase 5b).

        /*  Point the device at an address by hand: the chunk, then the registry,
            where every instance referring to the device follows. The device is
            used whatever NAME answers there, with a warning. */
        bool pointDevice (int deviceIndex, const std::string& host, int port);

        /*  The read-only leaf whose value tells this device from another of the
            same NAME, and the value as the editor read it (identityText); nullopt
            clears both. Changes the device's key in the registry. */
        void setDeviceIdentity (int deviceIndex, std::optional<std::string> path, std::optional<std::string> value);

        /** Whether the registry this instance runs on has the Finder entries (an older shared library has not). */
        bool canFind() const noexcept;

        /** Start the registry's Finder (the editor opened). */
        void finderEnsure();

        /** A typed host and port for the Finder to probe and list under the status's `found`. */
        void finderAddManual (const std::string& host, int port);

        /*  The hash of the device's tree as the editor shows it, written at every
            bind and rebind (Phase 5a): what the registry compares the live tree
            with for the PRD's "the device changed since the bind" notice. */
        void setDeviceTreeHash (int deviceIndex, const std::string& hash);

        bool bind (int slot, const oscquery::Node& node, std::size_t argument, int deviceIndex, std::string& why);
        bool bindAll (int firstSlot, const oscquery::Node& node, int deviceIndex, std::string& why);
        void unbind (int slot);

        void setRange (int slot, double min, double max);
        void setScaling (int slot, Scaling scaling);
        void setDirection (int slot, Direction direction);
        void setDisplayName (int slot, const std::string& name);

        /** Moves on every change of a binding: the shim renames its parameters when it does. */
        std::uint64_t revision() const noexcept { return bindingRevision.load (std::memory_order_acquire); }

        //==============================================================================
        //  The audio thread.

        /*  `normalised[i]` is slot i's host value. Posts what the host MOVED
            since the last block, then ends the block. A value equal to the
            last one received from the device for that slot is not posted: a
            slot whose direction is both does not echo the device back to
            itself.

            WHAT IS NOT A MOVE (decided 2026-11-11, after Julien Bayle's retest
            of ec6725c, 2.1): connecting, binding, and a session being loaded.
            Opening a project used to push every slot's saved value onto the
            device, a live renderer included. Now the first block after a state
            load only notes where the host stands. The device is told when the
            host moves, and once for every slot when the transport starts
            (setPlaying), as a sequencer chases its controllers: without that a
            lane that holds its value from the first bar would never be sent. */
        void processBlock (const float* normalised, int valueCount) noexcept OQA_AUDIO_THREAD;

        /** The host's transport, told before each processBlock by a shim that knows it. Audio thread. */
        void setPlaying (bool playing) noexcept OQA_AUDIO_THREAD { playingNow = playing; }

        //==============================================================================
        //  Record (Phase 4). Values the devices stream for this instance's
        //  receiving slots wait in the registry; `wake` is called on a registry
        //  thread, at most once per message, and must only signal (the shim
        //  triggers an async update). pollReceived, on the message thread,
        //  drains them oldest first and remembers each slot's last.

        void setReceiveWake (std::function<void()> wake);

        /** Calls `apply (slot, normalised)` for each value waiting, oldest first; returns how many were applied. */
        int pollReceived (const std::function<void (int slot, float normalised)>& apply);

        /*  THE HOST HAS THE WORD WHILE IT IS MOVING A SLOT (decided 2026-11-11;
            the retest's 2.3 and 2.6). A value from the device for a slot the
            host moved less than hostHoldMs ago is not applied over the move:
            it is kept aside. When the hold ends, a device that ended up
            elsewhere is told the host's value again; one that agrees is left
            alone. Outside a hold the device is followed, as before.

            `hostNow`, when a shim on the message thread can give it, is what
            each host parameter reads this instant. A value there that the
            audio thread has not seen yet, and that is not the device's own
            last value, is a move still on its way to processBlock: applying
            the device's value now would erase it before it was ever sent. */
        int pollReceived (const float* hostNow, int hostCount,
                          void (*apply) (int slot, float normalised, void* context), void* context) noexcept;
        int pollReceived (const float* hostNow, int hostCount, const std::function<void (int slot, float normalised)>& apply);

        /** How long after its last move the host keeps the word on a slot. 200 ms by default; 0 turns the hold off. */
        void setHostHoldMs (int milliseconds) noexcept { hostHoldMs.store (std::max (0, milliseconds), std::memory_order_relaxed); }

        /*  The same with a function pointer and a context, for a caller on
            the audio thread (the CLAP shim drains in process and flush): no
            std::function, nothing allocated here. */
        int pollReceived (void (*apply) (int slot, float normalised, void* context), void* context) noexcept;

        /** The last value received for a slot, as 0-1; nullopt until one has. Any thread. */
        std::optional<float> lastReceived (int slot) const noexcept;

        /** Whether the registry this instance runs on has the receive entries (an older shared library has not). */
        bool canReceive() const noexcept;

        //==============================================================================
        //  The editor.

        bool testSend (int slot, float normalised, std::string& error);

        /** The device's tree at `path` as it serves it, or nullopt when not loaded. */
        std::optional<std::string> deviceTreeJson (int deviceIndex, const std::string& path = "/") const;

        /** The registry's view of this instance's devices, and its warnings, as JSON. */
        std::string statusJson() const;

        /** The registry's view of everything (every device, every instance, the settings, the Finder): the Monitor's. */
        std::string processStatusJson() const;

        //==============================================================================
        //  Commands (Phase 6b-3): writes to the plugin's own OSCQuery server,
        //  queued in the registry for this instance; `wake` is called on a
        //  registry thread and must only signal; pollCommands, on the main
        //  thread, applies them (bind, unbind, device, range, scaling,
        //  direction, display name, the instance number) and says how many,
        //  what could not be done going to warnings().

        void setCommandWake (std::function<void()> wake);
        int pollCommands();
        bool canCommand() const noexcept;

        /*  Bind a slot to a path by name, the node read from the device's tree
            as the registry serves it (the editor's rebind and the server's
            writes share this). A node with several arguments takes consecutive
            slots. False with the reason. */
        bool bindPath (int slot, int deviceIndex, const std::string& path, std::size_t argument, std::string& why);

        /** The chunk's device with this NAME, added from the registry's known or found devices when it is not yet there; -1 when nobody knows it. */
        int deviceIndexFor (const std::string& name);

        /*  A typed or pasted path, with or without the $instance token
            (PathPattern.h; Phase 6d-2): the token replaced by this instance's
            number, bound as bindPath does, and the pattern kept on the slot(s)
            so a change of number rebinds. A path without the token is bindPath
            with no pattern kept. */
        bool bindPattern (int slot, int deviceIndex, const std::string& pattern, std::size_t argument, std::string& why);

        /*  The number $instance stands for in this instance's paths (the
            chunk's `instance`). Setting it rebinds every slot with a pattern;
            a slot whose new path the device does not serve keeps its binding,
            and warnings() says which. */
        int instanceNumber() const noexcept { return state.instanceNumber; }
        void setInstanceNumber (int number);

        /*  A command for another instance of this process, by the id the
            process-wide status gives it: the same commands the plugin's own
            server queues (bind, unbind, device, display_name, scaling,
            direction, range_min, range_max, instance), so the Monitor applies
            a pattern across instances (6d-2). False when the registry has no
            such instance or no such entry. */
        bool commandInstance (juce::int64 instanceId, int slot, const std::string& kind, const std::string& text, const std::string& text2, double number);

        /*  Unbind every slot the registry calls missing, retyped or renamed
            (PRD, UX: "clear all stale"); not the unauthorised, which still
            receive. Returns how many. Never asks the host to drop a lane's
            automation (the 5b rule). */
        int clearStale();

        /*  Back to the device's own RANGE (the status's device_range_min/max,
            handed in by the caller who shows them): the slot's range set and
            `range_overridden` cleared. */
        void clearRangeOverride (int slot, double deviceMin, double deviceMax);

        /** The registry's process-wide settings as JSON (direct_send, receive_port, gesture_timeout_ms...). */
        std::string settingsJson() const;

        /** Fields present in `json` replace the registry's; false with the reason when it is not JSON. */
        bool setSettingsJson (std::string_view json, std::string& error);

        /** What the registry could not honour in the last push, in words. */
        const std::vector<std::string>& warnings() const noexcept { return lastWarnings; }

    private:
        void push();
        void registerWake();
        void registerCommandWake();
        std::string takeError (char* text) const;
        static void wakeTrampoline (void* context);
        static void commandWakeTrampoline (void* context);
        bool applyCommand (const juce::var& command, std::string& why);

        const OqaCoreApi* api;
        int count;
        std::string label;
        OqaInstance* handle = nullptr;

        StateChunk state;
        std::vector<std::string> lastWarnings;
        std::atomic<std::uint64_t> bindingRevision { 0 };

        //  The audio thread's own.
        std::vector<float> lastPosted;
        bool playingNow = false, wasPlaying = false;

        /** Set by a state load: the next block notes where the host stands and posts nothing. */
        std::atomic<bool> rebaseline { false };

        /*  Per slot, shared with the message thread: the host value the audio
            thread last saw, when the host last moved it (steady milliseconds),
            and the device's value kept aside during a hold (NaN: none). */
        std::unique_ptr<std::atomic<float>[]> seen;
        std::unique_ptr<std::atomic<std::int64_t>[]> lastMoveMs;
        std::unique_ptr<std::atomic<float>[]> deferred;
        std::atomic<int> hostHoldMs { 200 };

        //  Record: written on the message thread, read by the audio thread and the editor.
        std::function<void()> receiveWake;
        std::unique_ptr<std::atomic<float>[]> received;

        //  Commands (6b-3).
        std::function<void()> commandWake;
        std::map<int, std::string> pendingDevice;      ///< a device NAME written to an unbound slot, for its next path
    };
}
