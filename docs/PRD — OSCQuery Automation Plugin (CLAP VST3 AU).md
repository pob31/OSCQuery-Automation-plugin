# PRD — OSCQuery Automation Plugin (CLAP / VST3 / AU)

Nov 4, 2026 · @Pierre-Olivier Boulant

## Summary

A generic DAW plugin that records and plays back automation curves to and from any OSC-controlled device, with the OSC address space discovered over OSCQuery instead of typed into an XML file. Shipped as CLAP, VST3 and AU from one codebase, open source, developed as the reference client for an OSCQuery discovery profile. Standardisation of that profile is not a goal of this project: a profile becomes a standard when manufacturers adopt it, and that takes more than code (removed 2026-11-04).

Repository: [github.com/pob31/OSCQuery-Automation-plugin](https://github.com/pob31/OSCQuery-Automation-plugin)

**Problem.** Today's OSC automation plugins (IRCAM ToscA and its successor OSCar, the d&b Soundscape, Holophonix, L-ISA and WFS-DIY plugins) either require a hand-written mapping file or hard-code one manufacturer's parameter set. Each DAW session ends up tied to a device it cannot describe, and heterogeneous rigs (WFS + Ambisonics + haptic floor at PRISM, for instance) have no common way to expose parameters to a DAW.

**Approach.** Discovery is done once, at setup: the plugin browses the device tree over OSCQuery, the user binds each automation slot to a path, and the bindings are saved in the session. After that, the plugin behaves like a static ToscA-style plugin, so hosts never see a changing parameter list. Discovery (finding devices, reading trees) is the subject; namespaces such as ADM-OSC are deliberately out of scope.

## Goals and non-goals

**Goals**

1. Bind DAW automation lanes to any OSCQuery-published parameter without typing an address.
2. Play back (DAW to device) and record (device to DAW) with the same binding.
3. Survive the move from studio to venue: a session rebinds to the same device on a different IP.
4. One codebase, three formats: CLAP, VST3, AU. Pro Tools (AAX) is a later target, not a v1 target.
5. Serve as the reference client for the OSCQuery discovery profile and its interoperability test suite.
6. Open source under the GPLv3 licence (decided 2026-11-04, was MIT), in the pob31 repository; institutions that adopt it are expected to fork.

**Non-goals**

- No audio processing in the plugin; audio passes through untouched.
- No namespace semantics. The plugin does not know what `/adm/obj/1/azim` means; it only knows it is a float in \[-180, 180\].
- No device configuration (speaker setup, patching). Only parameters the device exposes as automatable.
- No authentication scheme. Write access is the device's business (see Access control).
- No dynamic parameter lists inside a running session. Parameter count is fixed per plugin instance; only names and bindings change.

## Users and use cases

| User | Situation | What they need from the plugin |
| --- | --- | --- |
| Sound designer in a DAW (Reaper, Nuendo, Live, Logic) | Writes spatial moves against a renderer during creation | Browse the renderer's tree, bind azimuth/x/y/gain per track, draw curves, hear them played back |
| Mixing engineer with a controller | Moves objects from a fader or tablet while the DAW records | `LISTEN` feedback from the device written into automation lanes in Write/Touch/Latch |
| Touring tech | Reopens the session at a new venue with a different renderer IP | Rebind by device name, see which slots are unresolved, fix them in one dialog |
| Research platform (PRISM) | WFS, Ambisonics and haptic floor in one room with no shared namespace | One plugin per track driving any of the three devices, chosen from the browser |
| Device manufacturer (Holophonix, d&b, L-Acoustics, Fletcher Machine) | Asked to add OSCQuery | A client that proves the profile works and a test matrix showing where their implementation falls short |
| Show control (Go.dot, QLab, ossia) | Needs the same bindings outside the DAW | Bindings exported as JSON so other clients can reuse them |

## Discovery: finding devices and reading trees

Discovery happens in the plugin editor, never in the audio thread, and only when the user opens the browser or asks to rebind.

**Finding devices**

- Browse `_oscjson._tcp` via DNS-SD (mDNS) on all active interfaces. Show the `NAME` from `HOST_INFO`, host and port.
- Manual entry fallback: host and port typed by the user. Required, because multicast often does not cross VLANs on show networks. DNS-SD can also be turned off whole in the global settings (`discovery_enabled`, added 2026-11-10): the plugin then neither browses nor announces its own server, and devices are typed. For a network where multicast is not welcome; found necessary the day the validators' hundreds of plugin processes flooded one (`docs/bench/2026-11-10-mdns-flood.md`).
- Recent devices list, persisted in the plugin's global settings (not the session), keyed by `NAME` plus last known host:port.
- Each discovered device is probed with `GET /?HOST_INFO` before it is listed. Devices that fail the probe show as unreachable, not hidden.

**Reading the tree**

- `GET /` for the full tree by default; lazy subtree `GET /<path>` when the root reports more than a configurable node count (default 5000) or the device advertises a depth limit.
- Only nodes with `TYPE` and `ACCESS` including write are bindable for playback; nodes with `ACCESS` including read are bindable for record. Container nodes are browsable, not bindable.
- Per node, the plugin reads and stores: `FULL_PATH`, `TYPE`, `RANGE`, `UNIT` (when present), `ACCESS`, `DESCRIPTION`, `CLIPMODE`, `CRITICAL`.
- Supported `TYPE` for v1: `f`, `i`, `T`/`F` (as a toggle), and multi-argument float nodes (e.g. `fff` for xyz) bound as one slot per argument.
- Change notifications over the websocket (`PATH_REMOVED`, `PATH_RENAMED`, `PATH_CHANGED`) mark affected slots as stale in the UI. The plugin never rebinds on its own.

**Conformance the plugin assumes from the device (the Core profile)**

1. `HOST_INFO` with `NAME`, `OSC_PORT`, `OSC_TRANSPORT`, as the OSCQuery proposal defines them; `NAME` stable across reboot. The profile adds no field to `HOST_INFO` (decided 2026-11-04: the proposal needs support, not a rewrite). Two optional extensions, `DEVICE_ID` and `CLOCK`, are drafted outside the profile in `docs/spec/extensions-draft-0.1.md`; the plugin uses one only when the device lists it in `EXTENSIONS`, and assumes neither (decided 2026-11-10).
2. A static tree whose paths survive reboot and session reload.
3. `TYPE` and `RANGE` on every bindable leaf.

Everything else (`LISTEN`, change notifications, websocket OSC) is the Live profile and is detected at runtime from `EXTENSIONS`.

## Binding model: slots, paths, scaling

Each plugin instance owns a fixed pool of **N generic slots** (N = 32 by default; 8, 16, 32, 64 and 128 variants as separate plugin IDs so the host sees a fixed count). A slot is one host-automatable parameter.

| Slot field | Source | Notes |
| --- | --- | --- |
| Host parameter ID | Fixed at build time | Stable across binds, renames and sessions; automation curves attach to it |
| Device | User choice in browser | Stored as `NAME` + last known host:port, plus an optional identity leaf (a read-only node the user designates, such as a serial number), and the device's `DEVICE_ID` when it declares one (added 2026-11-10) |
| Path | User choice in browser | Full OSC path, with argument index for multi-argument nodes |
| Type | From tree | `f`, `i`, `T/F`; drives the OSC type tag on send |
| Range | From tree, user-overridable | Maps host 0–1 to device units; `RANGE` absent → user must enter min/max before the slot is active |
| Scaling | User choice | Linear (default), log, or exponential; same choices as OSCar |
| Direction | User choice | Send, receive, or both |
| Display name | From path, user-editable | Shown in the host's automation lane |

**Rules**

- The host always sees a normalised 0–1 float. Scaling to device units happens inside the plugin, so no host rescan is ever needed for a range change.
- Unbound slots are hidden in CLAP (`is_hidden`) and named `(unbound)` in VST3 and AU: VST3 cannot hide a parameter, and AUv2 has no hidden flag; leaving a slot out of the AU parameter list loses its automation (REAPER) or the slot itself (GarageBand) (measured 2026-11-08, `docs/spikes/phase1-formats.md`).
- Multi-argument nodes (`fff`) bind as consecutive slots; sending is grouped so the device receives one message with all arguments, using the last known value for arguments not being automated.
- Rate limiting per slot: default 100 Hz, configurable; sends only on value change, with an optional periodic resend for lossy links.
- A `$instance` token in paths is supported for ToscA-style per-track numbering, but it is sugar over the explicit path, not the primary model.

## Host parameter model per format

The plugin never changes its parameter count at runtime. It changes names, grouping and hidden flags only.

| Capability | CLAP | VST3 | AU |
| --- | --- | --- | --- |
| Stable parameter identity | `clap_id` per slot | `ParamID` per slot | `AudioUnitParameterID` per slot |
| Rename after bind | `rescan(CLAP_PARAM_RESCAN_INFO)`, immediate, while active | `restartComponent(kParamTitlesChanged)`; Samplitude, Live and Bitwig follow on lanes already drawn, during playback too; Reaper renames the parameter but not a lane already drawn (measured 2026-11-08) | `kAudioUnitProperty_ParameterInfo` change notification; GarageBand follows, Reaper as for VST3, Logic unmeasured (2026-11-08) |
| Hierarchical grouping | `module` path from the OSC path (`/wfs/input/12`) | Flat; group in name prefix | `clump` ID per device |
| Hide unbound slots | `CLAP_PARAM_IS_HIDDEN` | Not available | Not without cost: no AUv2 flag, and leaving the slot out of the parameter list loses automation (2026-11-08) |
| Drop stale automation | `clap_host_params->clear()` | Not available | Not available |
| Sample-accurate automation | Yes | Yes, if host sends it | Yes |
| Hosts (Nov 2026) | Bitwig, Reaper, FL Studio, Studio One | Everything except Logic and Pro Tools | Logic, GarageBand, Live and Reaper on macOS |

**Decisions**

- CLAP is the reference format: it is the only one where rename, grouping, hide and clear all work as designed.
- VST3 and AU are best-effort for naming: the bound path is always visible inside the plugin editor, so a host that ignores the rename still works.
- AU never hides an unbound slot (decided 2026-11-08, after the Phase 1 format bench): AUv2 has no hidden flag, and the one means it has, leaving the slot out of the parameter list, deleted automation in REAPER and kept the slots out of GarageBand. Unbound AU slots are named `(unbound)`, as in VST3. CLAP's `is_hidden` is unaffected.
- Build with JUCE. VST3 and AU first, through JUCE's own wrappers; CLAP at milestone 4b through a shim of our own written against the CLAP C API with `clap-helpers`. Neither `clap-juce-extensions` nor `clap-wrapper` is used (decided 2026-11-04: JUCE 9.0.5 has no native CLAP and nothing on `develop` points to one, so nothing waits on JUCE). The shim interface is written for CLAP's capabilities from the start; VST3 and AU answer *unsupported* where they must.
- Slot-count variants are five plugin IDs (8/16/32/64/128), decided by consequence the same day: with JUCE each ID is its own binary and VST3 cannot hide slots. The process-wide registry therefore lives in a shared core library loaded by every binary, after WFS-DIY's Bridge.
- The CLAP ids are `io.github.pob31.oqa.Q008` … `Q128`, the bundle ids the JUCE shim gives the same codes, all five listed by one `.clap` (decided 2026-11-09, milestone 4b). Like the VST3 and AU identities, they never change once a session has saved one. The Monitor is the sixth, `io.github.pob31.oqa.QMon`, zero parameters (decided 2026-11-09, with the Monitor itself).
- AAX is deferred; its parameter list is frozen at instantiation and Avid's SDK agreement conflicts with an open-source distribution.

## Transport, send and record directions

**Send (DAW to device)**

- UDP by default to `OSC_IP:OSC_PORT` from `HOST_INFO`; TCP (SLIP-framed) when `OSC_TRANSPORT` is TCP or the node is marked `CRITICAL`.
- **One bundle per device per audio block**, holding every changed slot from every instance in the process. Bundles are about jitter and atomicity, not raw latency: 64 tracks moving xyz become one datagram instead of 192, and the device applies x, y and z together. Bundles stay under the MTU (about 1,400 bytes, 40 to 45 three-float messages) and split beyond that.
- **Timetags.** Devices that honour bundle timetags receive timetag = intended playback time and schedule the change themselves. No device on the bench does yet: WFS-DIY reads the timetag and discards it, applying bundles on arrival (checked in its source 2026-11-08; this sentence said "WFS-DIY does" until then), and most commercial renderers do not either. The path is built and verified against the mock device. Whether a device honours them is a per-device setting the user ticks in the Monitor, default off, since OSCQuery has no field for it and the profile adds none (decided 2026-11-04). Devices with the setting off receive the immediate timetag. A device that declares the optional `CLOCK` extension pre-fills the setting from its `SCHEDULING`; set by hand, the setting wins over the device's word, and the device's word over the session's (added 2026-11-10).
- **No rate limit by default.** Every block that has changes is sent and the device smooths. The per-slot limiter exists as protection for lossy links or slow devices, default off.
- A shared network thread per device across all plugin instances in the process, so 64 tracks do not open 64 sockets. Instances register with a process-wide device registry; sends from the audio thread go through a lock-free queue to a realtime-priority network thread (a few hundred microseconds). An option sends with non-blocking `sendto` directly from the audio thread for the lowest latency, at the risk of a stall on a full socket buffer; default off.

**Record (device to DAW)**

- Preferred: `LISTEN` on the websocket for each bound path. Incoming values are scaled to 0–1 and pushed to the host as parameter changes through the format's host-callback path (`clap_host_params->request_flush`, VST3 `IEditController` output events, AU `AUParameterListener`).
- Fallback for devices without websocket (WebSockets are optional in the OSCQuery proposal; only servers that stream values need them): the plugin opens a UDP listener and shows its port in the editor; the user enters that port in the device's own configuration, as ToscA and OSCar users do today and as WFS-DIY's client table allows. Telling a device where to send OSC is the device's business, and the profile adds nothing for it (decided 2026-11-04).
- Record only writes to the host when the slot's direction includes receive and the host lane is in Write, Touch or Latch, matching the Soundscape plugin's `Rx` behaviour.
- Loop guard: a value the plugin just sent is ignored if echoed back within 50 ms, so send and receive on the same slot do not oscillate.

**Timing**

- **Where the latency is.** Host automation arrives once per block: 1.3 ms at 64 samples, 10.7 ms at 512 at 48 kHz. CLAP and VST3 deliver sample offsets within the block, so the plugin knows the whole block's moves at its start and can send them at once.
- **OSC runs ahead of the audio.** A packet sent at block start reaches the device before the audio for that block leaves the converters, by roughly the DAW's output latency. Net OSC latency is usually negative relative to what is heard, so alignment matters more than speed.
- **Per-device offset.** A signed offset in milliseconds per device, applied to timetags when the device honours them and to a send delay otherwise, to line the renderer up with the DAW's audio. OSC latency compensation, set once per rig.
- No clock or transport is sent. Devices that need transport get it from the DAW's own MTC or LTC, not from this plugin.
- Target on a local network: under 1 ms from the start of block processing to the packet on the wire, excluding the block itself.

## Session persistence and device re-identification

All bindings live in the plugin's state chunk inside the DAW session. No external mapping file.

**State chunk (JSON, versioned)**

- `version`
- `devices[]`: `name`, `last_host`, `last_port`, `tree_hash` (hash of the typed leaves at bind time; this said "bindable leaves" until 2026-11-10, which the hash never was: it takes every leaf with a `TYPE`, so that it does not depend on which client asks), `identity_path` and `identity_value` (optional), `device_id` (optional, the device's `DEVICE_ID` when it declares one; added 2026-11-10), `honours_timetags`, `timetags_by_hand` (optional, true when a hand set `honours_timetags`, so that it stands over a device's `CLOCK` on reload; added 2026-11-10), `offset_ms`
- `slots[]`: `host_param_id`, `device_index`, `path`, `arg_index`, `type`, `range`, `scaling`, `direction`, `display_name`, `range_overridden`
- `values[]`: every slot's host value, 0–1, in slot order (added 2026-11-09, milestone 4b: a CLAP restores its own parameter values from its state, and clap-validator checks that it does; every format writes them, so the chunk opens anywhere with its values)
- `instance`: the number `$instance` stands for in this instance's paths, user-set, default 1 (added 2026-11-09 for the Monitor's bulk bind); a slot bound through a pattern keeps it as `path_pattern` beside its resolved `path`

