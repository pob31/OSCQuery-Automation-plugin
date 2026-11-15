# Clock and Timing — OSCQuery Automation Plugin

Nov 15, 2026 · @Pierre-Olivier Boulant

## Purpose and scope

This note defines how the plugin times its OSC output relative to the audio clock of an AoIP network (Dante, AES67, RAVENNA), and what it does when there is no such clock. It complements the PRD, which owns transport, bundling and the per-device offset; this document owns the clock modes and what they ask of the optional `CLOCK` extension (`docs/spec/extensions-draft-0.1.md`), which sits beside the Core and Live profiles and adds no field to either.

It is a design proposal: none of it is built. *Status against the code*, near the end, says what the plugin does today and which decisions of the PRD this note would amend.

The plugin runs in two situations with different timing needs:

- **In a DAW**, with a timeline. The goal is alignment: an automation point at sample N should act on the device when sample N is heard.
- **Outside a DAW**, as a bridge for a hardware controller or a show-control app, with no timeline. The goal is simultaneity and determinism: one gesture should reach every device at the same instant, with path jitter absorbed.

Three modes cover both, in order of preference: click calibration, PTP observer, immediate.

## Rate and anchor

"Sync" hides two separate problems.

**Rate.** Do the DAW's samples and the device's samples tick at the same speed? When the DAW's audio interface is on the AoIP network (Dante Virtual Soundcard, a Dante or RAVENNA card), its sample clock is the PTP media clock and the rate is locked by cabling. No drift, nothing to do in software. When the DAW runs on an internal or USB clock, the rate is free and any offset drifts.

**Anchor.** Which PTP instant does DAW sample 0 correspond to? The audio driver knows and does not expose it on macOS or Windows. The plugin sees only block callbacks and, in some formats, a host-time stamp for the block: AU passes `AudioTimeStamp.mHostTime`, VST3 has `ProcessContext.systemTime` when the host sets `kSystemTimeValid`, CLAP passes no host time, only `steady_time`, a sample counter the host may leave at −1, and the plugin stamps the callback itself.

Once the rate is locked, the anchor is a constant, and a constant is measured once rather than estimated continuously. That is the whole idea behind Mode A. Mode B exists for the case where there is no sample stream to anchor to.

## Mode A: click calibration

For a DAW whose interface is clocked by the AoIP network and that already sends audio to the device over it. No PTP parsing at either end.

1. The plugin emits a click on one audio channel at a known sample position N and, in the same block, an OSC message carrying N to the device's click leaf (`/clock/click` in the examples here; the plugin reads the path from the device and types none).
2. The device detects the click's arrival at its own sample M (onset detection; sub-sample with interpolation).
3. M − N is the anchor: a constant that includes the DAW's output latency and the Dante/AES67 transport latency. The device reports it back over OSC and the plugin stores it per device.
4. From then on, timetag = anchor + sample position. Sample-accurate.

**Re-anchoring.** The click is cheap, so it runs again:

- on transport start, because some hosts stop calling `process` when idle and the plugin's sample counter is no longer trustworthy;
- when the host reports a buffer-size or output-latency change (Reaper and Live expose this);
- every few minutes as a health check. A moving anchor means the DAW is not on the network clock; the plugin then drops to Mode B, or warns if no PTP is visible.

**Requirements on the device.** A click endpoint, onset detection on a designated input, and a reply with the measured offset and confidence. The endpoint is a leaf of the device's tree and `CLOCK` names its path (below), so the plugin discovers whether Mode A is available.

**Not applicable** when the device cannot see the DAW's audio, when the DAW runs on an internal clock, or when there is no audio at all (the controller-bridge case).

## Mode B: PTP observer

For the plugin running outside a DAW with no timeline and no audio: a bridge for a hardware controller, a show-control app, a console. Here there is nothing to be sample-accurate to. A gesture happens now, and the clock exists to make "now" land on every device at the same instant.

**What the shared clock buys**

- **Simultaneity.** Timetag = now + a fixed lead (default 2 ms). Every device following the same grandmaster fires together; the lead absorbs path jitter. This is Dante's receive-latency idea applied to control data.
- **Order and interpolation.** Timetagged bundles let a device order messages from several senders correctly and interpolate between known-time points instead of guessing from arrival times.

