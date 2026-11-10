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
    THE DEVICE REGISTRY (PRD, Architecture: "a process-wide registry, so 64
    instances share one OSCQuery client and one socket per device"; devplan
    Phase 3). One entry per device the session refers to, keyed by NAME and the
    identity leaf's value, resolved to one host and port, owning one
    oscquery::Device, one sender and one network thread at realtime priority.
    Instances register, hand over their bindings, and from then on their audio
    threads post slot values into the device's ring and wake its thread once
    per block.

    THE SEND PIPELINE. A bound path of one instance is a Group: the message
    the device receives, every argument of the node, the ones being automated
    from their slots and the others from the last value known (the tree's at
    bind time). The audio thread stores the new normalised value into the
    group and pushes {instance, slot, value} into the device's ring, then an
    end-of-block marker per device and one wake. The network thread wakes,
    drains the ring, waits until every instance active in the last second has
    posted its marker or the gather deadline (0.5 ms) passes, then builds one
    bundle per device of every dirty group, splits at the MTU, stamps the
    immediate time tag or NTP now plus the device's offset, and sends: UDP
    through RawUdpSender, TCP through TcpSender when OSC_TRANSPORT says so,
    CRITICAL leaves on a UDP device as a websocket frame (spec L4). The
    per-path rate limiter sits before the bundle. The optional direct send
    (default off) encodes the group's message on the audio thread into the
    instance's own buffer and sends it at once; no bundle, no time tag.

    LIFETIMES. A plan (which slot goes to which group and device) is built
    for an instance on every change of its bindings and published through one
    atomic pointer; the old plans stay alive until the instance unregisters,
    because an audio thread may still be reading one. Groups are shared
    between the plan that made them and the device's list; the network thread
    copies that list under its lock at the start of a round and holds the
    copies for the round. A device entry lives as long as the registry: the
    sockets and the thread of a device the session once referred to are not
    worth the churn of closing on every releaseResources.

    THE RECEIVE PATH (Phase 4, 2026-11-09). Every group's path is LISTENed on
    its device, sending groups included: the stream is also what keeps the
    arguments no slot drives at the device's current value. A value arrives on
    the library's Device thread (never the audio thread): the groups at that
    path are found, the arguments no slot drives are refreshed, a driven
    argument echoing what was just sent is counted and dropped (the loop
    guard), and the rest is scaled to 0-1 by the receiving slot and pushed into
    its instance's incoming ring, with one wake to the instance's shim. Devices
    without LISTEN are served by one UDP receiver per process (4b).

    STALE SLOTS (Phase 5a, 2026-11-09). When a plan is built and the device's
    tree is in, every bound slot is checked against it: the path exists, is a
    leaf, has the slot's argument with the slot's TYPE, and ACCESS as reported
    to this client allows what the direction wants. A slot whose path is gone,
    retyped or moved (PATH_RENAMED remembered per device) is stale: it keeps
    its lane and its name in the host and joins no group, so nothing is sent
    or LISTENed for it; a slot the device lets this client only read drives
    nothing but still receives. The standing of every bound slot is in the
    instance's status (`slots`), with where a moved path went; the chunk's
    `tree_hash` against the live tree's is one warning line. PATH_REMOVED and
    PATH_RENAMED rebuild the plans live, as the tree landing does.

    RE-IDENTIFICATION (Phase 5b, 2026-11-09; PRD "Reopening a session"). A
    device entry drives nothing until the device at its address is confirmed:
    HOST_INFO's NAME is the chunk's and, when the chunk names an identity leaf,
    that leaf reads the stored value. A mismatch, or no answer, sets the entry
    searching: the resolver, one normal-priority thread per registry, owns the
    process-wide Finder (DNS-SD plus typed addresses), takes the candidates
    whose NAME is the chunk's, tells them apart by the identity leaf, then by
    tree hash, adopts the one left and leaves several or none unresolved with
    the candidates in the status for the user to pick from. A device pointed
    at by hand (deviceSetAddress) is used whatever NAME answers, with a warning
    line. The resolver snapshots under the registry's lock, does its HTTP
    outside it, and re-takes it to apply; its reads go through throwaway
    Devices, never the entry's.

    LOCK ORDER: the registry's lock, then a device entry's groups lock, or
    either alone; never the reverse. received() copies the matching groups
    under the groups lock, releases, and only then reaches the registry.

    This class is the implementation; RegistryApi.h is what a plugin binary
    calls through. The registry inside the shared library is the one every
    binary in the process shares; a binary that found no library uses its
    own, and says so.