**Reopening a session**

1. For each device, try the last host:port and compare `NAME` from `HOST_INFO`, and the identity leaf's value when one is set.
2. On mismatch or no answer, browse DNS-SD for the same `NAME`. Several candidates are told apart by the identity leaf, then by `tree_hash`; if still ambiguous, the device shows as unresolved and the user points at it once (in the Monitor or any instance's device pane), and every instance follows.

   Added 2026-11-10, for a device that declares `DEVICE_ID` and a session that saved one: in step 1 the two IDs are compared instead of `NAME` (equal confirms the device even under a new `NAME`; different is a mismatch even under the same one), and in step 2 a device declaring the saved ID is looked for first, whatever its `NAME`. Two live devices declaring the same ID are ambiguous and the user points; a candidate declaring a different ID is never adopted without the user. A device that declares none goes through steps 1 and 2 as written.
3. Once found, verify each bound path still exists with the same `TYPE`. Missing or retyped paths mark the slot as stale; it keeps its automation but stops sending.
4. If `tree_hash` differs, show a one-line notice in the editor so the user knows the device changed since the bind.

No automatic rebinding to a different device. A stale slot is fixed by the user in the browser, where the old path is shown next to the new tree.

**Export and import**

- Bindings export to a standalone JSON file so Go.dot, ossia or a second DAW can reuse them.
- Import can address slots by `host_param_id` so a ToscA or OSCar session can be migrated by a small converter script.

## Access control and failure states

The plugin assumes an unauthenticated control network. Reading a tree is harmless; writing is governed by the device's own allow-list (WFS-DIY's client table, Holophonix and d&b remote-control toggles).

