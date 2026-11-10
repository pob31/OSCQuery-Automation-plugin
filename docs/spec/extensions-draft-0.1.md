# OSCQuery optional extensions: device identity and clock

**Draft 0.1**, shared for discussion by Julien Bayle, 2026-11-10; amended the same day by
Pierre-Olivier Boulant (what changed is listed at the end). Copyright (C) 2026 Pierre-Olivier
Boulant, Julien Bayle. This text is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), independent of this repository's code
licence. Not proposed to Vidvox, ossia or any standards body.

These extensions are **optional and outside the Core and Live profiles**
([`core-profile-draft-0.1.md`](core-profile-draft-0.1.md)). The profile still adds no field to
OSCQuery: a device passes Core and Live without any of what follows, and a client that knows these
extensions behaves, with a device that does not declare them, exactly as the profile describes.

## Principles

Every extension follows four rules, so that using them never takes an implementation out of OSCQuery:

1. **Declared**: the server lists it in `HOST_INFO` `EXTENSIONS`; a client uses it only with a server that declares it. A key that is present but not declared is ignored.
2. **Ignorable**: a peer that does not know it works exactly as before and loses nothing.
3. **Additive**: it adds a key or a command; it never changes the meaning of an existing one (for instance, `LISTEN` keeps its per-address meaning).
4. **Neutral and written**: no vendor or lab prefix, a short written spec, a test in a shared bench.

## `DEVICE_ID` and `TREE_HASH`

Status: draft, to design together.

### Purpose

A client should recognise a device by a stable identifier, not by its name: two devices can share a name, and mDNS may rename one ("name (2)") differently from one launch to the next.

### `DEVICE_ID`

In `HOST_INFO`, declared in `EXTENSIONS`:

```json
"DEVICE_ID": "6f1c2a9e-0b4d-4e2a-9f7c-3d5b8a1e2c47"
```