*/

#include <core/StateChunk.h>

#include <oscquery/Finder.h>

#include <juce_core/juce_core.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oqa::core
{
    /*  THE PROCESS IS ENDING (Windows, Phase 6b-3): ExitProcess has killed
        every other thread before the DLLs' static destructors run, so the
        registry's destructor must not join threads or tear down sockets then;
        the OS reclaims them. The shared core library's DllMain says so on
        DLL_PROCESS_DETACH with lpReserved set; the CLAP module's too, for a
        registry of its own. POSIX exits with the threads alive and joins. */
    void noteProcessExiting() noexcept;
    bool processExiting() noexcept;

    struct RegistrySettings
    {
        bool directSend = false;           ///< encode and sendto on the audio thread; default off
        double defaultRateHz = 0.0;        ///< per-path rate limit; 0 is off
        double resendMs = 0.0;             ///< periodic resend of unchanged values; 0 is off
        int gatherDeadlineUs = 500;        ///< how long a device's thread waits for every instance's marker
        std::size_t mtu = 1400;            ///< bundles split above this
        int activeWindowMs = 1000;         ///< an instance that posted this recently is waited for

        //  Phase 4 (2026-11-09): record.
        int receivePort = 0;               ///< the UDP port for devices without LISTEN; 0 is ephemeral
        double loopGuardMs = 50.0;         ///< a value echoed by the device this soon after a send is ignored
        double gestureTimeoutMs = 200.0;   ///< the shim closes a host gesture after this much silence

        //  Phase 6 (2026-11-09): the plugin's own OSCQuery server.
        bool serverEnabled = true;         ///< serve and announce the slot table
        int serverPort = 0;                ///< its HTTP (and websocket) port; 0 is ephemeral, announced
        int serverOscPort = 0;             ///< its OSC_PORT, a UDP endpoint of its own; 0 is ephemeral
        std::string serverAllow;           ///< who may write: "" loopback only, "any", or addresses separated by commas
        std::string serverName;            ///< "" for the default, "OSCQuery Automation @ <machine> (<host app>)"

        /*  DNS-SD on the network (2026-11-10). False: the Finder neither browses
            nor announces, and nothing of this plugin's goes to the mDNS group;
            devices are added by typing host:port, and the server, still served,
            is reached the same way. For a network where multicast is not
            welcome, and for the validators, which load the plugin in hundreds
            of processes at once (docs/bench/2026-11-10-mdns-flood.md). */
        bool discoveryEnabled = true;

        juce::var toVar() const;

        /** Fields present in `v` replace this one's; the rest stay. */
        void applyVar (const juce::var& v);
    };

    //==============================================================================
    class Registry
    {
    public:
        class InstanceRecord;

        /** On the per-user settings file, or the one OQA_SETTINGS_FILE names (SettingsFile::chosen()). */
        Registry();

        /** On a settings file of the caller's choosing (tests); an empty File keeps nothing on disk. */
        explicit Registry (const juce::File& settingsFile);
        ~Registry();

        Registry (const Registry&) = delete;
        Registry& operator= (const Registry&) = delete;

        /** This binary's one registry. */
        static Registry& shared();

        //==============================================================================
        InstanceRecord* registerInstance (const std::string& label, int slotCount);
        void unregisterInstance (InstanceRecord* instance);

        /*  Registered for the plugin's life (Phase 6): the host stopped or
            restarted the instance's audio. For the status; the gather waits
            only for instances that posted recently anyway. */
        void setInstanceActive (InstanceRecord* instance, bool active);

        //==============================================================================
        //  Commands (6b-3): writes to the plugin's own server, queued for the
        //  instance they name, drained on the shim's main thread.
        void setCommandWake (InstanceRecord* instance, void (*wake) (void* context), void* context);
        std::string pollCommandsJson (InstanceRecord* instance);

        /*  A command queued for the instance of this id, as a server write
            would queue it: {slot, kind, text, text2, number} (6d-2, the
            Monitor's apply-across-instances). False when no instance has the
            id or the JSON is not a command. */
        bool enqueueCommandJson (std::uint32_t instanceId, const std::string& json);

        /*  The instance's bindings, from its chunk: devices are resolved (and
            started) and a plan built. False with the reason when the chunk
            names what cannot be bound at all; slots that cannot go are left
            out and the reason appended to `warnings`. Not for the audio thread. */
        bool setBindings (InstanceRecord* instance, const StateChunk& chunk, std::string& error);

        /** The same from the chunk's JSON, read against the instance's slot count. */
        bool setBindingsJson (InstanceRecord* instance, std::string_view chunkJson, std::string& error);

        /** What the last setBindings could not honour, in words. */
        std::vector<std::string> warnings (InstanceRecord* instance) const;

        //==============================================================================
        //  The audio thread.
        void post (InstanceRecord* instance, int slot, float normalised) noexcept;
        void endBlock (InstanceRecord* instance) noexcept;

        //==============================================================================
        //  Record (Phase 4). Values the device streams for an instance's receiving
        //  slots wait in the instance's incoming ring; `wake` is called on a
        //  registry thread, at most once per message delivered, and must only
        //  signal (the shim triggers an async update). pollReceived drains the
        //  ring into the caller's arrays, oldest first: nothing allocated, no lock.
        void setReceiver (InstanceRecord* instance, void (*wake) (void* context), void* context);
        int pollReceived (InstanceRecord* instance, int* slots, float* values, int capacity) noexcept;

        //==============================================================================
        bool testSend (InstanceRecord* instance, int slot, float normalised, std::string& error);

        /** The device's tree at `path`, as served; nullopt when not loaded or no such device. */
        std::optional<juce::var> deviceTree (InstanceRecord* instance, int deviceIndex, const std::string& path) const;

        /** Devices (and the settings) as JSON-able data; for an instance, its devices only. */
        juce::var status (InstanceRecord* instance = nullptr) const;

        bool setDeviceAddress (const std::string& name, const std::string& identityValue,
                               const std::string& host, int port);

        /*  The device's timing, set by hand (6a-3): stands whatever chunk is
            pushed next, and every instance's saveState() carries it. False for
            a device the registry does not have. */
        bool setDeviceTiming (const std::string& name, const std::string& identityValue,
                              bool honoursTimetags, double offsetMs);

        //==============================================================================
        //  The Finder (5b): one per process, in the resolver.

        /** Start browsing and probing now (an editor opened); otherwise it starts with the first search. */
        void finderEnsure();

        /** A typed host and port, probed and listed under `found` in the status, and a candidate for every search. */
        void finderAddManual (const std::string& host, int port);

        /** For tests: where DNS-SD is asked. Restarts the Finder when it runs. */
        void setFinderOptions (const oscquery::Finder::Options& options);

        /*  Replace the settings, tell every device, and write them to the
            settings file (Phase 6a-1); a file that cannot be written is said
            in the status's `settings_file`, the settings still applied. */
        void setSettings (const RegistrySettings& settings);
        RegistrySettings settings() const;

        /** Where the settings live on disk; an empty File when nowhere. */
        juce::File settingsFile() const;

        std::size_t deviceCount() const;
        std::size_t instanceCount() const;

        /** Stop every device's thread and forget every instance: for a process that is ending. */
        void shutdown();

    private:
        /** The plugin's own server started, stopped or re-pointed as the settings say (6b-2). */
        void reconcileServer();

        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