- The plugin reads `ACCESS` as reported **to this client**. A device that answers `ACCESS: 1` (read-only) for a client not on its allow-list makes the slot bindable for record but not for send, and the editor says why.
- OSC received over the websocket is treated by the device exactly like UDP OSC, so the query port never becomes a write path that skips the allow-list. The plugin documents this expectation for implementers and the test matrix checks it.
- Every failure is visible in the editor, never silent:

| State | Cause | Shown as |
| --- | --- | --- |
| Unreachable | No `HOST_INFO` answer | Device row greyed, last-seen time |
| Unauthorised | `ACCESS` excludes write for this client | Slot amber, tooltip names the device's allow-list |
| Stale | Path missing or retyped after reopen | Slot red, old path shown, rebind button |
| Range unknown | Leaf without `RANGE` | Slot inactive until min/max entered |
| Echo loop | Send and receive on the same slot oscillating | Loop guard engages, counter shown |
| Dropped sends | Rate limiter active or socket error | Per-device send counter and error log |
| Server write refused | A client not on the plugin's own allow-list, or an unknown device or path, wrote to the plugin's OSCQuery server (added 2026-11-09, milestone 5+) | Counted in the Monitor; the slot's `state` leaf says why; the editor's warnings line |
| Server port in use | The pinned HTTP or OSC port of the plugin's server cannot be bound (added 2026-11-09) | `server_error` in the status and the Monitor; never a silent fall-back to another port |
| Announce failed | The plugin's server could not announce over DNS-SD (multicast refused, a sandbox) (added 2026-11-09) | The Discovery's problem in the Monitor's server line; the server still answers on its port |

