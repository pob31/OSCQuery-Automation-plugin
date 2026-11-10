# OSCQuery: device identity and clock, shared drafts

Shared for discussion by Julien Bayle, 2026-11-10. Text licence: CC BY 4.0, independent of this repository's code licence. Not proposed to Vidvox, ossia or any standards body.

## Principles

Every extension follows four rules, so that using them never takes an implementation out of OSCQuery:

1. **Declared**: the server lists it in `HOST_INFO` `EXTENSIONS`; a client uses it only with a server that declares it.
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

- Generated once per device instance (UUID v4) and kept by the host with the project, so it survives restarts and renames.
- `NAME` stays the human label; it may change, the ID does not.

### `TREE_HASH`

Method from the OSCQuery Automation plugin (Pierre-Olivier Boulant):

- SHA-256, in hex, of the sorted lines `FULL_PATH TYPE`, one per leaf.
- Changes when the tree structure changes, not when values change.
- Use: recognise a device without re-reading its tree, and pick between several candidates.

### Client resolution order

`DEVICE_ID`, then `NAME` with `TREE_HASH`, then `NAME` alone. A saved `TREE_HASH` that differs must not be silently accepted.

### Open questions

- Served in `HOST_INFO` or as `/?TREE_HASH`.
- A copied project keeps its ID: two live devices with one `DEVICE_ID`. Detect and regenerate, or warn?
- Where each host keeps the ID: Max patch, TouchDesigner project, Unity scene, plugin state.
- Fit with the open Vidvox issue #64 ("METADATA field in HOST_INFO") and with DNS-SD TXT records.

## `CLOCK`

Status: draft, to design together.

### Purpose

OSC bundles already carry an NTP timetag; OSCQuery lacks a way to say which clock it refers to and what a receiver does with it.

Today, senders mostly use the "immediate" timetag (1). The OSCQuery Automation plugin already sends real timetags with an offset, measured accurate to 0.2 ms.

### Declaration

In `HOST_INFO`, declared in `EXTENSIONS`:

```json
"CLOCK": { "SOURCE": "ptp", "DOMAIN": "0", "SCHEDULING": false }
```

- `SOURCE`: the clock the device's timetags refer to: `ntp`, `ptp`, or `local` (free-running, comparable only to itself).
- `DOMAIN`: optional, which PTP domain or NTP server, so two devices can tell whether they share a clock.
- `SCHEDULING`: whether the device executes a future timetag at that time (`true`) or applies values on arrival and only passes the timetag on (`false`).

### Rules

- A sender puts a real timetag only when it means something; otherwise immediate.
- A receiver keeps the timetag and hands it to its host with the value, even when it does not schedule.
- Values of one tick travel in one bundle, changed values only, split at the datagram size.

### Open questions

- Which reference clock on a platform that already runs PTP for Dante or AES67 audio; reference IEEE 1588 and AES67 rather than define new values.
- Late bundles: apply at once, drop, or report.
- Timetags on WebSocket-carried values (binary OSC bundles) as well as UDP.
