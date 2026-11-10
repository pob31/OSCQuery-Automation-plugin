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

#include <core/Scaling.h>
#include <core/registry/Registry.h>
#include <core/registry/SettingsFile.h>
#include <core/server/PluginServer.h>
#include <core/server/ServerNamespace.h>

#include <deque>

#include <core/Identity.h>
#include <core/registry/ChangeRing.h>
#include <core/registry/Wake.h>
#include <core/rt/RtCheck.h>
#include <core/send/RateLimiter.h>
#include <core/send/TimeTags.h>

#include <oscquery/Device.h>
#include <oscquery/net/RawUdpSender.h>
#include <oscquery/net/TcpSender.h>
#include <oscquery/net/UdpEndpoint.h>
#include <oscquery/osc/Bundler.h>
#include <oscquery/osc/Codec.h>
#include <oscquery/osc/FixedEncoder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oqa::core
{
    using oscquery::osc::Value;

    namespace
    {
        constexpr std::size_t ringCapacity = 8192;
        constexpr std::size_t incomingCapacity = 4096;
        constexpr std::size_t datagramBufferBytes = 65536;
        constexpr std::uint16_t noSlot = 0xffff;

        /** The steady clock in nanoseconds: what the loop guard measures with, on any thread. */
        std::int64_t steadyNanos() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::string keyOf (const std::string& name, const std::string& identityValue)
        {
            return name + "\n" + identityValue;
        }

        std::string keyOf (const DeviceRef& ref)
        {
            return keyOf (ref.name, ref.identityValue.value_or (""));
        }

        /** The JSON of a tree's node at `path`, walking CONTENTS. */
        std::optional<juce::var> nodeAt (const juce::var& root, const std::string& path)
        {
            if (path.empty() || path == "/")
                return root;

            juce::var node = root;
            std::size_t start = 1;

            while (start <= path.size())
            {
                const auto end = path.find ('/', start);
                const auto segment = path.substr (start, end == std::string::npos ? std::string::npos : end - start);

                if (segment.empty())
                    return std::nullopt;

                const auto contents = node.getProperty ("CONTENTS", {});

                if (! contents.isObject() || contents.isArray())
                    return std::nullopt;

                node = contents.getProperty (juce::String (segment), {});

                if (node.isVoid())
                    return std::nullopt;

                if (end == std::string::npos)
                    break;

                start = end + 1;
            }

            return node;
        }

        //==============================================================================
        //  Re-identification (5b).

        /*  Where an entry stands with the device at its address. `checking`:
            no HOST_INFO yet, or not compared yet; `confirmed`: NAME and the
            identity leaf agree; `mismatch`: something else answers there;
            `searching`: nothing answers, or a mismatch, and no candidate by
            NAME has turned up; `unresolved`: candidates by NAME exist and the
            identity leaf and the tree hash leave none or several; `pointed`:
            the user pointed by hand, and whatever answers is used. */
        enum class Resolution { checking, confirmed, mismatch, searching, unresolved, pointed };

        const char* resolutionName (Resolution r) noexcept
        {
            switch (r)
            {
                case Resolution::checking:   return "checking";
                case Resolution::confirmed:  return "confirmed";
                case Resolution::mismatch:   return "mismatch";
                case Resolution::searching:  return "searching";
                case Resolution::unresolved: return "unresolved";
                case Resolution::pointed:    return "pointed";
            }

            return "checking";
        }

        /** A device found under the chunk's NAME while searching, and what told it apart. */
        struct CandidateInfo
        {
            std::string host;
            int port = 0;
            std::string identity;       // the identity leaf's value there, when read
            bool identityMatch = false;
            bool hashKnown = false;
            bool hashMatch = false;
        };

        constexpr int resolverHttpTimeoutMs = 3000;

        /*  Blocking reads through a throwaway Device, never started: the
            resolver's thread only. The identity leaf is read without a tree,
            so a number comes back as a double; identityText spells both sides
            the same way. */
        std::optional<std::string> readIdentity (const std::string& host, int port, const std::string& path)
        {
            oscquery::Device::Options options;
            options.host = host;
            options.port = port;
            options.httpTimeoutMs = resolverHttpTimeoutMs;
            options.openWebsocket = false;

            oscquery::Device probe (options);
            const auto values = probe.readValue (path);

            if (! values.has_value() || values->empty())
                return std::nullopt;

            return identityText (*values);
        }

        std::optional<std::string> fetchTreeHash (const std::string& host, int port)
        {
            oscquery::Device::Options options;
            options.host = host;
            options.port = port;
            options.httpTimeoutMs = resolverHttpTimeoutMs;
            options.openWebsocket = false;

            oscquery::Device probe (options);

            if (! probe.probe() || ! probe.fetchTree())
                return std::nullopt;

            std::string hash;
            probe.withTree ([&] (const oscquery::Tree& tree) { hash = tree.treeHash(); });
            return hash;
        }
    }

    //==============================================================================
    juce::var RegistrySettings::toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("direct_send", directSend);
        o->setProperty ("default_rate_hz", defaultRateHz);
        o->setProperty ("resend_ms", resendMs);
        o->setProperty ("gather_deadline_us", gatherDeadlineUs);
        o->setProperty ("mtu", static_cast<int> (mtu));
        o->setProperty ("active_window_ms", activeWindowMs);
        o->setProperty ("receive_port", receivePort);
        o->setProperty ("loop_guard_ms", loopGuardMs);
        o->setProperty ("gesture_timeout_ms", gestureTimeoutMs);
        o->setProperty ("server_enabled", serverEnabled);
        o->setProperty ("server_port", serverPort);
        o->setProperty ("server_osc_port", serverOscPort);
        o->setProperty ("server_allow", juce::String (serverAllow));
        o->setProperty ("server_name", juce::String (serverName));
        o->setProperty ("discovery_enabled", discoveryEnabled);
        return juce::var (o);
    }

    void RegistrySettings::applyVar (const juce::var& v)
    {
        if (! v.isObject() || v.isArray())
            return;

        if (v.hasProperty ("direct_send"))        directSend = static_cast<bool> (v["direct_send"]);
        if (v.hasProperty ("default_rate_hz"))    defaultRateHz = std::max (0.0, static_cast<double> (v["default_rate_hz"]));
        if (v.hasProperty ("resend_ms"))          resendMs = std::max (0.0, static_cast<double> (v["resend_ms"]));
        if (v.hasProperty ("gather_deadline_us")) gatherDeadlineUs = std::clamp (static_cast<int> (v["gather_deadline_us"]), 0, 20000);
        if (v.hasProperty ("mtu"))                mtu = static_cast<std::size_t> (std::clamp (static_cast<int> (v["mtu"]), 64, 65507));
        if (v.hasProperty ("active_window_ms"))   activeWindowMs = std::clamp (static_cast<int> (v["active_window_ms"]), 10, 60000);
        if (v.hasProperty ("receive_port"))       receivePort = std::clamp (static_cast<int> (v["receive_port"]), 0, 65535);
        if (v.hasProperty ("loop_guard_ms"))      loopGuardMs = std::clamp (static_cast<double> (v["loop_guard_ms"]), 0.0, 10000.0);
        if (v.hasProperty ("gesture_timeout_ms")) gestureTimeoutMs = std::clamp (static_cast<double> (v["gesture_timeout_ms"]), 10.0, 60000.0);
        if (v.hasProperty ("server_enabled"))     serverEnabled = static_cast<bool> (v["server_enabled"]);
        if (v.hasProperty ("server_port"))        serverPort = std::clamp (static_cast<int> (v["server_port"]), 0, 65535);
        if (v.hasProperty ("server_osc_port"))    serverOscPort = std::clamp (static_cast<int> (v["server_osc_port"]), 0, 65535);
        if (v.hasProperty ("server_allow"))       serverAllow = v["server_allow"].toString().trim().toStdString();
        if (v.hasProperty ("server_name"))        serverName = v["server_name"].toString().trim().toStdString();
        if (v.hasProperty ("discovery_enabled"))  discoveryEnabled = static_cast<bool> (v["discovery_enabled"]);
    }

    //==============================================================================
    class DeviceEntry;

    /*  One bound path of one instance: the message the device receives, and
        the one it streams back. */
    struct Group
    {
        std::uint32_t instanceId = 0;
        DeviceEntry* device = nullptr;
        std::string path;
        bool critical = false;
        std::size_t limiterKey = 0;

        /*  Per argument: the slot bound to it (a copy, for scaling), whether a
            sending slot drives it, and the receiving slot's host parameter ID
            (or -1). A slot whose direction is both is driven and received. */
        std::vector<Slot> slots;
        std::vector<bool> driven;
        std::vector<int> receiver;

        /** Per argument: the normalised value last posted by the audio thread (driven arguments). */
        std::unique_ptr<std::atomic<float>[]> known;

        /*  Per argument: the device's value as last known, in its units: the
            tree's at bind time, then whatever the device streams (written on
            the Device thread, read on the network thread). `tags` says how it
            goes on the wire; `initial` keeps the tree's value for an argument
            of a type no slot binds (s, b...), which never changes. */
        std::unique_ptr<std::atomic<double>[]> latest;
        std::string tags;
        std::vector<Value> initial;

        std::atomic<bool> dirty { false };

        /** When this group was last sent (steady clock): the loop guard's reference. 0: never. */
        std::atomic<std::int64_t> lastSentNanos { 0 };

        std::size_t argCount() const noexcept { return slots.size(); }

        /** The wire value of argument `i` now. */
        Value argument (std::size_t i) const noexcept
        {
            if (driven[i])
                return slots[i].deviceValue (known[i].load (std::memory_order_relaxed));

            const double v = latest[i].load (std::memory_order_relaxed);

            switch (tags[i])
            {
                case 'f': return Value::float32 (static_cast<float> (v));
                case 'i':
                {
                    constexpr double lo = static_cast<double> (std::numeric_limits<std::int32_t>::min());
                    constexpr double hi = static_cast<double> (std::numeric_limits<std::int32_t>::max());
                    return Value::int32 (static_cast<std::int32_t> (std::clamp (std::round (v), lo, hi)));
                }
                case 'T':
                case 'F': return Value::boolean (v >= 0.5);
                default:  return initial[i];
            }
        }
    };

    /** One received value on its way to an instance's slot. */
    struct Delivery
    {
        std::uint32_t instance = 0;
        int slot = 0;
        float normalised = 0.0f;
    };

    struct InstancePlan
    {
        std::vector<Group*> groupOfSlot;
        std::vector<std::uint16_t> argOfSlot;
        std::vector<DeviceEntry*> devices;
        std::vector<std::shared_ptr<Group>> groups;
    };

    /*  WHAT THE TREE SAYS OF A BOUND SLOT (Phase 5a; PRD "Reopening a session"
        step 3 and the failure-states table). Pending until the device's tree is
        in; then ok, or stale (the path is gone, retyped, or moved: the slot keeps
        its lane and joins no group), or unauthorised (ACCESS as reported to this
        client excludes what the slot's direction wants: it still receives when
        it may). Never stored in the chunk: a device repaired later un-stales the
        slot by itself. */
    enum class SlotState { pending, ok, missing, retyped, renamed, unauthorised, inactive };

    const char* slotStateName (SlotState s) noexcept
    {
        switch (s)
        {
            case SlotState::inactive:     return "inactive";
            case SlotState::pending:      return "pending";
            case SlotState::ok:           return "ok";
            case SlotState::missing:      return "missing";
            case SlotState::retyped:      return "retyped";
            case SlotState::renamed:      return "renamed";
            case SlotState::unauthorised: return "unauthorised";
        }

        return "pending";
    }

    struct SlotCheck
    {
        SlotState state = SlotState::pending;
        std::string detail;
        std::string renamedTo;
        bool mayDrive = true;       // a sending slot joins its group's driven arguments
        bool mayReceive = true;     // a receiving slot is delivered to

        /*  The device's own RANGE for the argument, when the tree has one (6a-2):
            what the editor greys beside a user-overridden range, and what a
            reset goes back to. The slot's range is the chunk's; this is the
            tree's. */
        std::optional<double> deviceRangeMin, deviceRangeMax;

        bool stale() const noexcept
        {
            return state == SlotState::missing || state == SlotState::retyped || state == SlotState::renamed;
        }
    };

    class Registry::InstanceRecord
    {
    public:
        std::uint32_t id = 0;
        std::string label;
        int slotCount = 0;

        /*  Registered for the plugin's life (decided 2026-11-09, Phase 6):
            false while the host has stopped the instance's audio. The gather
            already waits only for instances that posted within the window; this
            is for the status, the Monitor and the server. */
        std::atomic<bool> active { true };

        /** Each slot's last posted host value, for the server's `value` leaves (6b-2). Written by post(). */
        std::unique_ptr<std::atomic<float>[]> lastValues;

        /*  Commands from the server (6b-3), under the registry's lock, drained
            by the shim's main thread through pollCommandsJson; the wake only
            signals. */
        struct Command
        {
            int slot = -1;
            std::string kind, text, text2;
            double number = 0.0;
        };

        std::deque<Command> commands;
        void (*commandWake) (void* context) = nullptr;
        void* commandWakeContext = nullptr;

        std::atomic<const InstancePlan*> plan { nullptr };
        std::vector<std::unique_ptr<InstancePlan>> plans;       // every plan ever, the live one last

        std::vector<DeviceEntry*> deviceByIndex;                // the chunk's devices[], resolved (message thread)
        std::vector<std::string> warnings;
        std::unique_ptr<StateChunk> lastChunk;                  // what the plan was built from, for a rebuild
        std::map<int, SlotCheck> slotChecks;                    // per bound slot, by host parameter ID (5a)

        /** The direct-send path's buffer, this instance's own. */
        std::vector<std::uint8_t> directBuffer;
        std::atomic<std::uint64_t> directSent { 0 };

        /*  Record: values from the devices for this instance's receiving
            slots, drained by the shim through pollReceived; the wake it asked
            for, called under the registry's lock so it cannot outlive the
            instance. Full is a counted drop, as the send ring's is. */
        ChangeRing<incomingCapacity> incoming;
        void (*wake) (void* context) = nullptr;
        void* wakeContext = nullptr;
        std::atomic<std::uint64_t> received { 0 };
    };

    //==============================================================================
    class DeviceEntry final : private juce::Thread
    {
    public:
        DeviceEntry (std::string keyIn, DeviceRef refIn, const RegistrySettings& settingsIn,
                     std::function<void (std::vector<Delivery>&)> onReceivedIn,
                     std::function<void (DeviceEntry&)> onRebuildIn,
                     std::function<void()> onNeedsResolverIn)
            : juce::Thread ("oqa device " + juce::String (refIn.name)),
              key (std::move (keyIn)), ref (std::move (refIn)), settings (settingsIn),
              onRebuild (std::move (onRebuildIn)), onNeedsResolver (std::move (onNeedsResolverIn)),
              onReceived (std::move (onReceivedIn)), guardMs (settingsIn.loopGuardMs)
        {
            buffer.resize (datagramBufferBytes);
            openDevice();
            stopReplaced();

            /*  Realtime where the system allows it: woken about once a block,
                busy for tens of microseconds. The budget declared is small on
                purpose: macOS demotes a time-constraint thread that overruns
                its computation, and the gather below waits on the semaphore
                rather than spinning, so the thread never does. A high-priority
                thread where realtime is refused. */
            if (! startRealtimeThread (RealtimeOptions {}.withPriority (9).withPeriodMs (1.0)
                                                         .withProcessingTimeMs (0.05).withMaximumProcessingTimeMs (0.25)))
                startThread (Priority::highest);
        }

        ~DeviceEntry() override
        {
            signalThreadShouldExit();
            wake.signal();
            stopThread (3000);

            /*  Never joined under the device lock: the Device thread may be
                mid-delivery, waiting for the registry's lock, whose holder may
                want this device lock. */
            std::unique_ptr<oscquery::Device> old;

            {
                const std::lock_guard<std::mutex> guard (deviceLock);
                old = std::move (device);
            }

            if (old != nullptr)
                old->stop();
        }

        const std::string key;

        //==============================================================================
        //  The audio thread.
        ChangeRing<ringCapacity> ring;
        Wake wake;

        bool directSend (Registry::InstanceRecord& instance, Group& group) noexcept
        {
            if (! senderReady.load (std::memory_order_acquire) || tcp != nullptr || group.critical)
                return false;

            std::array<Value, 16> args;
            const auto n = std::min (group.argCount(), args.size());

            for (std::size_t i = 0; i < n; ++i)
                args[i] = group.argument (i);

            const auto size = oscquery::osc::encodeMessageInto (instance.directBuffer.data(), instance.directBuffer.size(),
                                                                group.path, args.data(), n);

            if (size == 0)
                return false;

            if (udp.send (instance.directBuffer.data(), size))
            {
                group.lastSentNanos.store (steadyNanos(), std::memory_order_relaxed);
                instance.directSent.fetch_add (1, std::memory_order_relaxed);
                messagesSent.fetch_add (1, std::memory_order_relaxed);
                return true;
            }

            return false;
        }

        //==============================================================================
        //  The message thread.

        /*  The instance's groups replace its old ones; the device is then told
            to LISTEN on every path any group holds and to IGNORE the ones none
            does. Under the groups lock first, the device lock after, never
            both: received() runs under the former alone. */
        void setGroups (std::uint32_t instanceId, std::vector<std::shared_ptr<Group>> incoming)
        {
            std::set<std::string> before, after;

            {
                const std::lock_guard<std::mutex> guard (groupsLock);

                for (const auto& g : allGroups)
                    before.insert (g->path);

                std::erase_if (allGroups, [instanceId] (const std::shared_ptr<Group>& g) { return g->instanceId == instanceId; });

                for (auto& g : incoming)
                    allGroups.push_back (std::move (g));

                for (const auto& g : allGroups)
                    after.insert (g->path);

                groupsRevision.fetch_add (1, std::memory_order_release);
            }

            /*  LISTEN waits for the device to be confirmed (5b): listenAll()
                then subscribes every group's path. IGNORE goes at once. */
            const bool confirmed = useDevice.load (std::memory_order_acquire);

            withDevice ([&] (oscquery::Device& d)
            {
                if (confirmed)
                    for (const auto& path : after)
                        if (before.count (path) == 0)
                            (void) d.listen (path);

                for (const auto& path : before)
                    if (after.count (path) == 0)
                        d.ignore (path);
            });
        }

        void removeGroups (std::uint32_t instanceId)
        {
            setGroups (instanceId, {});
        }

        void setSettings (const RegistrySettings& s)
        {
            const std::lock_guard<std::mutex> guard (settingsLock);
            settings = s;
            guardMs.store (s.loopGuardMs, std::memory_order_relaxed);
            settingsRevision.fetch_add (1, std::memory_order_release);
        }

        /*  Point the entry elsewhere: by the user's hand (used whatever NAME
            answers, with a warning) or by the resolver (confirmed like any
            address). The network thread opens the new Device. */
        void setAddress (const std::string& host, int port, bool byHand)
        {
            {
                const std::lock_guard<std::mutex> guard (settingsLock);
                ref.lastHost = host;
                ref.lastPort = port;
                handPointed = byHand;
                resolution = Resolution::checking;
                resolutionNoteText.clear();
                candidates.clear();
            }

            useDevice.store (false, std::memory_order_release);
            addressChanged.store (true, std::memory_order_release);
            wake.signal();
        }

        //==============================================================================
        //  Re-identification (5b). identify() and search() run on the resolver's
        //  thread, their HTTP through throwaway Devices, no registry lock held.

        bool needsIdentify() const noexcept { return identifyPending.load (std::memory_order_acquire); }

        bool needsSearch() const
        {
            const std::lock_guard<std::mutex> guard (settingsLock);
            return resolution == Resolution::mismatch || resolution == Resolution::searching || resolution == Resolution::unresolved;
        }

        /** The warning line the instance's status carries for this device, when there is one. */
        std::string resolutionNote() const
        {
            const std::lock_guard<std::mutex> guard (settingsLock);
            return resolutionNoteText;
        }

        /*  HOST_INFO is in at the current address: is it the device the chunk
            means? NAME must be the chunk's; the identity leaf, when the chunk
            names one, must read the stored value. A hand-pointed device passes
            whatever answers, with a note. */
        void identify()
        {
            if (! identifyPending.exchange (false, std::memory_order_acq_rel))
                return;

            std::optional<oscquery::HostInfo> info;
            std::string host;
            int port = 0;

            withDevice ([&] (const oscquery::Device& d)
            {
                info = d.hostInfo();
                host = d.options().host;
                port = d.options().port;
            });

            if (! info.has_value())
                return;

            const auto r = reference();
            const auto where = host + ":" + std::to_string (port);
            std::string live;
            std::string why;

            if (r.identityPath.has_value() && ! r.identityPath->empty())
            {
                const auto read = readIdentity (host, port, *r.identityPath);
                live = read.value_or (std::string());

                if (! read.has_value())
                    why = "its identity leaf " + *r.identityPath + " cannot be read";
                else if (live != r.identityValue.value_or (std::string()))
                    why = "its identity leaf " + *r.identityPath + " reads '" + live + "', not '" + r.identityValue.value_or (std::string()) + "'";
            }

            if (info->name != r.name)
                why = "it answers as '" + info->name + "', not '" + r.name + "'" + (why.empty() ? "" : "; " + why);

            bool pointed = false;
            Resolution next = Resolution::confirmed;
            std::string note;

            {
                const std::lock_guard<std::mutex> guard (settingsLock);
                pointed = handPointed;

                if (pointed)
                {
                    next = Resolution::pointed;

                    if (! why.empty())
                        note = "device \"" + r.name + "\" pointed at " + where + ": " + why;
                }
                else if (! why.empty())
                {
                    next = Resolution::mismatch;
                    note = "device \"" + r.name + "\" at " + where + ": " + why + "; looking for it";
                }

                resolution = next;
                answeredName = info->name;
                identityLive = live;
                resolutionNoteText = note;

                if (pointed)
                    candidates.clear();
            }

            if (next == Resolution::confirmed || next == Resolution::pointed)
            {
                useDevice.store (true, std::memory_order_release);
                listenAll();
                wake.signal();                  // the network thread opens the sender
            }
            else
            {
                useDevice.store (false, std::memory_order_release);

                if (onNeedsResolver)
                    onNeedsResolver();          // a search
            }

            /*  The slots' standing changed (pending to ok, or to the mismatch's words). */
            if (onRebuild)
                onRebuild (*this);
        }

        /*  Among what the Finder lists: the devices answering as the chunk's
            NAME, elsewhere than the address that failed; told apart by the
            identity leaf when the chunk names one, then by the tree hash when
            the chunk has one. One left is adopted; none or several stay for
            the user, listed in the status. */
        void search (const std::vector<oscquery::Finder::Candidate>& found)
        {
            DeviceRef r;

            {
                const std::lock_guard<std::mutex> guard (settingsLock);

                if (resolution != Resolution::mismatch && resolution != Resolution::searching && resolution != Resolution::unresolved)
                    return;

                r = ref;
            }

            std::vector<const oscquery::Finder::Candidate*> byName;

            for (const auto& c : found)
                if (c.reachable && c.hostInfo.has_value() && c.hostInfo->name == r.name
                    && ! (c.host == r.lastHost && c.port == r.lastPort))
                    byName.push_back (&c);

            if (byName.empty())
            {
                const std::lock_guard<std::mutex> guard (settingsLock);

                if (resolution == Resolution::unresolved)
                {
                    resolution = Resolution::searching;
                    candidates.clear();
                }

                return;
            }

            std::vector<CandidateInfo> infos;
            std::vector<std::size_t> matching;
            const bool byIdentity = r.identityPath.has_value() && ! r.identityPath->empty();

            for (const auto* c : byName)
            {
                CandidateInfo info;
                info.host = c->host;
                info.port = c->port;

                if (byIdentity)
                {
                    const auto read = readIdentity (c->host, c->port, *r.identityPath);
                    info.identity = read.value_or (std::string());
                    info.identityMatch = read.has_value() && info.identity == r.identityValue.value_or (std::string());
                }
                else
                {
                    info.identityMatch = true;
                }

                if (info.identityMatch)
                    matching.push_back (infos.size());

                infos.push_back (std::move (info));
            }

            if (matching.size() > 1 && ! r.treeHash.empty())
            {
                std::vector<std::size_t> byHash;

                for (const auto i : matching)
                {
                    auto& info = infos[i];
                    const auto hash = fetchTreeHash (info.host, info.port);
                    info.hashKnown = hash.has_value();
                    info.hashMatch = hash.has_value() && *hash == r.treeHash;

                    if (info.hashMatch)
                        byHash.push_back (i);
                }

                matching = byHash;
            }

            if (matching.size() == 1)
            {
                const auto& chosen = infos[matching.front()];
                setAddress (chosen.host, chosen.port, false);

                /*  Kept after the move: how the choice was made. */
                const std::lock_guard<std::mutex> guard (settingsLock);
                candidates = infos;
                return;
            }

            {
                const std::lock_guard<std::mutex> guard (settingsLock);
                resolution = Resolution::unresolved;
                candidates = infos;
                resolutionNoteText = "device \"" + r.name + "\": " + std::to_string (byName.size()) + " found under its name, "
                                   + (matching.empty() ? std::string ("none") : std::to_string (matching.size())) + " matching; point at one";
            }

            /*  The instances' warnings carry the note. */
            if (onRebuild)
                onRebuild (*this);
        }

        /** LISTEN on every group's path, once the device is confirmed. Groups lock, then device lock, never nested. */
        void listenAll()
        {
            std::set<std::string> paths;

            {
                const std::lock_guard<std::mutex> guard (groupsLock);

                for (const auto& g : allGroups)
                    paths.insert (g->path);
            }

            withDevice ([&] (oscquery::Device& d)
            {
                for (const auto& path : paths)
                    (void) d.listen (path);
            });
        }

        /*  The device's timing (PRD, Transport: "a per-device setting the user
            ticks in the Monitor", "set once per rig"). Set by hand (the
            Monitor, any editor) it stands, whatever chunk is pushed next;
            set by a chunk's push it is the last chunk's word until a hand
            sets it (6a-3). */
        void setTiming (bool honoursTimetags, double offsetMs, bool byHand)
        {
            const std::lock_guard<std::mutex> guard (settingsLock);

            if (! byHand && timingByHand)
                return;

            ref.honoursTimetags = honoursTimetags;
            ref.offsetMs = std::isfinite (offsetMs) ? offsetMs : 0.0;
            timingByHand = timingByHand || byHand;
        }

        /** The chunk's hash of this device's tree at bind time, when the chunk has one. */
        void setTreeHash (const std::string& hash)
        {
            if (hash.empty())
                return;

            const std::lock_guard<std::mutex> guard (settingsLock);
            ref.treeHash = hash;
        }

        /** The live tree's hash, computed by the network thread when the tree lands; empty before. */
        std::string treeHash() const
        {
            const std::lock_guard<std::mutex> guard (settingsLock);
            return liveTreeHash;
        }

        /** Milliseconds since the device last said anything (HTTP, websocket or UDP); -1 never. */
        std::int64_t lastSeenMs() const noexcept
        {
            const auto seen = lastSeenNanos.load (std::memory_order_relaxed);
            return seen == 0 ? -1 : std::max<std::int64_t> (0, (steadyNanos() - seen) / 1000000);
        }

        //==============================================================================
        /*  THE CHECK OF A BOUND SLOT against the tree (5a). Under the device
            lock and the Device's own; the renames under their own lock, alone.
            Called under the registry's lock from applyBindings. */
        SlotCheck check (const Slot& slot) const
        {
            SlotCheck out;
            out.detail = "waiting for the tree";

            /*  Until the device is confirmed (5b) nothing is known of its tree
                worth acting on: pending, with the resolution's words. */
            if (! useDevice.load (std::memory_order_acquire))
            {
                const std::lock_guard<std::mutex> guard (settingsLock);

                switch (resolution)
                {
                    case Resolution::mismatch:   out.detail = "the device at the address answers as '" + answeredName + "'"; break;
                    case Resolution::searching:  out.detail = "the device is being looked for"; break;
                    case Resolution::unresolved: out.detail = "the device is unresolved: point at it"; break;
                    default:                     out.detail = "waiting for the device to be identified"; break;
                }

                return out;
            }

            withDevice ([&] (const oscquery::Device& d)
            {
                if (! d.treeLoaded())
                    return;

                d.withTree ([&] (const oscquery::Tree& tree)
                {
                    const auto* node = tree.find (slot.path);

                    if (node == nullptr)
                    {
                        if (! ancestorsLoaded (tree, slot.path))
                        {
                            out.detail = "the device has not served this part of its tree yet";
                            return;
                        }

                        const auto target = renamedTo (slot.path);

                        if (! target.empty())
                        {
                            out.state = SlotState::renamed;
                            out.renamedTo = target;
                            out.detail = "moved to " + target;
                        }
                        else
                        {
                            out.state = SlotState::missing;
                            out.detail = "the path is not in the tree";
                        }

                        return;
                    }

                    forgetRename (slot.path);

                    if (node->isContainer())
                    {
                        out.state = SlotState::missing;
                        out.detail = "the path is a container now";
                        return;
                    }

                    const auto arg = static_cast<std::size_t> (std::max (0, slot.argIndex));
                    const auto& type = *node->type;

                    if (arg >= type.size())
                    {
                        out.state = SlotState::retyped;
                        out.detail = "TYPE '" + type + "' has no argument " + std::to_string (arg);
                        return;
                    }

                    const auto isToggle = [] (char c) { return c == 'T' || c == 'F'; };

                    if (type[arg] != slot.type && ! (isToggle (type[arg]) && isToggle (slot.type)))
                    {
                        out.state = SlotState::retyped;
                        out.detail = std::string ("TYPE '") + slot.type + "' is now '" + type[arg] + "'";
                        return;
                    }

                    if (const auto view = node->argument (arg); view.range.has_value() && view.range->hasMinMax())
                    {
                        out.deviceRangeMin = *view.range->min;
                        out.deviceRangeMax = *view.range->max;
                    }

                    const auto access = std::to_string (node->effectiveAccess());
                    out.mayDrive = slot.sends() && node->bindableForSend();
                    out.mayReceive = slot.receives() && node->bindableForRecord();

                    if (slot.sends() && ! node->bindableForSend())
                    {
                        out.state = SlotState::unauthorised;
                        out.detail = "ACCESS " + access + ": read-only for this client";
                        return;
                    }

                    if (slot.receives() && ! slot.sends() && ! node->bindableForRecord())
                    {
                        out.state = SlotState::unauthorised;
                        out.detail = "ACCESS " + access + ": not readable for this client";
                        return;
                    }

                    out.state = SlotState::ok;
                    out.detail.clear();
                });
            });

            return out;
        }

        DeviceRef reference() const
        {
            const std::lock_guard<std::mutex> guard (settingsLock);
            return ref;
        }

        template <typename Fn>
        void withDevice (Fn&& fn) const
        {
            const std::lock_guard<std::mutex> guard (deviceLock);

            if (device != nullptr)
                fn (*device);
        }

        /** The tree's VALUE for every argument of `path`, when the tree is loaded. */
        std::vector<Value> initialValues (const std::string& path, std::size_t argCount) const
        {
            std::vector<Value> values (argCount, Value::float32 (0.0f));

            withDevice ([&] (const oscquery::Device& d)
            {
                if (! d.treeLoaded())
                    return;

                d.withTree ([&] (const oscquery::Tree& tree)
                {
                    if (const auto* node = tree.find (path))
                        for (std::size_t i = 0; i < argCount && i < node->argumentCount(); ++i)
                            if (const auto v = node->argument (i).value)
                                values[i] = *v;
                });
            });

            return values;
        }

        bool nodeIsCritical (const std::string& path) const
        {
            bool critical = false;

            withDevice ([&] (const oscquery::Device& d)
            {
                d.withTree ([&] (const oscquery::Tree& tree)
                {
                    if (const auto* node = tree.find (path))
                        critical = node->critical;
                });
            });

            return critical;
        }

        /** The node's TYPE when the tree is loaded and has it; empty otherwise. */
        std::string nodeTags (const std::string& path) const
        {
            std::string tags;

            withDevice ([&] (const oscquery::Device& d)
            {
                if (! d.treeLoaded())
                    return;

                d.withTree ([&] (const oscquery::Tree& tree)
                {
                    if (const auto* node = tree.find (path))
                        if (node->type.has_value())
                            tags = *node->type;
                });
            });

            return tags;
        }

        //==============================================================================
        //  For the registry's UDP receiver (4b).

        /** Whether any group of any instance sits at `path`. */
        bool hasGroupAt (const std::string& path) const
        {
            const std::lock_guard<std::mutex> guard (groupsLock);

            for (const auto& g : allGroups)
                if (g->path == path)
                    return true;

            return false;
        }

        /** True while the websocket streams: UDP feedback is not this device's way then. */
        bool isListening() const
        {
            bool out = false;
            withDevice ([&] (const oscquery::Device& d) { out = d.state() == oscquery::Device::State::Listening; });
            return out;
        }

        /** The address OSC is sent to, as resolved; empty until the sender is open. */
        std::string oscIp() const
        {
            const std::lock_guard<std::mutex> guard (settingsLock);
            return resolvedOscIp;
        }

        void receivedUdp (const std::string& path, const oscquery::osc::Values& values)
        {
            udpReceived.fetch_add (1, std::memory_order_relaxed);
            received (path, values);
        }

        void udpIgnoredWhileListening()
        {
            udpIgnored.fetch_add (1, std::memory_order_relaxed);
        }

        //==============================================================================
        /*  A VALUE FROM THE DEVICE, on the library's Device thread (LISTEN) or
            the UDP receiver's (4b). The groups at that path, copied under the
            groups lock alone; then, per argument: the value the device holds is
            remembered for the arguments no slot drives; a driven argument that
            echoes what this device was just sent, within the guard window, is
            counted and dropped; what a receiving slot wants is scaled to 0-1 and
            handed to the registry for its instance. */
        void received (const std::string& path, const oscquery::osc::Values& values)
        {
            std::vector<std::shared_ptr<Group>> matching;

            {
                const std::lock_guard<std::mutex> guard (groupsLock);

                for (const auto& g : allGroups)
                    if (g->path == path)
                        matching.push_back (g);
            }

            noteSeen();
            valuesReceived.fetch_add (1, std::memory_order_relaxed);

            if (matching.empty())
            {
                unmatchedReceived.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            const auto now = steadyNanos();
            const auto guardNanos = static_cast<std::int64_t> (guardMs.load (std::memory_order_relaxed) * 1.0e6);
            std::vector<Delivery> deliveries;

            for (const auto& g : matching)
            {
                const auto n = std::min (values.size(), g->argCount());

                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto& v = values[i];
                    const bool boolean = v.isBool();

                    if (! boolean && ! (v.isNumber() && ! v.isNonFinite()))
                        continue;

                    if (! g->driven[i])
                        g->latest[i].store (boolean ? (v.getBool() ? 1.0 : 0.0) : v.asDouble(), std::memory_order_relaxed);

                    if (g->receiver[i] < 0)
                        continue;

                    const auto host = g->slots[i].hostValue (v);

                    if (! host.has_value())
                        continue;

                    if (g->driven[i])
                    {
                        const auto sentAt = g->lastSentNanos.load (std::memory_order_relaxed);

                        if (sentAt != 0 && now - sentAt < guardNanos
                            && std::abs (*host - g->known[i].load (std::memory_order_relaxed)) <= 1.0e-4f)
                        {
                            loopGuarded.fetch_add (1, std::memory_order_relaxed);
                            continue;
                        }
                    }

                    deliveries.push_back ({ g->instanceId, g->receiver[i], *host });
                }
            }

            if (! deliveries.empty() && onReceived)
                onReceived (deliveries);
        }

        juce::var status() const
        {
            auto* o = new juce::DynamicObject();
            const auto r = reference();

            o->setProperty ("name", juce::String (r.name));
            o->setProperty ("identity_value", r.identityValue.has_value() ? juce::var (juce::String (*r.identityValue)) : juce::var());
            o->setProperty ("host", juce::String (r.lastHost));
            o->setProperty ("port", r.lastPort);
            o->setProperty ("honours_timetags", r.honoursTimetags);
            o->setProperty ("offset_ms", r.offsetMs);
            o->setProperty ("timing_by_hand", timingByHandNow());

            withDevice ([&] (const oscquery::Device& d)
            {
                const auto state = d.state();
                o->setProperty ("state", juce::String (oscquery::Device::stateName (state)));
                o->setProperty ("reachable", state != oscquery::Device::State::Unprobed
                                          && state != oscquery::Device::State::Unreachable
                                          && state != oscquery::Device::State::Closed);
                o->setProperty ("tree_loaded", d.treeLoaded());

                if (const auto info = d.hostInfo())
                {
                    o->setProperty ("osc_transport", juce::String (info->oscTransport));
                    o->setProperty ("osc_port", info->oscPort);
                }

                auto error = d.lastError();

                if (error.empty())
                {
                    const std::lock_guard<std::mutex> guard (settingsLock);
                    error = lastCallbackError;
                }

                o->setProperty ("last_error", juce::String (error));

                /*  Where record stands on this device: listening over the
                    websocket; unsupported (no LISTEN in EXTENSIONS: the UDP
                    receiver is the way, 4b); else pending. */
                const auto info = d.hostInfo();
                const char* listen = "pending";

                if (info.has_value() && ! info->has ("LISTEN"))
                    listen = "unsupported";
                else if (state == oscquery::Device::State::Listening)
                    listen = "listening";

                o->setProperty ("listen", listen);
            });

            /*  Re-identification's words (5a): the chunk's hash of the tree at
                bind time against the live tree's, and when the device last
                said anything. */
            {
                const std::lock_guard<std::mutex> guard (settingsLock);
                o->setProperty ("tree_hash", juce::String (r.treeHash));
                o->setProperty ("tree_hash_live", juce::String (liveTreeHash));
                o->setProperty ("tree_changed", ! r.treeHash.empty() && ! liveTreeHash.empty() && r.treeHash != liveTreeHash);
            }

            o->setProperty ("last_seen_ms", static_cast<juce::int64> (lastSeenMs()));

            /*  Where the entry stands with the device at its address (5b), and
                the devices found under its NAME while it is unresolved. */
            {
                const std::lock_guard<std::mutex> guard (settingsLock);
                o->setProperty ("resolution", resolutionName (resolution));
                o->setProperty ("answered_name", juce::String (answeredName));
                o->setProperty ("identity_path", r.identityPath.has_value() ? juce::var (juce::String (*r.identityPath)) : juce::var());
                o->setProperty ("identity_live", juce::String (identityLive));
                o->setProperty ("resolution_note", juce::String (resolutionNoteText));
                o->setProperty ("hand_pointed", handPointed);

                juce::Array<juce::var> list;

                for (const auto& c : candidates)
                {
                    auto* entry = new juce::DynamicObject();
                    entry->setProperty ("host", juce::String (c.host));
                    entry->setProperty ("port", c.port);
                    entry->setProperty ("identity", juce::String (c.identity));
                    entry->setProperty ("identity_match", c.identityMatch);
                    entry->setProperty ("hash_known", c.hashKnown);
                    entry->setProperty ("hash_match", c.hashMatch);
                    list.add (juce::var (entry));
                }

                o->setProperty ("candidates", list);
            }

            o->setProperty ("values_received", static_cast<juce::int64> (valuesReceived.load()));
            o->setProperty ("loop_guarded", static_cast<juce::int64> (loopGuarded.load()));
            o->setProperty ("unmatched_received", static_cast<juce::int64> (unmatchedReceived.load()));
            o->setProperty ("udp_received", static_cast<juce::int64> (udpReceived.load()));
            o->setProperty ("udp_ignored", static_cast<juce::int64> (udpIgnored.load()));

            o->setProperty ("sender_ready", senderReady.load());
            o->setProperty ("resolved", juce::String (udp.resolvedAddress()));
            o->setProperty ("bundles_sent", static_cast<juce::int64> (bundlesSent.load()));
            o->setProperty ("messages_sent", static_cast<juce::int64> (messagesSent.load()));
            o->setProperty ("datagrams_dropped", static_cast<juce::int64> (udp.dropped() + ring.dropped()));
            o->setProperty ("send_failures", static_cast<juce::int64> (udp.failures() + tcpFailures.load()));
            o->setProperty ("unsent_no_route", static_cast<juce::int64> (unsent.load()));
            o->setProperty ("websocket_sent", static_cast<juce::int64> (websocketSent.load()));
            o->setProperty ("rounds", static_cast<juce::int64> (rounds.load()));
            o->setProperty ("rate_limited", static_cast<juce::int64> (limiter.held()));
            o->setProperty ("resent", static_cast<juce::int64> (limiter.resent()));

            {
                const std::lock_guard<std::mutex> guard (groupsLock);
                o->setProperty ("groups", static_cast<int> (allGroups.size()));
            }

            {
                auto* rates = new juce::DynamicObject();
                rates->setProperty ("bundles_s", rateBundles.load (std::memory_order_relaxed));
                rates->setProperty ("messages_s", rateMessages.load (std::memory_order_relaxed));
                rates->setProperty ("received_s", rateReceived.load (std::memory_order_relaxed));
                rates->setProperty ("udp_received_s", rateUdpReceived.load (std::memory_order_relaxed));
                o->setProperty ("rates", juce::var (rates));
            }

            {
                const auto at = lastErrorNanos.load (std::memory_order_relaxed);
                o->setProperty ("last_error_ms_ago", at == 0 ? juce::int64 (-1)
                                                             : juce::int64 (std::max<std::int64_t> (0, (unixNanosNow() - at) / 1000000)));
            }

            return juce::var (o);
        }

        /** The resolver's tick: the counters' change since the last sample, per second. */
        void sampleRates (std::chrono::steady_clock::time_point now)
        {
            const auto bundles = bundlesSent.load (std::memory_order_relaxed);
            const auto messages = messagesSent.load (std::memory_order_relaxed);
            const auto received = valuesReceived.load (std::memory_order_relaxed);
            const auto udpValues = udpReceived.load (std::memory_order_relaxed);

            if (ratePrevAt != std::chrono::steady_clock::time_point {})
            {
                const auto seconds = std::chrono::duration<double> (now - ratePrevAt).count();

                if (seconds > 0.05)
                {
                    rateBundles.store ((bundles - ratePrevBundles) / seconds, std::memory_order_relaxed);
                    rateMessages.store ((messages - ratePrevMessages) / seconds, std::memory_order_relaxed);
                    rateReceived.store ((received - ratePrevReceived) / seconds, std::memory_order_relaxed);
                    rateUdpReceived.store ((udpValues - ratePrevUdp) / seconds, std::memory_order_relaxed);
                }
            }

            ratePrevBundles = bundles;
            ratePrevMessages = messages;
            ratePrevReceived = received;
            ratePrevUdp = udpValues;
            ratePrevAt = now;
        }

    private:
        //==============================================================================
        void openDevice()
        {
            const auto r = reference();

            oscquery::Device::Options options;
            options.host = r.lastHost;
            options.port = r.lastPort;

            /*  The paths to LISTEN on, taken before the device lock: the
                groups lock is never taken under it. */
            std::set<std::string> paths;

            {
                const std::lock_guard<std::mutex> groupsGuard (groupsLock);

                for (const auto& g : allGroups)
                    paths.insert (g->path);
            }

            const std::lock_guard<std::mutex> guard (deviceLock);

            /*  The device being replaced is stopped by the caller through
                stopReplaced(), once this lock is released: stop() joins its
                thread, never under this lock (see the destructor). */
            stopping = std::move (device);
            device = std::make_unique<oscquery::Device> (options);

            /*  The tree landing (or changing) is noted for the network thread,
                which has the registry rebuild the plans that were made before
                it: a Device thread never takes the device lock, which the
                thread stopping that device holds. */
            oscquery::Device::Callbacks callbacks;
            callbacks.onValue = [this] (const std::string& path, const oscquery::osc::Values& values) { received (path, values); };
            callbacks.onState = [this] (oscquery::Device::State state)
            {
                using State = oscquery::Device::State;

                if (state == State::Reachable || state == State::TreeLoaded || state == State::Listening)
                {
                    noteSeen();

                    /*  HOST_INFO is in: the resolver compares NAME and the
                        identity leaf before anything is sent (5b). */
                    if (! identified.load (std::memory_order_acquire))
                    {
                        identifyPending.store (true, std::memory_order_release);

                        if (onNeedsResolver)
                            onNeedsResolver();
                    }
                }

                if (state == State::Unreachable)
                {
                    /*  Nothing answers: not this device's address any more,
                        until it answers again. The resolver searches. */
                    {
                        const std::lock_guard<std::mutex> guard (settingsLock);

                        if (resolution != Resolution::unresolved)
                            resolution = Resolution::searching;
                    }

                    useDevice.store (false, std::memory_order_release);
                    identified.store (false, std::memory_order_release);

                    if (onNeedsResolver)
                        onNeedsResolver();
                }

                if (state == State::TreeLoaded || state == State::Listening)
                {
                    treeArrived.store (true, std::memory_order_release);
                    wake.signal();
                }
            };
            callbacks.onTreeChanged = [this] (const std::string&)
            {
                noteSeen();
                treeArrived.store (true, std::memory_order_release);
                wake.signal();
            };

            /*  A path gone or moved (5a): the library has already patched its
                tree and dropped its subscriptions under the path; the plans
                are rebuilt so the slots there show stale, a rename remembered
                for the one-click rebind. On the Device's thread, which never
                takes the device lock: the renames have their own. */
            callbacks.onPathRemoved = [this] (const std::string&)
            {
                noteSeen();
                treeArrived.store (true, std::memory_order_release);
                wake.signal();
            };
            callbacks.onPathRenamed = [this] (const std::string& oldPath, const std::string& newPath)
            {
                noteSeen();

                {
                    const std::lock_guard<std::mutex> guard (renamesLock);
                    renames[oldPath] = newPath;
                }

                treeArrived.store (true, std::memory_order_release);
                wake.signal();
            };
            callbacks.onError = [this] (const std::string& message)
            {
                lastErrorNanos.store (unixNanosNow(), std::memory_order_relaxed);
                const std::lock_guard<std::mutex> guard (settingsLock);
                lastCallbackError = message;
            };
            /*  Nothing is LISTENed before the device is confirmed (5b):
                identify() subscribes every group's path then. */
            identified.store (false, std::memory_order_release);
            identifyPending.store (false, std::memory_order_release);
            useDevice.store (false, std::memory_order_release);
            (void) paths;

            device->setCallbacks (std::move (callbacks));
            device->start();

            senderReady.store (false, std::memory_order_release);
            udp.close();
            tcp.reset();

            const std::lock_guard<std::mutex> settingsGuard (settingsLock);
            resolvedOscIp.clear();
            liveTreeHash.clear();
            lastCallbackError.clear();
            answeredName.clear();
            identityLive.clear();
        }

        void noteSeen() noexcept
        {
            lastSeenNanos.store (steadyNanos(), std::memory_order_relaxed);
        }

        /*  Whether every container above `path` that exists has been served:
            a path missing under one the device sent without CONTENTS is not
            known missing. */
        static bool ancestorsLoaded (const oscquery::Tree& tree, const std::string& path)
        {
            auto parent = path;

            while (! parent.empty())
            {
                const auto slash = parent.rfind ('/');

                if (slash == std::string::npos)
                    break;

                parent = slash == 0 ? std::string ("/") : parent.substr (0, slash);

                if (const auto* node = tree.find (parent))
                    return node->loaded;

                if (parent == "/")
                    break;
            }

            return true;
        }

        /** Where `path` went, following PATH_RENAMED of it or of a container above it; empty when nowhere. */
        std::string renamedTo (const std::string& path) const
        {
            const std::lock_guard<std::mutex> guard (renamesLock);
            auto current = path;

            for (int hops = 0; hops < 16; ++hops)
            {
                std::string next;

                for (const auto& [from, to] : renames)
                {
                    if (current == from)
                    {
                        next = to;
                        break;
                    }

                    if (current.size() > from.size() && current.compare (0, from.size(), from) == 0 && current[from.size()] == '/')
                    {
                        next = to + current.substr (from.size());
                        break;
                    }
                }

                if (next.empty() || next == current)
                    break;

                current = next;
            }

            return current == path ? std::string() : current;
        }

        /** The tree holds `path` again: no rename of it is worth remembering. */
        void forgetRename (const std::string& path) const
        {
            const std::lock_guard<std::mutex> guard (renamesLock);
            renames.erase (path);
        }

        /** The network thread, when the tree lands or changes: the hash the status and the notice compare. */
        void refreshTreeHash()
        {
            std::string hash;

            withDevice ([&] (const oscquery::Device& d)
            {
                if (d.treeLoaded())
                    d.withTree ([&] (const oscquery::Tree& tree) { hash = tree.treeHash(); });
            });

            const std::lock_guard<std::mutex> guard (settingsLock);
            liveTreeHash = hash;
        }

        /** The device openDevice() replaced, for stopReplaced(). */
        std::unique_ptr<oscquery::Device> stopping;

        void stopReplaced()
        {
            if (stopping != nullptr)
                stopping->stop();

            stopping.reset();
        }

        /*  Once HOST_INFO is in: the UDP sender to OSC_IP (else the HTTP host)
            and OSC_PORT, or the TCP sender when OSC_TRANSPORT says so. */
        void ensureSender()
        {
            if (senderReady.load (std::memory_order_relaxed))
                return;

            //  Not until the device at the address is the one the chunk means (5b).
            if (! useDevice.load (std::memory_order_acquire))
                return;

            std::optional<oscquery::HostInfo> info;
            std::string host;

            withDevice ([&] (const oscquery::Device& d)
            {
                info = d.hostInfo();
                host = d.options().host;
            });

            if (! info.has_value() || info->oscPort <= 0)
                return;

            const auto oscHost = info->oscHost (host);
            std::string error;

            if (info->oscOverTcp())
            {
                oscquery::net::TcpSender::Options o;
                o.host = oscHost;
                o.port = info->oscPort;
                tcp = std::make_unique<oscquery::net::TcpSender> (o);

                if (! tcp->connect (error))
                {
                    tcp.reset();
                    return;
                }
            }
            else if (! udp.open (oscHost, info->oscPort, error))
            {
                return;
            }

            {
                /*  The address as resolved, less the port and any brackets:
                    what a datagram's sender is matched against (4b). */
                auto text = tcp != nullptr ? oscHost : udp.resolvedAddress();
                const auto colon = text.rfind (':');

                if (colon != std::string::npos && text.find (':') == colon)
                    text = text.substr (0, colon);
                else if (! text.empty() && text.front() == '[' && colon != std::string::npos && colon >= 2)
                    text = text.substr (1, colon - 2);

                const std::lock_guard<std::mutex> guard (settingsLock);
                resolvedOscIp = text;
            }

            senderReady.store (true, std::memory_order_release);
        }

        void refreshGroups()
        {
            const auto revision = groupsRevision.load (std::memory_order_acquire);

            if (revision == seenGroupsRevision)
                return;

            seenGroupsRevision = revision;

            const std::lock_guard<std::mutex> guard (groupsLock);
            groups = allGroups;

            std::size_t maxKey = 0;

            for (const auto& g : groups)
                maxKey = std::max (maxKey, g->limiterKey + 1);

            if (limiter.size() < maxKey)
                limiter.resize (maxKey);

            groupByKey.assign (maxKey, nullptr);

            for (const auto& g : groups)
                groupByKey[g->limiterKey] = g.get();
        }

        void refreshSettings()
        {
            const auto revision = settingsRevision.load (std::memory_order_acquire);

            if (revision == seenSettingsRevision)
                return;

            seenSettingsRevision = revision;

            const std::lock_guard<std::mutex> guard (settingsLock);
            local = settings;
            limiter.setOptions ({ local.defaultRateHz, local.resendMs });
        }

        /** Pops everything waiting; true if anything was. */
        bool drain (std::chrono::steady_clock::time_point now)
        {
            SlotChange change;
            bool any = false;

            while (ring.pop (change))
            {
                any = true;

                if (change.kind == SlotChange::endOfBlock)
                {
                    marked.insert (change.instance);
                    lastSeen[change.instance] = now;
                    continue;
                }

                lastSeen[change.instance] = now;

                for (const auto& g : groups)
                    if (g->instanceId == change.instance)
                        for (std::size_t i = 0; i < g->argCount(); ++i)
                            if (g->driven[i] && g->slots[i].hostParamId == change.slot)
                                g->dirty.store (true, std::memory_order_relaxed);
            }

            return any;
        }

        bool everyActiveMarked (std::chrono::steady_clock::time_point now) const
        {
            const auto window = std::chrono::milliseconds (local.activeWindowMs);

            for (const auto& [instance, seen] : lastSeen)
                if (now - seen <= window && marked.count (instance) == 0)
                    return false;

            return true;
        }

        void flush()
        {
            const auto nowNanos = unixNanosNow();

            limiter.flushDue (nowNanos, [this] (std::size_t key, double)
            {
                if (key < groupByKey.size() && groupByKey[key] != nullptr)
                    due.push_back (groupByKey[key]);
            });

            for (const auto& g : groups)
                if (g->dirty.exchange (false, std::memory_order_relaxed))
                    if (limiter.offer (g->limiterKey, 0.0, nowNanos))
                        due.push_back (g.get());

            if (due.empty())
                return;

            ensureSender();

            /*  The loop guard's reference: what goes out now may come back
                over LISTEN within the window. Stamped whether the send below
                succeeds or not; an unsent value echoes nothing. */
            const auto sentAt = steadyNanos();

            for (auto* g : due)
                g->lastSentNanos.store (sentAt, std::memory_order_relaxed);

            const bool timetags = ref.honoursTimetags;
            const auto tag = timetags ? ntpNow (ref.offsetMs) : immediateTimeTag;

            /*  CRITICAL on a device that is not TCP: a websocket frame each
                (spec draft L4); what the websocket cannot take goes on the wire. */
            wire.clear();

            for (auto* g : due)
            {
                if (g->critical && tcp == nullptr)
                {
                    std::vector<Value> args;

                    for (std::size_t i = 0; i < g->argCount(); ++i)
                        args.push_back (g->argument (i));

                    bool sent = false;
                    std::string error;

                    withDevice ([&] (oscquery::Device& d)
                    {
                        sent = d.sendOsc (oscquery::osc::Packet::message (g->path, std::move (args)), error);
                    });

                    if (sent)
                    {
                        websocketSent.fetch_add (1, std::memory_order_relaxed);
                        messagesSent.fetch_add (1, std::memory_order_relaxed);
                        continue;
                    }
                }

                wire.push_back (g);
            }

            due.clear();

            if (wire.empty())
                return;

            if (! senderReady.load (std::memory_order_relaxed))
            {
                unsent.fetch_add (wire.size(), std::memory_order_relaxed);
                return;
            }

            if (tcp != nullptr)
            {
                std::vector<oscquery::osc::Packet> packets;

                for (auto* g : wire)
                {
                    std::vector<Value> args;

                    for (std::size_t i = 0; i < g->argCount(); ++i)
                        args.push_back (g->argument (i));

                    packets.push_back (oscquery::osc::Packet::message (g->path, std::move (args)));
                }

                const auto bundled = oscquery::osc::bundle (packets, tag, datagramBufferBytes);

                for (const auto& datagram : bundled.datagrams)
                {
                    std::string error;

                    if (tcp->send (datagram, error))
                    {
                        bundlesSent.fetch_add (1, std::memory_order_relaxed);
                    }
                    else
                    {
                        tcpFailures.fetch_add (1, std::memory_order_relaxed);
                    }
                }

                messagesSent.fetch_add (packets.size() - bundled.refused.size(), std::memory_order_relaxed);
                return;
            }

            /*  UDP: one bundle per round, split at the MTU by refusal. */
            oscquery::osc::BundleWriter writer (buffer.data(), std::min (local.mtu, buffer.size()));
            writer.begin (tag);
            std::array<Value, 16> args;

            for (auto* g : wire)
            {
                const auto n = std::min (g->argCount(), args.size());

                for (std::size_t i = 0; i < n; ++i)
                    args[i] = g->argument (i);

                if (writer.add (g->path, args.data(), n))
                {
                    messagesSent.fetch_add (1, std::memory_order_relaxed);
                    continue;
                }

                if (! writer.empty())
                {
                    if (udp.send (writer.data(), writer.size()))
                        bundlesSent.fetch_add (1, std::memory_order_relaxed);

                    writer.begin (tag);

                    if (writer.add (g->path, args.data(), n))
                    {
                        messagesSent.fetch_add (1, std::memory_order_relaxed);
                        continue;
                    }
                }

                refused.fetch_add (1, std::memory_order_relaxed);       // too large for any bundle at this MTU
            }

            if (! writer.empty() && udp.send (writer.data(), writer.size()))
                bundlesSent.fetch_add (1, std::memory_order_relaxed);
        }

        void run() override
        {
            using namespace std::chrono;

            while (! threadShouldExit())
            {
                (void) wake.waitFor (milliseconds (100));
                wake.drain();

                if (threadShouldExit())
                    break;

                if (addressChanged.exchange (false, std::memory_order_acq_rel))
                {
                    openDevice();
                    stopReplaced();
                }

                if (treeArrived.exchange (false, std::memory_order_acq_rel) && onRebuild)
                {
                    const bool loaded = [this]
                    {
                        bool out = false;
                        withDevice ([&] (const oscquery::Device& d) { out = d.treeLoaded(); });
                        return out;
                    }();

                    if (loaded)
                    {
                        refreshTreeHash();
                        onRebuild (*this);
                    }
                }

                refreshSettings();
                refreshGroups();
                ensureSender();

                auto now = steady_clock::now();
                marked.clear();

                if (drain (now))
                {
                    /*  Gather: every instance active lately posts its marker
                        within the block, each with a wake; wait for the last
                        of them on the semaphore, never spinning, until the
                        deadline. */
                    const auto deadline = now + microseconds (local.gatherDeadlineUs);

                    while (! everyActiveMarked (now) && now < deadline)
                    {
                        if (! drain (now))
                            (void) wake.waitFor (deadline - now);

                        now = steady_clock::now();
                    }

                    rounds.fetch_add (1, std::memory_order_relaxed);
                }

                flush();

                /*  Forget instances silent for ten windows, so a closed
                    instance is not waited for. */
                const auto forget = milliseconds (local.activeWindowMs) * 10;

                for (auto it = lastSeen.begin(); it != lastSeen.end();)
                    it = (now - it->second > forget) ? lastSeen.erase (it) : std::next (it);
            }
        }

        //==============================================================================
        mutable std::mutex settingsLock;
        DeviceRef ref;
        RegistrySettings settings;
        RegistrySettings local;
        std::atomic<std::uint64_t> settingsRevision { 1 };
        std::uint64_t seenSettingsRevision = 0;

        mutable std::mutex deviceLock;
        std::unique_ptr<oscquery::Device> device;
        std::atomic<bool> addressChanged { false };
        std::atomic<bool> treeArrived { false };
        std::function<void (DeviceEntry&)> onRebuild;          // the registry rebuilds this device's instances' plans
        std::function<void()> onNeedsResolver;                 // wakes the resolver's thread

        //  Re-identification (5b): under settingsLock unless atomic.
        Resolution resolution = Resolution::checking;
        std::string answeredName;
        std::string identityLive;
        std::string resolutionNoteText;
        bool handPointed = false;
        bool timingByHand = false;              // under settingsLock
        bool timingByHandNow() const { const std::lock_guard<std::mutex> guard (settingsLock); return timingByHand; }
        std::vector<CandidateInfo> candidates;
        std::atomic<bool> useDevice { false };                 // confirmed or pointed: send and LISTEN
        std::atomic<bool> identified { false };                // compared since this Device answered
        std::atomic<bool> identifyPending { false };

        oscquery::net::RawUdpSender udp;
        std::unique_ptr<oscquery::net::TcpSender> tcp;
        std::atomic<bool> senderReady { false };

        mutable std::mutex groupsLock;
        std::vector<std::shared_ptr<Group>> allGroups;
        std::atomic<std::uint64_t> groupsRevision { 1 };

        //  The network thread's own.
        std::uint64_t seenGroupsRevision = 0;
        std::vector<std::shared_ptr<Group>> groups;
        std::vector<Group*> groupByKey;
        std::vector<Group*> due;
        std::vector<Group*> wire;
        RateLimiter limiter;
        std::unordered_set<std::uint32_t> marked;
        std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> lastSeen;
        std::vector<std::uint8_t> buffer;

        std::atomic<std::uint64_t> bundlesSent { 0 };
        std::atomic<std::uint64_t> messagesSent { 0 };
        std::atomic<std::uint64_t> websocketSent { 0 };
        std::atomic<std::uint64_t> tcpFailures { 0 };
        std::atomic<std::uint64_t> unsent { 0 };
        std::atomic<std::uint64_t> refused { 0 };
        std::atomic<std::uint64_t> rounds { 0 };

        /*  Rates over the last second (6a-2), sampled by the resolver's tick
            from the counters above, so every reader (editors, the Monitor, the
            server) shows the same numbers. The previous sample is the tick's
            own; the rates are read anywhere. */
        std::uint64_t ratePrevBundles = 0, ratePrevMessages = 0, ratePrevReceived = 0, ratePrevUdp = 0;
        std::chrono::steady_clock::time_point ratePrevAt {};
        std::atomic<double> rateBundles { 0.0 }, rateMessages { 0.0 }, rateReceived { 0.0 }, rateUdpReceived { 0.0 };
        std::atomic<std::int64_t> lastErrorNanos { 0 };     ///< when the library last reported an error; 0 never

        //  Record.
        std::function<void (std::vector<Delivery>&)> onReceived;
        std::atomic<double> guardMs { 50.0 };
        std::atomic<std::uint64_t> valuesReceived { 0 };
        std::atomic<std::uint64_t> loopGuarded { 0 };
        std::atomic<std::uint64_t> unmatchedReceived { 0 };
        std::atomic<std::uint64_t> udpReceived { 0 };
        std::atomic<std::uint64_t> udpIgnored { 0 };
        std::string resolvedOscIp;              // under settingsLock

        //  Re-identification and stale slots (5a).
        std::string liveTreeHash;               // under settingsLock
        std::string lastCallbackError;          // under settingsLock
        std::atomic<std::int64_t> lastSeenNanos { 0 };
        mutable std::mutex renamesLock;
        mutable std::map<std::string, std::string> renames;     // PATH_RENAMED old -> new, under renamesLock
    };

    //==============================================================================
    struct Registry::Impl
    {
        mutable std::mutex lock;

        /*  The settings on disk (6a-1): read once here, written on every
            setSettings; what went wrong, if anything, for the status. */
        juce::File settingsFile;
        bool settingsLoaded = false;
        std::string settingsFileError;
        std::map<std::string, std::unique_ptr<DeviceEntry>> devices;
        std::map<std::uint32_t, std::unique_ptr<InstanceRecord>> instances;
        std::uint32_t nextInstanceId = 1;
        std::size_t nextLimiterKey = 0;
        RegistrySettings settings;
        std::atomic<bool> directSend { false };

        void applyBindings (InstanceRecord* instance, const StateChunk& chunk);
        void rebuildPlansFor (DeviceEntry* entry);

        //==============================================================================
        /*  THE RESOLVER (5b): one normal-priority thread, woken by the entries
            (HOST_INFO in, nothing answering) and by the editor, ticking every
            second besides. It identifies each entry that asks, and for the
            entries searching it owns the process-wide Finder and offers them
            its candidates. Entries live as long as the registry and shutdown()
            stops this thread first, so the pointers it snapshots hold. */
        struct Resolver final : private juce::Thread
        {
            explicit Resolver (Impl& ownerIn) : juce::Thread ("oqa resolver"), owner (ownerIn)
            {
                startThread (Priority::normal);
            }

            ~Resolver() override
            {
                stopNow();
            }

            void stopNow()
            {
                signalThreadShouldExit();
                wake.signal();
                stopThread (5000);

                const std::lock_guard<std::mutex> guard (finderLock);
                finder.stop();
                finderRunning = false;
            }

            void poke() { wake.signal(); }

            /*  After shutdown() the thread is gone; a host that re-inits the
                plugin binary (CLAP hosts deinit after a scan and init again
                to use the plugins) gets a resolver again on the first
                instance registered. A no-op while the thread runs. */
            void restart()
            {
                if (! isThreadRunning())
                    startThread (Priority::normal);
            }

            /*  The options the Finder starts with: the ones at hand, or, with
                DNS-SD turned off in the settings, the same with no browsing
                and nothing announced. Typed entries are still probed. Under
                finderLock. */
            oscquery::Finder::Options effectiveOptions() const
            {
                auto options = finderOptions;

                if (! discovery)
                {
                    options.browse = false;
                    options.discovery.announceAs.clear();
                }

                return options;
            }

            /** DNS-SD on or off (the settings'); the Finder, if it runs, starts again with it. */
            void setDiscovery (bool enabled)
            {
                const std::lock_guard<std::mutex> guard (finderLock);

                if (discovery == enabled)
                    return;

                discovery = enabled;

                if (finderRunning)
                {
                    finder.stop();
                    finderRunning = finder.start (effectiveOptions());
                }
            }

            bool discoveryOn() const
            {
                const std::lock_guard<std::mutex> guard (finderLock);
                return discovery;
            }

            /** Start the Finder with the options at hand; a no-op once it runs. Any thread but under no registry lock. */
            void ensureFinder()
            {
                const std::lock_guard<std::mutex> guard (finderLock);

                if (finderRunning)
                    return;

                finderRunning = finder.start (effectiveOptions());
            }

            void addManual (const std::string& host, int port)
            {
                ensureFinder();
                const std::lock_guard<std::mutex> guard (finderLock);
                finder.addManual (host, port);
                finder.refresh();
                wake.signal();
            }

            void setOptions (const oscquery::Finder::Options& options)
            {
                const std::lock_guard<std::mutex> guard (finderLock);
                finderOptions = options;
                finderOptions.discovery.announceAs = announceName;
                finderOptions.discovery.ownPort = announceHttpPort;
                finderOptions.discovery.oscPort = announceOscPort;

                if (finderRunning)
                {
                    finder.stop();
                    finderRunning = finder.start (effectiveOptions());
                }
            }

            /*  The plugin's own server announced through the one Discovery
                (6b-2): the Finder runs from here on, browsing too. An empty
                name stops announcing. */
            void setAnnounce (const std::string& name, int httpPort, int oscPort)
            {
                {
                    const std::lock_guard<std::mutex> guard (finderLock);
                    announceName = name;
                    announceHttpPort = httpPort;
                    announceOscPort = oscPort;
                    finderOptions.discovery.announceAs = name;
                    finderOptions.discovery.ownPort = httpPort;
                    finderOptions.discovery.oscPort = oscPort;

                    if (finderRunning)
                    {
                        finder.stop();
                        finderRunning = finder.start (effectiveOptions());
                        return;
                    }

                    //  Nothing to announce with DNS-SD off: the Finder is not started for it.
                    if (! discovery)
                        return;
                }

                if (! name.empty())
                    ensureFinder();
            }

            std::vector<oscquery::Finder::Candidate> found() const
            {
                const std::lock_guard<std::mutex> guard (finderLock);
                return finderRunning ? finder.candidates() : std::vector<oscquery::Finder::Candidate> {};
            }

            bool running() const
            {
                const std::lock_guard<std::mutex> guard (finderLock);
                return finderRunning;
            }

            void run() override
            {
                while (! threadShouldExit())
                {
                    (void) wake.waitFor (std::chrono::milliseconds (1000));
                    wake.drain();

                    if (threadShouldExit())
                        break;

                    tick();
                }
            }

            void tick()
            {
                std::vector<DeviceEntry*> entries;

                {
                    const std::lock_guard<std::mutex> guard (owner.lock);

                    for (auto& [key, entry] : owner.devices)
                        entries.push_back (entry.get());
                }

                bool anySearching = false;
                const auto now = std::chrono::steady_clock::now();

                for (auto* entry : entries)
                {
                    if (threadShouldExit())
                        return;

                    entry->sampleRates (now);
                    entry->identify();
                    anySearching = anySearching || entry->needsSearch();
                }

                if (! anySearching)
                    return;

                ensureFinder();
                const auto candidates = found();

                for (auto* entry : entries)
                {
                    if (threadShouldExit())
                        return;

                    if (entry->needsSearch())
                        entry->search (candidates);
                }
            }

            Impl& owner;
            Wake wake;
            mutable std::mutex finderLock;
            oscquery::Finder finder;
            oscquery::Finder::Options finderOptions;
            bool finderRunning = false;
            bool discovery = true;              // under finderLock: the settings' discovery_enabled
            std::string announceName;           // under finderLock
            int announceHttpPort = 0, announceOscPort = 0;
        };

        std::unique_ptr<Resolver> resolver;

        /*  THE PLUGIN'S OWN SERVER (6b-2): one per process, started when the
            settings say so, rebuilt from the status whenever something changed,
            stopped before the resolver at shutdown. Null until the registry is
            whole. */
        std::unique_ptr<server::PluginServer> server;
        RegistrySettings serverSettingsInUse;       // what the running server was started with, under lock

        void serverChanged() noexcept
        {
            if (server != nullptr)
                server->rebuildSoon();
        }

        //==============================================================================
        /*  THE UDP RECEIVER (4b; PRD: "the plugin opens a UDP listener and shows
            its port in the editor; the user enters that port in the device's
            own configuration"). One per process, started once any instance has
            a receiving slot or the setting names a port, bound to
            settings.receivePort (0: ephemeral), rebound when that changes,
            stopped at shutdown. A datagram's messages are routed to the devices
            holding a group at the path and not streaming over a websocket
            (LISTEN is their way); several of those are told apart by the
            sender's address against each device's resolved OSC address, and
            what is still ambiguous goes to all of them, counted. Started and
            stopped outside the registry's lock: its handler takes that lock. */
        oscquery::net::UdpEndpoint receiver;
        int receiverPort = -1;                              // the port it was started for; -1 never
        std::atomic<std::uint64_t> udpDatagrams { 0 };
        std::atomic<std::uint64_t> udpRefused { 0 };
        std::atomic<std::uint64_t> udpUnrouted { 0 };
        std::atomic<std::uint64_t> udpAmbiguous { 0 };

        bool anyReceivingSlot() const           // under lock
        {
            for (const auto& [id, instance] : instances)
                if (const auto* plan = instance->plan.load (std::memory_order_acquire))
                    for (const auto& g : plan->groups)
                        for (const auto r : g->receiver)
                            if (r >= 0)
                                return true;

            return false;
        }

        /** Decide under the lock, act outside it. Message thread. */
        void reconcileReceiver()
        {
            bool wanted = false;
            int port = 0;

            {
                const std::lock_guard<std::mutex> guard (lock);
                port = settings.receivePort;
                wanted = receiver.isRunning() || anyReceivingSlot() || port > 0;
            }

            if (! wanted)
                return;

            if (receiver.isRunning() && receiverPort == port)
                return;

            receiver.stop();
            receiverPort = port;
            (void) receiver.start (port, [this] (oscquery::net::Datagram datagram) { onDatagram (std::move (datagram)); });
        }

        void onDatagram (oscquery::net::Datagram datagram)
        {
            udpDatagrams.fetch_add (1, std::memory_order_relaxed);
            const auto decoded = oscquery::osc::decode (datagram.bytes.data(), datagram.bytes.size());

            if (! decoded.ok)
            {
                udpRefused.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            route (decoded.packet, datagram.senderIp);
        }

        void route (const oscquery::osc::Packet& packet, const std::string& senderIp)
        {
            if (packet.isBundle())
            {
                for (const auto& element : packet.elements)
                    route (element, senderIp);

                return;
            }

            std::vector<DeviceEntry*> candidates;
            std::vector<DeviceEntry*> listening;

            {
                const std::lock_guard<std::mutex> guard (lock);

                for (auto& [key, entry] : devices)
                {
                    if (! entry->hasGroupAt (packet.address))
                        continue;

                    (entry->isListening() ? listening : candidates).push_back (entry.get());
                }
            }

            if (candidates.empty())
            {
                if (listening.empty())
                    udpUnrouted.fetch_add (1, std::memory_order_relaxed);

                for (auto* entry : listening)
                    entry->udpIgnoredWhileListening();

                return;
            }

            if (candidates.size() > 1)
            {
                std::vector<DeviceEntry*> fromSender;

                for (auto* entry : candidates)
                    if (entry->oscIp() == senderIp)
                        fromSender.push_back (entry);

                if (! fromSender.empty())
                    candidates = fromSender;

                if (candidates.size() > 1)
                    udpAmbiguous.fetch_add (1, std::memory_order_relaxed);
            }

            /*  Outside the lock: a delivery takes it. Entries live as long as
                the registry, and shutdown stops this receiver first. */
            for (auto* entry : candidates)
                entry->receivedUdp (packet.address, packet.args);
        }

        /*  This host's own IPv4 addresses, for the user to type into the device
            beside the port. Windows lists the multicast groups it has joined
            among a machine's addresses (the library's Discovery found this):
            a group is not an interface. Link-local (169.254) stays, last: a
            venue network without DHCP lives there. */
        static juce::var ownAddresses()
        {
            std::vector<std::string> addresses;

            for (const auto& address : juce::IPAddress::getAllAddresses (false))
            {
                if (address.isNull() || address.address[0] >= 224)
                    continue;

                const auto text = address.toString().toStdString();

                if (text.empty() || text.rfind ("127.", 0) == 0)
                    continue;

                addresses.push_back (text);
            }

            std::sort (addresses.begin(), addresses.end(), [] (const std::string& x, const std::string& y)
            {
                const bool xLocal = x.rfind ("169.254.", 0) == 0;
                const bool yLocal = y.rfind ("169.254.", 0) == 0;
                return xLocal != yLocal ? yLocal : x < y;
            });
            addresses.erase (std::unique (addresses.begin(), addresses.end()), addresses.end());

            juce::Array<juce::var> out;

            for (const auto& text : addresses)
                out.add (juce::String (text));

            return out;
        }

        DeviceEntry& entryFor (const DeviceRef& ref)
        {
            const auto key = keyOf (ref);
            auto found = devices.find (key);

            if (found != devices.end())
            {
                found->second->setTiming (ref.honoursTimetags, ref.offsetMs, false);
                found->second->setTreeHash (ref.treeHash);
                return *found->second;
            }

            auto entry = std::make_unique<DeviceEntry> (key, ref, settings,
                                                        [this] (std::vector<Delivery>& deliveries) { deliver (deliveries); },
                                                        [this] (DeviceEntry& e) { rebuildPlansFor (&e); },
                                                        [this] { if (resolver != nullptr) resolver->poke(); });
            auto& out = *entry;
            devices.emplace (key, std::move (entry));
            return out;
        }

        /*  A WRITE TO THE PLUGIN'S OWN SERVER (6b-3), on the server's thread:
            the path names an instance and a slot (ServerNamespace.h); the
            leaf and the arguments make a command, queued for the instance and
            its wake signalled under the lock, as deliver() does for values. A
            path the namespace does not know is dropped: the server only passes
            writes to leaves it serves as writable. */
        void onServerWrite (const std::string& path, const oscquery::osc::Values& values)
        {
            juce::StringArray segments;
            segments.addTokens (juce::String (path), "/", "");
            segments.removeEmptyStrings();

            if (segments.size() < 3 || segments[0] != juce::String (server::ns::pluginRoot + 1))
                return;

            const auto id = static_cast<std::uint32_t> (segments[1].getLargeIntValue());
            InstanceRecord::Command command;

            const auto textAt = [&] (int i) -> std::string
            {
                return i < static_cast<int> (values.size()) && values[static_cast<std::size_t> (i)].isString()
                           ? values[static_cast<std::size_t> (i)].getString() : std::string();
            };
            const auto numberAt = [&] (int i) -> double
            {
                if (i >= static_cast<int> (values.size()))
                    return 0.0;

                const auto& v = values[static_cast<std::size_t> (i)];
                return v.isFloat32() ? v.getFloat32() : v.isFloat64() ? v.getFloat64()
                     : v.isInt32() ? v.getInt32() : v.isInt64() ? static_cast<double> (v.getInt64()) : 0.0;
            };

            if (segments.size() == 3 && segments[2] == juce::String (server::ns::instance))
            {
                command.kind = "instance";
                command.number = numberAt (0);
            }
            else if (segments.size() >= 5 && segments[2] == juce::String (server::ns::slot))
            {
                command.slot = segments[3].getIntValue();
                const auto leaf = segments[4];

                if (leaf == juce::String (server::ns::path))
                {
                    command.text = textAt (0);
                    command.text2 = textAt (1);
                    command.kind = command.text.empty() ? "unbind" : "bind";
                }
                else if (leaf == juce::String (server::ns::device))        { command.kind = "device"; command.text = textAt (0); }
                else if (leaf == juce::String (server::ns::displayName))   { command.kind = "display_name"; command.text = textAt (0); }
                else if (leaf == juce::String (server::ns::scaling))       { command.kind = "scaling"; command.text = textAt (0); }
                else if (leaf == juce::String (server::ns::direction))     { command.kind = "direction"; command.text = textAt (0); }
                else if (segments.size() >= 6 && leaf == juce::String (server::ns::range))
                {
                    command.kind = segments[5] == juce::String (server::ns::min) ? "range_min" : "range_max";
                    command.number = numberAt (0);
                }
                else
                    return;
            }
            else
                return;

            const std::lock_guard<std::mutex> guard (lock);
            const auto found = instances.find (id);

            if (found == instances.end())
                return;

            auto* instance = found->second.get();

            if (instance->commands.size() < 256)
                instance->commands.push_back (std::move (command));

            if (instance->commandWake != nullptr)
                instance->commandWake (instance->commandWakeContext);
        }

        /*  Received values to their instances' rings, one wake per instance
            touched. On a Device thread (or the UDP receiver's), under the
            registry's lock, so the instance and its wake are alive for the
            call; the wake only signals, by contract. */
        void deliver (std::vector<Delivery>& deliveries)
        {
            const std::lock_guard<std::mutex> guard (lock);
            InstanceRecord* last = nullptr;

            for (const auto& d : deliveries)
            {
                const auto found = instances.find (d.instance);

                if (found == instances.end())
                    continue;

                auto* instance = found->second.get();
                instance->incoming.push ({ d.instance, static_cast<std::uint16_t> (d.slot), SlotChange::value, d.normalised });
                instance->received.fetch_add (1, std::memory_order_relaxed);

                if (instance != last)
                {
                    if (instance->wake != nullptr)
                        instance->wake (instance->wakeContext);

                    last = instance;
                }
            }
        }
    };

    Registry::Registry() : Registry (SettingsFile::chosen()) {}

    Registry::Registry (const juce::File& settingsFile) : impl (std::make_unique<Impl>())
    {
        impl->settingsFile = settingsFile;

        if (const auto saved = SettingsFile::load (settingsFile, impl->settingsFileError))
        {
            impl->settings.applyVar (*saved);
            impl->directSend.store (impl->settings.directSend, std::memory_order_relaxed);
            impl->settingsLoaded = true;
        }

        impl->resolver = std::make_unique<Impl::Resolver> (*impl);
        impl->resolver->setDiscovery (impl->settings.discoveryEnabled);

        server::PluginServer::Hooks hooks;
        hooks.processStatus = [this] { return status(); };
        hooks.pullValues = [this] (std::vector<server::PluginServer::SlotValue>& out)
        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            for (const auto& [id, record] : impl->instances)
            {
                if (record->lastChunk == nullptr || record->lastValues == nullptr)
                    continue;

                for (const auto& slot : record->lastChunk->slots.all())
                    if (slot.bound() && slot.hostParamId >= 0 && slot.hostParamId < record->slotCount)
                        out.push_back ({ id, slot.hostParamId, record->lastValues[static_cast<std::size_t> (slot.hostParamId)].load (std::memory_order_relaxed) });
            }
        };
        hooks.announce = [this] (const std::string& name, int httpPort, int oscPort)
        {
            if (impl->resolver != nullptr)
                impl->resolver->setAnnounce (name, httpPort, oscPort);
        };
        hooks.unannounce = [this]
        {
            if (impl->resolver != nullptr)
                impl->resolver->setAnnounce ({}, 0, 0);
        };
        hooks.write = [this] (const std::string&, const std::string& path, const oscquery::osc::Values& values)
        {
            impl->onServerWrite (path, values);
        };
        impl->server = std::make_unique<server::PluginServer> (std::move (hooks));

        reconcileServer();
    }

    void Registry::reconcileServer()
    {
        RegistrySettings wanted;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            wanted = impl->settings;
        }

        if (impl->server == nullptr)
            return;

        const auto& inUse = impl->serverSettingsInUse;
        const bool restart = impl->server->isRunning() != wanted.serverEnabled
                             || (wanted.serverEnabled && (inUse.serverPort != wanted.serverPort
                                                          || inUse.serverOscPort != wanted.serverOscPort
                                                          || inUse.serverName != wanted.serverName));

        if (restart)
        {
            impl->server->stop();

            if (wanted.serverEnabled)
            {
                std::string error;
                (void) impl->server->start (wanted, error);       // a failure stays in the server's status
            }
        }
        else if (wanted.serverEnabled)
        {
            impl->server->setAllow (wanted.serverAllow);
        }

        const std::lock_guard<std::mutex> guard (impl->lock);
        impl->serverSettingsInUse = wanted;
    }

    namespace
    {
        std::atomic<bool> exiting { false };
    }

    void noteProcessExiting() noexcept   { exiting.store (true, std::memory_order_relaxed); }
    bool processExiting() noexcept       { return exiting.load (std::memory_order_relaxed); }

    Registry::~Registry()
    {
        /*  At a process's end on Windows the threads are gone already, some
            killed mid-lock; joining them would wait for ever and tearing the
            sockets down would fail fast. Everything is left to the OS, which
            is reclaiming the process anyway: the Impl is released, not freed. */
        if (processExiting())
        {
            (void) impl.release();
            return;
        }

        shutdown();
    }

    void Registry::shutdown()
    {
        /*  The resolver first (it holds entry pointers), then the server, the
            receiver, then instances (their plans point at devices), then the
            devices, each stopping its thread and its device. For a process that is
            ending, or a test that wants no thread left when main returns.
            Taken out from under the lock before they are destroyed: a device
            being stopped may be mid-delivery, waiting for that very lock. */
        /*  The server stops answering first, without unannouncing: at a
            process's end every request it would still answer runs amid static
            destruction (the NUC's SIGSEGV under clap-validator, 6f), and
            unannouncing would restart a running Finder, a thread made during
            static destruction (the Windows runner's fail-fast, 6b-3). The
            resolver, and with it the Finder and the DNS-SD record, go next. */
        if (impl->server != nullptr)
            impl->server->stop (false);

        if (impl->resolver != nullptr)
            impl->resolver->stopNow();

        impl->receiver.stop();
        impl->receiverPort = -1;

        std::map<std::uint32_t, std::unique_ptr<InstanceRecord>> instances;
        std::map<std::string, std::unique_ptr<DeviceEntry>> devices;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            instances.swap (impl->instances);
            devices.swap (impl->devices);
        }

        instances.clear();
        devices.clear();
    }

    Registry& Registry::shared()
    {
        static Registry registry;
        return registry;
    }

    //==============================================================================
    Registry::InstanceRecord* Registry::registerInstance (const std::string& label, int slotCount)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);

        if (impl->resolver != nullptr)
            impl->resolver->restart();

        auto record = std::make_unique<InstanceRecord>();
        record->id = impl->nextInstanceId++;
        record->label = label;
        record->slotCount = std::max (0, slotCount);
        record->directBuffer.resize (4096);
        record->lastValues = std::make_unique<std::atomic<float>[]> (static_cast<std::size_t> (std::max (1, record->slotCount)));

        for (int i = 0; i < record->slotCount; ++i)
            record->lastValues[static_cast<std::size_t> (i)].store (0.0f, std::memory_order_relaxed);

        auto empty = std::make_unique<InstancePlan>();
        empty->groupOfSlot.assign (static_cast<std::size_t> (record->slotCount), nullptr);
        empty->argOfSlot.assign (static_cast<std::size_t> (record->slotCount), noSlot);
        record->plan.store (empty.get(), std::memory_order_release);
        record->plans.push_back (std::move (empty));

        auto* out = record.get();
        impl->instances.emplace (out->id, std::move (record));
        impl->serverChanged();
        return out;
    }

    void Registry::setCommandWake (InstanceRecord* instance, void (*wake) (void* context), void* context)
    {
        if (instance == nullptr)
            return;

        const std::lock_guard<std::mutex> guard (impl->lock);
        instance->commandWake = wake;
        instance->commandWakeContext = context;
    }

    std::string Registry::pollCommandsJson (InstanceRecord* instance)
    {
        juce::Array<juce::var> list;

        if (instance != nullptr)
        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            while (! instance->commands.empty())
            {
                const auto& c = instance->commands.front();
                auto* o = new juce::DynamicObject();
                o->setProperty ("slot", c.slot);
                o->setProperty ("kind", juce::String (c.kind));
                o->setProperty ("text", juce::String (c.text));
                o->setProperty ("text2", juce::String (c.text2));
                o->setProperty ("number", c.number);
                list.add (juce::var (o));
                instance->commands.pop_front();
            }
        }

        return juce::JSON::toString (juce::var (list), false).toStdString();
    }

    bool Registry::enqueueCommandJson (std::uint32_t instanceId, const std::string& json)
    {
        const auto parsed = juce::JSON::parse (juce::String (json));

        if (parsed.getDynamicObject() == nullptr || ! parsed.hasProperty ("kind"))
            return false;

        InstanceRecord::Command command;
        command.slot = static_cast<int> (parsed.getProperty ("slot", -1));
        command.kind = parsed.getProperty ("kind", "").toString().toStdString();
        command.text = parsed.getProperty ("text", "").toString().toStdString();
        command.text2 = parsed.getProperty ("text2", "").toString().toStdString();
        command.number = static_cast<double> (parsed.getProperty ("number", 0.0));

        if (command.kind.empty())
            return false;

        const std::lock_guard<std::mutex> guard (impl->lock);
        const auto found = impl->instances.find (instanceId);

        if (found == impl->instances.end())
            return false;

        auto* instance = found->second.get();

        if (instance->commands.size() >= 256)
            return false;

        instance->commands.push_back (std::move (command));

        if (instance->commandWake != nullptr)
            instance->commandWake (instance->commandWakeContext);

        return true;
    }

    void Registry::setInstanceActive (InstanceRecord* instance, bool active)
    {
        if (instance != nullptr)
        {
            instance->active.store (active, std::memory_order_relaxed);
            impl->serverChanged();
        }
    }

    void Registry::unregisterInstance (InstanceRecord* instance)
    {
        if (instance == nullptr)
            return;

        const std::lock_guard<std::mutex> guard (impl->lock);
        const auto found = impl->instances.find (instance->id);

        if (found == impl->instances.end())
            return;

        for (auto& [key, entry] : impl->devices)
            entry->removeGroups (instance->id);

        impl->instances.erase (found);
        impl->serverChanged();
    }

    bool Registry::setBindings (InstanceRecord* instance, const StateChunk& chunk, std::string& error)
    {
        if (instance == nullptr)
        {
            error = "no instance";
            return false;
        }

        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (impl->instances.count (instance->id) == 0)
            {
                error = "the instance is not registered";
                return false;
            }

            instance->lastChunk = std::make_unique<StateChunk> (chunk);
            impl->applyBindings (instance, chunk);
            impl->serverChanged();
        }

        impl->reconcileReceiver();
        return true;
    }

    /*  THE PLAN, built from the chunk under the registry's lock: on the message
        thread from setBindings, and again from a device's network thread when
        its tree lands after the bindings did (a reload), so a group planned
        short of the node's argument count is planned whole. The values the
        audio thread last posted carry over from the plan being replaced, so
        nothing is re-sent for the rebuild alone. */
    void Registry::Impl::applyBindings (InstanceRecord* instance, const StateChunk& chunk)
    {
        std::map<std::pair<DeviceEntry*, std::string>, std::shared_ptr<Group>> previous;

        if (const auto* live = instance->plan.load (std::memory_order_acquire))
            for (const auto& g : live->groups)
                previous[std::make_pair (g->device, g->path)] = g;

        instance->warnings.clear();
        instance->deviceByIndex.clear();
        instance->slotChecks.clear();

        for (const auto& ref : chunk.devices)
        {
            if (ref.name.empty() || ref.lastHost.empty() || ref.lastPort <= 0)
            {
                instance->deviceByIndex.push_back (nullptr);
                instance->warnings.push_back ("device \"" + ref.name + "\" has no address; its slots stay unresolved");
                continue;
            }

            auto& entry = entryFor (ref);
            instance->deviceByIndex.push_back (&entry);

            /*  The PRD's one-line notice (step 4 of reopening): the tree is not
                what it was when the slots were bound. The chunk's own hash
                against the live one, so each instance speaks for its binds. */
            const auto live = entry.treeHash();

            if (! ref.treeHash.empty() && ! live.empty() && ref.treeHash != live)
                instance->warnings.push_back ("device \"" + ref.name + "\": the tree changed since the bind");

            /*  Re-identification's word on this device (5b), when it has one. */
            if (const auto note = entry.resolutionNote(); ! note.empty())
                instance->warnings.push_back (note);
        }

        auto plan = std::make_unique<InstancePlan>();
        plan->groupOfSlot.assign (static_cast<std::size_t> (instance->slotCount), nullptr);
        plan->argOfSlot.assign (static_cast<std::size_t> (instance->slotCount), noSlot);

        std::map<std::pair<int, std::string>, std::shared_ptr<Group>> groups;

        for (const auto& slot : chunk.slots.all())
        {
            if (! slot.bound() || slot.hostParamId < 0 || slot.hostParamId >= instance->slotCount)
                continue;

            if (! slot.active())
            {
                /*  The PRD's "range unknown": inactive until min and max are entered. */
                SlotCheck inactive;
                inactive.state = SlotState::inactive;
                inactive.detail = "no range yet";
                inactive.mayDrive = inactive.mayReceive = false;
                instance->slotChecks[slot.hostParamId] = inactive;
                instance->warnings.push_back ("slot " + std::to_string (slot.hostParamId) + " (" + slot.path
                                              + ") has no range yet and neither sends nor receives");
                continue;
            }

            if (slot.deviceIndex < 0 || slot.deviceIndex >= static_cast<int> (instance->deviceByIndex.size())
                || instance->deviceByIndex[static_cast<std::size_t> (slot.deviceIndex)] == nullptr)
            {
                instance->warnings.push_back ("slot " + std::to_string (slot.hostParamId) + " (" + slot.path
                                              + ") names a device with no address");
                continue;
            }

            auto* entry = instance->deviceByIndex[static_cast<std::size_t> (slot.deviceIndex)];

            /*  Against the tree (5a): a stale slot keeps its lane and joins no
                group; an unauthorised one drives nothing but may still receive;
                a pending one (no tree yet) is planned as before and re-checked
                when the tree lands. */
            const auto check = entry->check (slot);
            instance->slotChecks[slot.hostParamId] = check;

            if (check.stale())
                continue;

            const bool drives = slot.sends() && (check.state != SlotState::unauthorised || check.mayDrive);
            const bool receives = slot.receives() && (check.state != SlotState::unauthorised || check.mayReceive);

            if (! drives && ! receives)
                continue;

            const auto groupKey = std::make_pair (slot.deviceIndex, slot.path);
            auto& group = groups[groupKey];

            if (group == nullptr)
            {
                group = std::make_shared<Group>();
                group->instanceId = instance->id;
                group->device = entry;
                group->path = slot.path;
                group->limiterKey = nextLimiterKey++;
                group->critical = entry->nodeIsCritical (slot.path);
            }

            const auto arg = static_cast<std::size_t> (std::max (0, slot.argIndex));

            if (group->slots.size() <= arg)
            {
                group->slots.resize (arg + 1);
                group->driven.resize (arg + 1, false);
                group->receiver.resize (arg + 1, -1);
            }

            group->slots[arg] = slot;
            group->critical = group->critical || slot.critical;

            if (drives)
            {
                /*  Only a sending slot is posted by the audio thread. */
                group->driven[arg] = true;
                plan->groupOfSlot[static_cast<std::size_t> (slot.hostParamId)] = group.get();
                plan->argOfSlot[static_cast<std::size_t> (slot.hostParamId)] = static_cast<std::uint16_t> (arg);
            }

            if (receives)
                group->receiver[arg] = slot.hostParamId;
        }

        std::map<DeviceEntry*, std::vector<std::shared_ptr<Group>>> perDevice;

        for (auto& [key, group] : groups)
        {
            /*  The message carries every argument of the node when the tree
                says how many; before the tree is in (a reload), the bound ones. */
            group->tags = group->device->nodeTags (group->path);
            const auto bound = group->argCount();
            const auto n = std::max (bound, group->tags.size());

            group->slots.resize (n);
            group->driven.resize (n, false);
            group->receiver.resize (n, -1);

            if (group->tags.size() < n)
            {
                group->tags.resize (n, 'f');

                for (std::size_t i = 0; i < bound; ++i)
                    if (group->driven[i] || group->receiver[i] >= 0)
                        group->tags[i] = group->slots[i].type;
            }

            group->known = std::make_unique<std::atomic<float>[]> (n);
            group->latest = std::make_unique<std::atomic<double>[]> (n);
            group->initial = group->device->initialValues (group->path, n);

            for (std::size_t i = 0; i < n; ++i)
            {
                const auto& v = group->initial[i];
                group->latest[i].store (v.isBool() ? (v.getBool() ? 1.0 : 0.0) : v.asDouble(), std::memory_order_relaxed);

                /*  Start from where the device is, so the first block only
                    sends what the host actually moved; on a rebuild, from
                    what the audio thread last posted. */
                float start = 0.0f;

                if (group->driven[i])
                {
                    if (const auto host = group->slots[i].hostValue (v))
                        start = *host;

                    const auto was = previous.find (std::make_pair (group->device, group->path));

                    if (was != previous.end() && i < was->second->argCount() && was->second->driven[i])
                        start = was->second->known[i].load (std::memory_order_relaxed);
                }

                group->known[i].store (start, std::memory_order_relaxed);
            }

            if (std::find (plan->devices.begin(), plan->devices.end(), group->device) == plan->devices.end())
                plan->devices.push_back (group->device);

            perDevice[group->device].push_back (group);
            plan->groups.push_back (group);
        }

        /*  Every device this instance used before learns its new groups (or none). */
        for (auto& [key, entry] : devices)
        {
            auto found = perDevice.find (entry.get());
            entry->setGroups (instance->id, found == perDevice.end() ? std::vector<std::shared_ptr<Group>> {} : found->second);
        }

        instance->plan.store (plan.get(), std::memory_order_release);
        instance->plans.push_back (std::move (plan));
    }

    void Registry::Impl::rebuildPlansFor (DeviceEntry* entry)
    {
        const std::lock_guard<std::mutex> guard (lock);

        for (auto& [id, instance] : instances)
        {
            if (instance->lastChunk == nullptr)
                continue;

            if (std::find (instance->deviceByIndex.begin(), instance->deviceByIndex.end(), entry) == instance->deviceByIndex.end())
                continue;

            applyBindings (instance.get(), *instance->lastChunk);
        }

        serverChanged();
    }

    bool Registry::setBindingsJson (InstanceRecord* instance, std::string_view chunkJson, std::string& error)
    {
        if (instance == nullptr)
        {
            error = "no instance";
            return false;
        }

        auto chunk = StateChunk::fromJson (chunkJson, instance->slotCount, error);

        if (! chunk.has_value())
            return false;

        if (! setBindings (instance, *chunk, error))
            return false;

        const std::lock_guard<std::mutex> guard (impl->lock);
        instance->warnings.insert (instance->warnings.end(), chunk->warnings.begin(), chunk->warnings.end());
        return true;
    }

    std::vector<std::string> Registry::warnings (InstanceRecord* instance) const
    {
        if (instance == nullptr)
            return {};

        const std::lock_guard<std::mutex> guard (impl->lock);
        return instance->warnings;
    }

    //==============================================================================
    void Registry::post (InstanceRecord* instance, int slot, float normalised) noexcept
    {
        if (instance == nullptr || slot < 0 || slot >= instance->slotCount)
            return;

        if (instance->lastValues != nullptr)
            instance->lastValues[static_cast<std::size_t> (slot)].store (normalised, std::memory_order_relaxed);

        const auto* plan = instance->plan.load (std::memory_order_acquire);
        auto* group = plan->groupOfSlot[static_cast<std::size_t> (slot)];

        if (group == nullptr)
            return;

        const auto arg = plan->argOfSlot[static_cast<std::size_t> (slot)];
        group->known[arg].store (normalised, std::memory_order_relaxed);

        if (impl->directSend.load (std::memory_order_relaxed) && group->device->directSend (*instance, *group))
            return;

        group->device->ring.push ({ instance->id, static_cast<std::uint16_t> (slot), SlotChange::value, normalised });
    }

    void Registry::endBlock (InstanceRecord* instance) noexcept
    {
        if (instance == nullptr)
            return;

        const auto* plan = instance->plan.load (std::memory_order_acquire);

        for (auto* device : plan->devices)
        {
            device->ring.push ({ instance->id, 0, SlotChange::endOfBlock, 0.0f });

            /*  The one syscall of the audio path, by decision (2026-11-08):
                a non-blocking wake, once per block per device. */
            device->wake.signal();
        }
    }

    //==============================================================================
    void Registry::setReceiver (InstanceRecord* instance, void (*wake) (void* context), void* context)
    {
        if (instance == nullptr)
            return;

        const std::lock_guard<std::mutex> guard (impl->lock);

        if (impl->instances.count (instance->id) == 0)
            return;

        instance->wake = wake;
        instance->wakeContext = context;
    }

    int Registry::pollReceived (InstanceRecord* instance, int* slots, float* values, int capacity) noexcept
    {
        if (instance == nullptr || slots == nullptr || values == nullptr || capacity <= 0)
            return 0;

        int n = 0;
        SlotChange change;

        while (n < capacity && instance->incoming.pop (change))
        {
            slots[n] = change.slot;
            values[n] = change.normalised;
            ++n;
        }

        return n;
    }

    //==============================================================================
    bool Registry::testSend (InstanceRecord* instance, int slot, float normalised, std::string& error)
    {
        if (instance == nullptr || slot < 0 || slot >= instance->slotCount)
        {
            error = "no such slot";
            return false;
        }

        const auto* plan = instance->plan.load (std::memory_order_acquire);
        auto* group = plan->groupOfSlot[static_cast<std::size_t> (slot)];

        if (group == nullptr)
        {
            error = "slot " + std::to_string (slot) + " is not bound to a sending, active slot";
            return false;
        }

        group->known[plan->argOfSlot[static_cast<std::size_t> (slot)]].store (normalised, std::memory_order_relaxed);
        group->device->ring.push ({ instance->id, static_cast<std::uint16_t> (slot), SlotChange::value, normalised });
        group->device->ring.push ({ instance->id, 0, SlotChange::endOfBlock, 0.0f });
        group->device->wake.signal();
        return true;
    }

    std::optional<juce::var> Registry::deviceTree (InstanceRecord* instance, int deviceIndex, const std::string& path) const
    {
        if (instance == nullptr)
            return std::nullopt;

        DeviceEntry* entry = nullptr;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (deviceIndex < 0 || deviceIndex >= static_cast<int> (instance->deviceByIndex.size()))
                return std::nullopt;

            entry = instance->deviceByIndex[static_cast<std::size_t> (deviceIndex)];
        }

        if (entry == nullptr)
            return std::nullopt;

        std::optional<juce::var> out;

        entry->withDevice ([&] (const oscquery::Device& d)
        {
            if (! d.treeLoaded())
                return;

            d.withTree ([&] (const oscquery::Tree& tree) { out = nodeAt (tree.toVar(), path); });
        });

        return out;
    }

    namespace
    {
        /** One bound slot's standing, as the status says it (per instance and in the Monitor's list). */
        juce::var slotCheckVar (int id, const SlotCheck& check)
        {
            auto* s = new juce::DynamicObject();
            s->setProperty ("id", id);
            s->setProperty ("state", slotStateName (check.state));
            s->setProperty ("detail", juce::String (check.detail));

            if (! check.renamedTo.empty())
                s->setProperty ("renamed_to", juce::String (check.renamedTo));

            if (check.deviceRangeMin.has_value() && check.deviceRangeMax.has_value())
            {
                s->setProperty ("device_range_min", *check.deviceRangeMin);
                s->setProperty ("device_range_max", *check.deviceRangeMax);
            }

            return juce::var (s);
        }
    }

    juce::var Registry::status (InstanceRecord* instance) const
    {
        auto* o = new juce::DynamicObject();
        juce::Array<juce::var> devices;

        {
            const std::lock_guard<std::mutex> guard (impl->lock);

            if (instance != nullptr)
            {
                for (auto* entry : instance->deviceByIndex)
                    devices.add (entry != nullptr ? entry->status() : juce::var());

                juce::StringArray warnings;

                for (const auto& w : instance->warnings)
                    warnings.add (juce::String (w));

                o->setProperty ("warnings", warnings);

                /*  Every bound slot's standing against its device's tree (5a). */
                juce::Array<juce::var> slots;

                for (const auto& [id, check] : instance->slotChecks)
                    slots.add (slotCheckVar (id, check));

                o->setProperty ("slots", slots);
                o->setProperty ("active", instance->active.load (std::memory_order_relaxed));
                o->setProperty ("direct_sent", static_cast<juce::int64> (instance->directSent.load()));
                o->setProperty ("received", static_cast<juce::int64> (instance->received.load()));
                o->setProperty ("received_dropped", static_cast<juce::int64> (instance->incoming.dropped()));
                o->setProperty ("plans", static_cast<int> (instance->plans.size()));
            }
            else
            {
                for (const auto& [key, entry] : impl->devices)
                    devices.add (entry->status());

                /*  THE MONITOR'S LIST (6a-2): every instance, its bound slots
                    with what they are bound to (from the chunk it last pushed)
                    and how the tree judges them. */
                juce::Array<juce::var> instances;

                for (const auto& [id, record] : impl->instances)
                {
                    auto* i = new juce::DynamicObject();
                    i->setProperty ("id", static_cast<juce::int64> (id));
                    i->setProperty ("label", juce::String (record->label));
                    i->setProperty ("slot_count", record->slotCount);
                    i->setProperty ("active", record->active.load (std::memory_order_relaxed));
                    i->setProperty ("instance", record->lastChunk != nullptr ? record->lastChunk->instanceNumber : 1);

                    juce::Array<juce::var> slots;

                    if (record->lastChunk != nullptr)
                    {
                        for (const auto& slot : record->lastChunk->slots.all())
                        {
                            if (! slot.bound())
                                continue;

                            const auto found = record->slotChecks.find (slot.hostParamId);
                            auto s = found != record->slotChecks.end() ? slotCheckVar (slot.hostParamId, found->second)
                                                                       : slotCheckVar (slot.hostParamId, SlotCheck {});
                            auto* so = s.getDynamicObject();
                            so->setProperty ("device_index", slot.deviceIndex);
                            so->setProperty ("device", slot.deviceIndex >= 0 && static_cast<std::size_t> (slot.deviceIndex) < record->lastChunk->devices.size()
                                                           ? juce::String (record->lastChunk->devices[static_cast<std::size_t> (slot.deviceIndex)].name)
                                                           : juce::String());
                            so->setProperty ("path", juce::String (slot.path));
                            so->setProperty ("arg_index", static_cast<int> (slot.argIndex));
                            so->setProperty ("type", juce::String::charToString (static_cast<juce::juce_wchar> (slot.type)));
                            so->setProperty ("direction", slot.direction == Direction::send ? "send" : slot.direction == Direction::receive ? "receive" : "both");
                            so->setProperty ("display_name", juce::String (slot.displayName));
                            so->setProperty ("scaling", scalingName (slot.scaling));
                            so->setProperty ("unit", juce::String (slot.unit));

                            if (slot.rangeKnown())
                            {
                                so->setProperty ("range_min", *slot.rangeMin);
                                so->setProperty ("range_max", *slot.rangeMax);
                            }

                            slots.add (s);
                        }
                    }

                    i->setProperty ("slots", slots);
                    instances.add (juce::var (i));
                }

                o->setProperty ("instances", instances);
                o->setProperty ("instance_count", static_cast<int> (impl->instances.size()));
            }

            o->setProperty ("settings", impl->settings.toVar());

        {
            auto* f = new juce::DynamicObject();
            f->setProperty ("path", impl->settingsFile.getFullPathName());
            f->setProperty ("loaded", impl->settingsLoaded);
            f->setProperty ("error", juce::String (impl->settingsFileError));
            o->setProperty ("settings_file", juce::var (f));
        }
        }

        o->setProperty ("devices", devices);

        /*  The Finder's list (5b): what DNS-SD found and what was typed, each
            probed, for the editor's device list and the Monitor. */
        {
            juce::Array<juce::var> found;
            const auto now = std::chrono::steady_clock::now();

            if (impl->resolver != nullptr)
            {
                for (const auto& c : impl->resolver->found())
                {
                    auto* f = new juce::DynamicObject();
                    f->setProperty ("name", juce::String (c.name));
                    f->setProperty ("host", juce::String (c.host));
                    f->setProperty ("port", c.port);
                    f->setProperty ("manual", c.manual);
                    f->setProperty ("advertised", c.advertised);
                    f->setProperty ("probed", c.probed);
                    f->setProperty ("reachable", c.reachable);
                    f->setProperty ("error", juce::String (c.error));
                    f->setProperty ("last_seen_ms", c.lastSeen == std::chrono::steady_clock::time_point {}
                                                        ? juce::int64 (-1)
                                                        : static_cast<juce::int64> (std::chrono::duration_cast<std::chrono::milliseconds> (now - c.lastSeen).count()));
                    found.add (juce::var (f));
                }
            }

            o->setProperty ("found", found);
            o->setProperty ("finder_running", impl->resolver != nullptr && impl->resolver->running());

            /*  The plugin's own server (6b-2). */
            if (impl->server != nullptr)
            {
                auto server = impl->server->status();
                server.getDynamicObject()->setProperty ("enabled", impl->settings.serverEnabled);
                server.getDynamicObject()->setProperty ("announcing", impl->server->isRunning() && impl->resolver != nullptr
                                                                          && impl->resolver->running() && impl->resolver->discoveryOn());
                o->setProperty ("server", server);
            }
        }

        /*  The UDP receiver: the port to type into a device without LISTEN,
            beside this host's addresses; 0 while it is not running. */
        o->setProperty ("receive_port", impl->receiver.isRunning() ? impl->receiver.boundPort() : 0);
        o->setProperty ("receive_addresses", Impl::ownAddresses());
        o->setProperty ("udp_datagrams", static_cast<juce::int64> (impl->udpDatagrams.load()));
        o->setProperty ("udp_refused", static_cast<juce::int64> (impl->udpRefused.load()));
        o->setProperty ("udp_unrouted", static_cast<juce::int64> (impl->udpUnrouted.load()));
        o->setProperty ("udp_ambiguous", static_cast<juce::int64> (impl->udpAmbiguous.load()));
        return juce::var (o);
    }

    bool Registry::setDeviceAddress (const std::string& name, const std::string& identityValue,
                                     const std::string& host, int port)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        const auto found = impl->devices.find (keyOf (name, identityValue));

        if (found == impl->devices.end() || host.empty() || port <= 0)
            return false;

        found->second->setAddress (host, port, true);
        return true;
    }

    bool Registry::setDeviceTiming (const std::string& name, const std::string& identityValue,
                                    bool honoursTimetags, double offsetMs)
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        const auto found = impl->devices.find (keyOf (name, identityValue));

        if (found == impl->devices.end())
            return false;

        found->second->setTiming (honoursTimetags, offsetMs, true);
        return true;
    }

    void Registry::finderEnsure()
    {
        if (impl->resolver != nullptr)
            impl->resolver->ensureFinder();
    }

    void Registry::finderAddManual (const std::string& host, int port)
    {
        if (impl->resolver != nullptr && ! host.empty() && port > 0)
            impl->resolver->addManual (host, port);
    }

    void Registry::setFinderOptions (const oscquery::Finder::Options& options)
    {
        if (impl->resolver != nullptr)
            impl->resolver->setOptions (options);
    }

    void Registry::setSettings (const RegistrySettings& settings)
    {
        {
            const std::lock_guard<std::mutex> guard (impl->lock);
            impl->settings = settings;
            impl->directSend.store (settings.directSend, std::memory_order_relaxed);

            for (auto& [key, entry] : impl->devices)
                entry->setSettings (settings);
        }

        if (impl->resolver != nullptr)
            impl->resolver->setDiscovery (settings.discoveryEnabled);

        impl->reconcileReceiver();
        reconcileServer();

        /*  To disk, outside the lock (a write may take its time). */
        if (impl->settingsFile != juce::File())
        {
            std::string error;
            const bool written = SettingsFile::save (impl->settingsFile, settings.toVar(), error);

            const std::lock_guard<std::mutex> guard (impl->lock);
            impl->settingsFileError = written ? std::string() : error;
            impl->settingsLoaded = impl->settingsLoaded || written;
        }
    }

    juce::File Registry::settingsFile() const
    {
        return impl->settingsFile;
    }

    RegistrySettings Registry::settings() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->settings;
    }

    std::size_t Registry::deviceCount() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->devices.size();
    }

    std::size_t Registry::instanceCount() const
    {
        const std::lock_guard<std::mutex> guard (impl->lock);
        return impl->instances.size();
    }

}