## UX: plugin UI, browser, status

The editor has two panes and no modal dialogs: a **device browser** on the left and the **slot table** on the right. Binding is drag from the tree to a slot row, or click a slot then click a leaf.

**Device browser**

- Device list from DNS-SD plus manual entries and recents; each with name, host, identity value when set, reachability dot.
- Tree view of the selected device with search-as-you-type on path and `DESCRIPTION`. Leaves show type, range and unit inline; non-writable leaves are shown but marked.
- A `Copy tree as JSON` button for bug reports to manufacturers.

**Slot table**

- One row per slot: index, display name, device, path, range, scaling, direction, current value, state colour.
- Bulk operations: bind a range of slots to a multi-argument node, apply the same path pattern across instances with `$instance`, clear all stale.
- Range and scaling editable inline; `RANGE` from the device shown greyed when overridden.

**Status bar**

- Per device: packets sent/received per second, websocket state, last error.
- A `Test` button per slot that sends the current value once, for checking the patch without arming automation.

**Headless use**

- Everything in the editor is also reachable through the plugin's own OSCQuery server (the plugin exposes its slot table at `/plugin/<instance>/slot/<n>/...`), so Go.dot or a script can bind slots without opening the editor. Dogfooding the spec.

## Architecture and tech choices

&#91;embedded content: plugin architecture: 3 shims, 1 core per instance, 1 shared registry\]