- A UUID, as a string, generated once per device instance. A client compares it as an opaque string and reads nothing into it.
- Where it is kept: an application keeps it with its project or session, so it survives restarts and renames; a hardware unit keeps it in its non-volatile store. An application with no project of its own (a plugin's server inside a DAW) keeps it per user and per application.
- `NAME` stays the human label; it may change, the ID does not.

### `TREE_HASH`

The tree hash is the one the Core profile defines for C5, and is not restated here: SHA-256, as
lowercase hex, of one line `FULL_PATH` TAB `TYPE` LF per leaf that has a `TYPE`, the lines sorted by
byte value. One definition, in one place, so two implementations cannot drift apart.

- Changes when the tree structure changes, not when values change.
- Use: notice that a device changed since it was bound, and pick between several candidates.
- A client computes it from the tree it reads. Whether a device also serves it is an open question below.

### Client resolution order

A saved `DEVICE_ID` is compared first, when the device declares one. After it, the order a profile
client already follows stands unchanged:

1. The last address: `DEVICE_ID` when both sides have one; otherwise `NAME`, and the identity leaf's value when the user designated one.
2. DNS-SD: a device declaring the saved `DEVICE_ID`, whatever its `NAME`; otherwise candidates under the saved `NAME`, told apart by the identity leaf, then by the tree hash.
3. What is still ambiguous, the user points at once.

Rules:

- **Two live devices declaring one `DEVICE_ID` are ambiguous.** The client never picks one silently; it goes to step 3.
- A candidate that declares a *different* `DEVICE_ID` from the saved one is never adopted automatically, even under the right `NAME`; the user may still point at it.
- A saved tree hash that differs must not be silently accepted: the client says so.

### Open questions

- `TREE_HASH` served in `HOST_INFO`, or as `/?TREE_HASH`, or left to the client to compute.
- A copied project keeps its ID. The client side is closed above (ambiguous, never a silent pick); whether a device detects the copy and regenerates, or warns, is open.
- Where each application keeps the ID: Max patch, TouchDesigner project, Unity scene, plugin state.
- Fit with the open Vidvox issue #64 ("METADATA field in HOST_INFO") and with DNS-SD TXT records.

## `CLOCK`

Status: draft, to design together.

### Purpose

OSC bundles already carry an NTP timetag; OSCQuery lacks a way to say which clock it refers to and what a receiver does with it.

Today, senders mostly use the "immediate" timetag (1). The OSCQuery Automation plugin can send real
timetags, per device and off by default: the tag is the wall clock at the send plus a per-device
offset the user sets, within a millisecond of it on a mock device over loopback
(`docs/bench/2026-11-08-phase3-send.md`). No device on its bench schedules by timetag yet.

### Declaration

In `HOST_INFO`, declared in `EXTENSIONS`:

```json
"CLOCK": { "SOURCE": "ptp", "DOMAIN": "0", "SCHEDULING": false }
```

- `SOURCE`: what disciplines the clock the device's timetags refer to: `ntp`, `ptp`, or `local` (free-running, comparable only to itself).
- `DOMAIN`: optional, which PTP domain or NTP server, so two devices can tell whether they share a clock.
- `SCHEDULING`: whether the device executes a future timetag at that time (`true`) or applies values on arrival and only passes the timetag on (`false`).

The timetag itself does not change with `SOURCE`: it is always OSC's 64-bit NTP format, seconds
since 1900 on the UTC scale. A device on PTP, whose timescale is TAI, removes the TAI offset
itself; `SOURCE` says where the time comes from, not which epoch the tag is in.

### Rules

- A sender puts a real timetag only when it means something; otherwise immediate.
- A receiver keeps the timetag and hands it to its application with the value, even when it does not schedule.
- Values of one tick travel in one bundle, changed values only, split at the datagram size.

### Open questions

- Which reference clock on a platform that already runs PTP for Dante or AES67 audio; reference IEEE 1588 and AES67 rather than define new values.
- `DOMAIN` is one string for two different things, a PTP domain number and an NTP server; whether it stays one field.
- A value of `SOURCE` for an operating-system clock whose discipline the software cannot know. Until there is one, such a device declares `local`, which claims the least.
- A client has no way to read the device's current time, so the offset between the two clocks is still set by hand. A read-only clock leaf in the device's tree would close this without a new command.
- Late bundles: apply at once, drop, or report.
- Timetags on WebSocket-carried values (binary OSC bundles) as well as UDP.

## Checks

For the conformance tool, when it exists (it runs one check per row; a device that does not declare
an extension is not checked for it and does not fail):

| Check | Passes when | How it is run |
| --- | --- | --- |
| X1 `DEVICE_ID` | Listed in `EXTENSIONS`; a non-empty string; the same after a restart | Two `HOST_INFO` snapshots, before and after |
| X2 `CLOCK` | Listed in `EXTENSIONS`; an object whose `SOURCE` is one of the values above and whose `SCHEDULING` is a boolean | One GET |

## Amended 2026-11-10

Against the text as first shared (pull request #1):

- The tree hash is cited from Core C5 instead of restated; the first text gave it as "sorted lines `FULL_PATH TYPE`", which leaves the separator and the line ending open.
- "Measured accurate to 0.2 ms" is replaced by what the bench report records. The nearest figure there (227 µs at the 99th percentile) is the time from the audio block to the wire, a different quantity.
- The resolution order keeps the identity leaf and the user pointing once, which the first text left out, and says what a client does with a duplicated or a different `DEVICE_ID`.
- "UUID v4, kept by the host with the project" becomes a UUID kept where each kind of device can keep it; "host" is avoided, since it also means the DAW and `HOST_INFO`.
- The timetag's format is stated to be independent of `SOURCE`.
- Open questions added: `DOMAIN`'s two meanings, a `SOURCE` for an unknown discipline, reading the device's clock.
- A checks table is added.
