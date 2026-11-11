# tests/state

One state-chunk fixture per version ever shipped (devplan, cross-cutting: "every version ever
shipped stays loadable; a fixture per version in `tests/state/`"). `tests/core/StateChunkTests.cpp`
loads each and checks what it reads; a chunk's reader is never deleted.

Fixtures are the one place an OSC address may be written down in this repository: they are
sessions saved against the generic mock device (`tests/fixtures/trees/generic-device.json`),
never a real product's namespace.

| File | Version | What it holds |
| --- | --- | --- |
| `v1-generic.json` | 1 | Two devices (one with an identity leaf, timetags on and an offset), a ranged float, a user-overridden range with the log curve, an `fff` as three consecutive slots, a toggle, an integer set to receive |
| `v1-extras.json` | 1 | Fields this version does not know, at the top level, on a device and on a slot; a slot beyond 8 slots; a slot naming no device; an unknown scaling name |
| `v1-empty.json` | 1 | A session with nothing bound |
| `v1-receive.json` | 1 | One device; a read-only float set to receive, a ranged float set to both, a ranged float set to send (Phase 4: driven through the registry in `tests/core/ReceiveTests.cpp`) |
| `v1-values.json` | 1 | One device, one bound slot, and `values`: every slot's host value, eight of them for an 8-slot table (Phase 5b: what a CLAP restores from its own state) |
| `v1-instance.json` | 1 | `instance` set to 2; one slot bound through a `path_pattern` with `$instance`, one through a plain path (Phase 6d-2; `tests/core/PatternTests.cpp`) |
| `v1-device-id.json` | 1 | Two devices, the first with Phase 6x's two additive fields, the second with neither: `device_id` (the `DEVICE_ID` the device declared) and `timetags_by_hand` (a hand set `honours_timetags`, so it stands over what the device's `CLOCK` declares); one bound slot. The version stays 1 and each is written only where it is set |