The three format shims are thin; everything that matters lives in the core and in a process-wide registry, so 64 instances share one OSCQuery client and one socket per device.

- **Language and framework:** C++20 with JUCE 9, matching WFS-DIY so OSC and tree-browser code can be shared. VST3 and AU through JUCE's wrappers; CLAP through an in-house shim against the CLAP C API at milestone 4b.
- **OSCQuery client:** a small in-house library (HTTP, websocket, JSON tree model, DNS-SD) published separately, also GPLv3, so Go.dot and other open-source clients reuse it and the test suite links against it. Manufacturers with closed firmware validate against it through the conformance CLI rather than by bundling it; the author holds the copyright, so a JUCE-free permissive extraction stays possible if one asks.
- **Threads:** the audio thread reads slot values only; the registry owns a network thread per device; the editor owns the browser. Lock-free queues between them.
- **No external files:** bindings in the state chunk, recents in the standard per-user settings location.
- **Platforms:** macOS (universal), Windows, Linux (VST3 and CLAP only).
- **Test suite:** a conformance CLI that runs the Core and Live profile checks against any OSCQuery server and prints the matrix row for that device; the plugin's own OSCQuery server is its first test subject.

**Why no master plugin.** All instances share one shared library inside the host process, so the registry is free: nothing to insert, no load order, no track to delete by mistake. A master plugin would need IPC to reach the track instances and would fail in exactly the ways the bridge-style plugins do. What a master would have given, a place to look, is provided by a **Monitor** plugin: it shows the registry's devices, every slot across all instances, packet rates and errors, and holds global settings, but owns nothing. Any instance can open the same view.