**What the observer is**

- Listen-only. Joins 224.0.1.129 and parses Sync and Follow\_Up for PTPv1 (IEEE 1588-2002, what Dante uses; it has no Announce, and the grandmaster's properties travel in Sync) and Sync, Follow\_Up and Announce for PTPv2 (AES67, RAVENNA, Dante in AES67 mode). Never sends Delay\_Req; the plugin must not appear as a port on someone's Dante network.
- Keeps a slowly filtered estimate of (PTP − `CLOCK_MONOTONIC`). Nothing is disciplined; the system clock is untouched, which is also what avoids the NTP collisions Inferno's author documents.
- Reports grandmaster identity and domain, so the Monitor can flag a device that follows a different grandmaster. That is the failure that silently breaks simultaneity, and it happens the moment a second Dante network is plugged in.
- Accuracy needed: agreement with the devices well inside the lead. Software timestamping gives 50 to 500 µs on a quiet wired LAN, which is plenty against 2 ms. Wi-Fi is milliseconds; the plugin refuses to claim sync over it.

**Limits.** The clock covers only the plugin-to-device leg. The controller feeds the bridge over MIDI or OSC with its own jitter and no timestamp; the bridge timestamps on receipt. Console-to-renderer determinism is not on offer and the documentation should say so.

**Device side.** The device needs the same observer, since DVS and the PCIe cards do not expose PTP time to applications. The observer lives in the client library (`lib/oscquery`, GPLv3) so both ends can run identical code; closed firmware cannot link it (PRD, Licence) and writes the same observer from this note. On Linux, reading the PHC through a running ptp4l or Statime is an optional backend with hardware timestamps.

## Mode C and mode selection

**Mode C: immediate.** No shared clock at all (a non-AoIP network, a laptop on Wi-Fi). Bundles carry the immediate timetag and devices execute on arrival. Jitter is whatever the network gives, typically tens to hundreds of microseconds on wired Ethernet, which is acceptable for control data that the device smooths anyway. It is also the mode every device supports today, so it is the compatibility floor.

**Selection** is automatic, per device, in this order:

| Condition | Mode | Alignment |
| --- | --- | --- |
| Audio reaches the device and the measured anchor is stable | A, click | sample-accurate |
| PTP visible on the network and the device declares a `CLOCK` whose `SOURCE` is `ptp` | B, observer | sub-millisecond, drift-free |
| Neither | C, immediate | arrival time |

The Monitor shows the mode in effect per device, the anchor or offset, and its confidence. A drop from A to B or from B to C is a visible event, never silent, because the usual cause is a cabling or clocking change someone should know about.

## Timetags, the `CLOCK` field, per-device offset

**Timetags stay standard OSC.** NTP format, 1900 epoch, UTC. PTP is TAI with a 1970 epoch, so the observer converts with the 2,208,988,800 s epoch offset and `currentUtcOffset` from the Announce message in PTPv2 and from Sync in PTPv1 (37 s at the time of writing). No PTP-flavoured timetag is invented; a device that honours timetags at all can honour these.

**`CLOCK` in `HOST_INFO`** is the optional extension drafted in `docs/spec/extensions-draft-0.1.md`: declared in `EXTENSIONS`, ignorable, additive, and outside the Core and Live profiles, which add no field (decided 2026-11-04 and 2026-11-10). The draft has `SOURCE`, `DOMAIN` and `SCHEDULING`, and the plugin reads those three today. This note proposes three keys more, for the draft to take or refuse. Absent, or present but not listed in `EXTENSIONS`, means Mode C only.

```json
"CLOCK": {
  "SOURCE": "ptp",
  "DOMAIN": "0",
  "SCHEDULING": true,
  "PTP": { "VERSION": 1, "GRANDMASTER": "00-1d-c1-ff-fe-12-34-56" },
  "CLICK": { "PATH": "/clock/click", "INPUT": 7 },
  "SAMPLE_RATE": 48000
}
```

- `SOURCE` and `DOMAIN` (as drafted): which clock the device follows. `ptp` with its domain is what Mode B needs.
- `SCHEDULING` (as drafted): the device schedules bundles by their timetag rather than executing on arrival.
- `PTP` (proposed): the PTP version and the grandmaster the device follows. The plugin listens to that domain and version, and the Monitor compares grandmasters across devices.
- `CLICK` (proposed): the device supports Mode A; `PATH` is its click leaf and `INPUT` the channel on which it listens for the calibration click. The reply carries the measured offset in samples and a confidence figure.
- `SAMPLE_RATE` (proposed): lets the plugin express the anchor in the device's sample units.

**Per-device offset.** The signed offset in milliseconds from the PRD is unchanged. In Mode A it is added to the anchor (to pull a renderer with extra internal latency into line); in Mode B it adjusts the lead; in Mode C it becomes a send delay. One number, one meaning: how far this device should lag or lead the others.

## Where the latency is

| Stage | Magnitude | Who controls it |
| --- | --- | --- |
| Host delivers automation once per block | 1.3 ms at 64 samples, 10.7 ms at 512 (48 kHz); sample offsets within the block are known at block start on CLAP and VST3 | DAW buffer size |
| Audio thread to network thread | a few hundred µs through a lock-free queue; near zero with the direct non-blocking `sendto` option | plugin setting |
| Wire and switch | tens of µs on wired Ethernet; milliseconds and variable on Wi-Fi | network |
| Device execution | arrival, or the timetag in Modes A and B | device |
| DAW output latency | buffer plus converter, typically 2× block plus a few ms | DAW and interface |

The last row is why OSC normally runs *ahead* of the audio: a packet sent at block start reaches the device before that block leaves the converters. Raw speed is rarely the problem. Alignment is, and that is what Modes A and B fix. In Mode B the lead is deliberately spent to buy simultaneity; 2 ms is below a single 128-sample block and well below anything a listener can place.

## Test matrix additions

These check the `CLOCK` extension, not Core or Live: as in the extensions draft, a device that does not declare `CLOCK` is not checked and does not fail.

| Check | Profile | What passes |
| --- | --- | --- |
| K1 `CLOCK` declared | Extension | `EXTENSIONS` lists `CLOCK` and `HOST_INFO` carries it with `SOURCE` and `SCHEDULING` (X2 of the extensions draft) |
| K2 Timetags honoured | Extension | A bundle timetagged 50 ms ahead executes within ±1 ms of that time, measured on the device's output |
| K3 Click round trip | Extension | The click leaf returns an offset; ten repeats agree within ±1 sample on a network-clocked DAW |
| K4 Anchor stability | Extension | Anchor unchanged within ±1 sample over 30 minutes with the DAW on the AoIP clock |
| K5 Observer agreement | Extension | Plugin's PTP estimate vs. a Dante device's own PTP time, captured with a network tap: under 250 µs on wired Ethernet |
| K6 Grandmaster mismatch | Extension | Monitor flags a device following a different grandmaster within one Announce interval (PTPv2) or one Sync interval (PTPv1) |
| K7 Mode fallback | Extension | Taking the DAW's interface off the AoIP clock drops the mode A → B, and taking PTP off the network drops it B → C; each visibly, without stalling output |

K5 is the number manufacturers will ask for first; it is also the first published figure for software PTP observation on a Dante network from a plugin, so the capture method should be documented with the result.

## Status against the code, 2026-11-15

What the plugin does today, against what is proposed above:

- **Mode C is the default.** Bundles carry the immediate timetag unless *honours timetags* is on for the device.
- **There is a fourth case, which the three modes do not name.** With *honours timetags* on, the tag is the system wall clock at the send plus the per-device offset (`src/core/send/TimeTags`): neither an anchor nor a PTP time. The setting is ticked by hand in the Monitor or the device list, or pre-filled from a declared `SCHEDULING`, and the hand wins over the device's word (PRD, Transport, 2026-11-10). No bench device schedules by timetag, so it is verified on the mock only.
- **The offset is applied to timetags only.** The send delay for a device with the setting off is in the PRD and is not built (devplan, Phase 6x, the author's open items), so the Mode C half of *Per-device offset* is not true yet. A send delay can make a device lag, never lead.
- **One value per slot per block.** JUCE's `AudioProcessor` delivers no sample offsets, and the CLAP shim consumes them and hands the core the block's last value (devplan, Phase 3 record, amended 2026-11-09). The sample-accurate send Mode A assumes is the open item named there.
- **No shim reads a host time**: not `mHostTime`, not `systemTime`, not `steady_time`.
- **Audio is untouched.** One stereo bus each way, and `processBlock` leaves the buffer alone.
- **`CLOCK` is read as drafted**: `SOURCE`, `DOMAIN` and `SCHEDULING`, only when `EXTENSIONS` lists it (`lib/oscquery/oscquery/HostInfo.cpp`). The plugin's own server declares `local` and no scheduling. A received bundle's timetag is decoded and dropped.
- **No PTP code, no build outside a host** (VST3, AU and CLAP only), and no MIDI input.

Decisions this note would amend. Each is the PRD's to take first, since the PRD wins:

- *No audio processing in the plugin; audio passes through untouched* (non-goals). Mode A's click is audio the plugin writes.
- *No clock or transport is sent* (Timing). The click message is one.
- *Whether a device honours timetags is a per-device setting the user ticks* (Transport). Automatic selection replaces that setting or sits under it, and this note does not say which.
- The plugin outside a DAW, fed by a controller over MIDI or OSC, is not in the PRD: its goals are DAW automation lanes, and its headless use is the plugin's own server inside a host.

## References

- [Statime](https://github.com/pendulum-project/statime), Rust PTP implementation, MIT/Apache-2.0. The PTPv1 slave lives in [teodly/statime, branch `inferno-dev`](https://github.com/teodly/statime/tree/inferno-dev); upstream PR #602 is still an open draft. This is the reference for the observer's PTPv1 parsing; its licence is compatible with the plugin's GPLv3.
- [Inferno](https://github.com/teodly/inferno), unofficial Dante implementation, GPLv3-or-later or AGPLv3-or-later. Read as documentation, not taken as code: its README documents Dante's PTPv1 use, the NTP-collision problem with software timestamps, and the monotonic virtual-clock approach the observer adopts. Its `usrvclock` protocol is a documented way for a daemon to hand a clock to applications on Linux.
- IEEE 1588-2002 (PTPv1) and IEEE 1588-2008 (PTPv2); AES67-2018 for the PTPv2 media profile.
- OSC 1.0 timetag format (NTP, 64-bit fixed point).

## Open questions

- [ ] **Click detection on the device.** Onset detector design, and whether to use a dedicated channel or a short burst on a working one. A chirp with matched-filter detection gives sub-sample precision and survives being mixed with programme.
- [ ] **Hosts that mute the plugin's audio output.** Mode A needs the click to reach the device; a plugin on a control-only track has no audio path. A dedicated "clock" track is a workable convention; the Monitor could own it.
- [ ] **PTPv1 subdomain naming.** Dante uses the `_DFLT` subdomain and non-default multicast addresses in some configurations; confirm against captures before fixing the observer's defaults.
- [ ] **Leap seconds.** `currentUtcOffset` changes rarely but the conversion must not jump a running show; apply changes only at re-anchor.
- [ ] **Mode A's timetag.** Step 4 counts in samples, and a timetag is NTP time. Which end turns a sample position into a time, and against which clock, is not said. The count also has to be the plugin's own running counter (or CLAP's `steady_time`), since the timeline position jumps at every loop and locate.
- [ ] **Mode B inside a DAW.** The mode is defined for the case with no timeline, and the selection table also falls back to it in a DAW. There the anchor is still unknown, so "now plus the lead" gives simultaneity across devices and not alignment with the audio; "sub-millisecond" in the table is agreement between devices.
- [ ] **Dante's timescale.** The conversion to UTC assumes the grandmaster keeps TAI. Whether a Dante leader does, or counts from an arbitrary start, is unconfirmed. If it is arbitrary, the tags agree only between ends that read the same PTP, and a device scheduling against an NTP-disciplined clock cannot honour them. PTPv2 says which in Announce (`ptpTimescale`). Confirm against captures.
- [ ] **Binding the PTP ports.** Sync arrives on UDP 319, Follow\_Up and Announce on 320. Both are below 1024, which an unprivileged process on Linux cannot bind by default, and a machine running Dante Virtual Soundcard has them bound already, so the observer has to share them.
- [ ] **Port Statime or bind it.** A few hundred lines of C++ ported from the Rust, versus linking the crate through a C ABI. Port is the default unless the Rust side keeps moving.