Hosts that sandbox plugins per instance (Bitwig's per-plugin mode, Logic with AUv3, Reaper bridging) give each instance its own registry. Everything still works; the device simply sees several clients, and bundling happens per process rather than per DAW.

## Milestones and deliverables

Dates are placeholders to be set once staffing is known; order and gates are the commitment.

| # | Milestone | Deliverable | Gate |
| --- | --- | --- | --- |
| 0 | Core profile draft | 4-page spec of `HOST_INFO` as defined, stable `NAME`, path stability, `TYPE`/`RANGE`, `ACCESS` per client | Reviewed by Holophonix and one ossia maintainer |
| 1 | Format bench | Spike: 8-slot VST3 + AU build; rename measured in Reaper, Bitwig, Live, Nuendo and Logic | Rename matrix written; five slot-count IDs confirmed |
| 2 | Discovery and bind | Browser, tree model, bind, send over UDP, state chunk | WFS-DIY driven from Reaper with no typed address |
| 3 | Record | `LISTEN` to host automation, loop guard, Write/Touch/Latch semantics | Controller moves recorded in Reaper and Bitwig |
| 4 | Re-identification | Rebind by `NAME` and identity leaf, pointing once in the Monitor, stale slots, export/import JSON | Session moved between two machines with different IPs |
| 4b | Own CLAP shim | Native CLAP against the C API: factory with all variants, rename, hide, clear, module, JUCE editor embedded | Milestone 2 and 3 gates pass in Bitwig over CLAP; `clap-validator` clean |
| 5 | AU and Linux | AU build in Logic and Live; Linux CLAP/VST3 | Same session file opens in all three formats |
| 6 | Conformance CLI | Core and Live checks, matrix output | Run against WFS-DIY, Holophonix, ossia, Chataigne |
| 7 | Public beta | v0.9 release, matrix published, profile draft 0.2 published in the repository | Manufacturers contacted with their matrix row |

## Interoperability test matrix

Each check is pass/fail, run by the conformance CLI. Rows are devices; the published matrix is the argument made to manufacturers.

| Check | Profile | What passes |
| --- | --- | --- |
| C1 `HOST_INFO` | Core | Answers `GET /?HOST_INFO` with `NAME`, `OSC_PORT`, `OSC_TRANSPORT` |
| C2 Stable `NAME` | Core | `HOST_INFO` `NAME` unchanged across reboot, and the same as the DNS-SD instance name |
| C3 DNS-SD | Core | Advertises `_oscjson._tcp` with the same name and port |
| C4 Typed leaves | Core | Every writable leaf has `TYPE`; floats and ints have `RANGE` |
| C5 Path stability | Core | Tree hash identical after reboot and after session reload |
| C6 `ACCESS` per client | Core | A client outside the allow-list sees `ACCESS: 1` on writable leaves |
| C7 Subtree GET | Core | `GET /<path>` returns that subtree only |
| L1 `LISTEN` | Live | Websocket `LISTEN` streams value changes at the device's update rate |
| L2 Change notifications | Live | `PATH_REMOVED` and `PATH_RENAMED` sent when a parameter disappears or moves |
| L3 Websocket OSC | Live | OSC over the websocket obeys the same allow-list as UDP |
| L4 `CRITICAL` honoured | Live | `CRITICAL` leaves accept TCP OSC |
| L5 Large tree | Live | Tree of 10,000 leaves served within 2 s on a local network |

Initial subjects: WFS-DIY, XOA/Tight-WFS, the PRISM haptic floor controller, Holophonix, ossia score, Chataigne, the plugin's own server. d&b, L-ISA and Fletcher Machine rows are added as they implement.

## Open questions

- [x] **Reply port for devices without websocket.** Closed 2026-11-04. The proposal gives no way for a client to tell the device where to send OSC feedback, and under the no-new-field rule none is added: no `REPLY_PORT`, no `/register`. Record uses `LISTEN` when the device declares it in `EXTENSIONS`; otherwise the plugin's UDP listener port is shown in the editor and configured on the device side by the user. Record is therefore a v1 feature on every device that can send OSC to a configured address, which is all of them.
- [x] **UUID field name.** Closed 2026-11-04: no UUID, and no new `HOST_INFO` field of any kind. OSCQuery is itself a proposal that needs support, not a rewrite. Identity is `NAME`, the tree hash, and an optional identity leaf the user designates in the device's own tree; what that cannot resolve, the user resolves by pointing once. The same rule removes the `TIMETAGS` field. **Amended 2026-11-10:** the Core and Live profiles still add no field, and everything above stands for a device that offers nothing more. Beside them, an optional extensions draft (`docs/spec/extensions-draft-0.1.md`, first written by Julien Bayle) defines `DEVICE_ID` and `CLOCK`, each declared in `EXTENSIONS`, ignorable and additive. The plugin reads both when a device declares them, and its own server serves both.
- [x] **Slot count variants.** Decided 2026-11-04, by consequence of building VST3 and AU first with JUCE: five plugin IDs (8/16/32/64/128). Hidden works in CLAP and AU, not VST3, and with JUCE each ID is its own binary, so the registry moves to a shared core library.
- [x] **Licence.** Decided 2026-11-04: GPLv3 for the plugin and the client library, matching WFS-DIY and go.dot so their OSCQuery and mDNS code is reused as is. JUCE (AGPLv3 tier) and the VST3 SDK (GPLv3 or Steinberg agreement) make an open-source binary GPL in practice regardless of the source licence, so MIT bought nothing for the plugin. Cost: manufacturers cannot bundle the library in closed firmware; they run the conformance CLI instead.
- [x] **JUCE CLAP status.** Checked 2026-11-04: not in JUCE 9.0.5, and nothing on upstream `develop` mentions CLAP; the Q3 2025 roadmap promise has no date. Decided: VST3 and AU first, our own CLAP shim at milestone 4b. If native support lands later it is a third way to build the same shim interface, not a design change.
- [x] **Nuendo.** Decided 2026-11-04: Nuendo uses VST3 with best-effort naming. The bound path is always visible in the plugin editor, so a lane that keeps its `(unbound)` or stale title still works. Nuendo's actual `kParamTitlesChanged` behaviour is measured and written down in milestone 5, not negotiated with Steinberg.
