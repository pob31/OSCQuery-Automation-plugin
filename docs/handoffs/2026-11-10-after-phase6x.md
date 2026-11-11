# Handoff, 2026-11-10: Phase 6x built, its farm, CI and DAW rows open, Phase 7 next

For the next Claude Code session on this repository, and for the author. Read this, then
`CLAUDE.md`, then the devplan's *How to read this*, its Phase 6x record, and Phase 7.

## Where things stand

- **Julien Bayle has started contributing** (pull request #1, merged 2026-11-10), so **every change
  goes through a pull request now**, on a branch, CI green before the merge. `CLAUDE.md` says so.
- **Pull request #2 (text)**, branch `docs/extensions-layer`: Julien's draft moved to
  `docs/spec/extensions-draft-0.1.md` and corrected, the PRD, `CLAUDE.md`, C5 and the devplan
  amended. **The code's pull request is stacked on it**, branch `extensions-device-id-clock`:
  merge #2 first.
- **Phase 6x is built** (2026-11-10, commits 6x-1 to 6x-6): the plugin reads the optional
  `DEVICE_ID` and `CLOCK` when a device declares them, and its own server serves both. Report:
  `docs/bench/2026-11-10-phase6x-extensions.md`.
- **Measured** (Windows, Debug, loopback): `ctest --preset vs-debug`, 8 of 8 entries, C and fr_FR.
- **CI green on three platforms** on the code's pull request (#3), and **the farm passes** on both
  machines (fresh clones in `~/oqa-6x`; `~/oqa-6b` on each still holds uncommitted edits from an
  earlier session and was left alone).
- **Not measured:** WFS-DIY live, any DAW. The report's last table lists the rows.

## Decided this session (the author, 2026-11-10)

1. Pull request #1 merged as it was, corrected on top.
2. `DEVICE_ID` and `CLOCK` are optional extensions beside the profile. Core and Live add no field.
3. The plugin is their client and their server.

The devplan's Phase 6x record holds the implementer's decisions, all the author's to overrule. The
ones most worth a look: `timetags_by_hand` (a chunk field that was not in the plan), the seed file
`<settings file>.server-id`, the server's `CLOCK` declaring `local`, and a UUID of version 8.

## What may move

- **The names and the shape of the two keys.** Julien Bayle and Jean-Michaël Celerier (libossia)
  are discussing the draft. Julien's fork (`github.com/gluon/libossia`) held nothing on either
  extension on 2026-11-10; upstream libossia's `HOST_INFO` has `NAME`, `WS_IP`, `WS_PORT`,
  `OSC_PORT`, `OSC_TRANSPORT`, `EXTENSIONS` and an extension of its own, `OSC_STREAMING`. On this
  side the names are `deviceIdExtension` and `clockExtension` in
  `lib/oscquery/oscquery/HostInfo.h`, with the `CLOCK` reader in `HostInfo.cpp` and
  `ClockInfo::toVar`. Nothing is taken from libossia's code (`CLAUDE.md`); what is harmonised is
  the draft's text.
- **The tree hash**, if another implementation serves or compares one: it must be Core C5's, line
  for line, or the two will never agree on a device.

## Open, the author's

- The send delay the PRD and two tooltips promise for a device with timetags off does not exist.
- The plugin's server emits `WS_PORT`; the Phase 6 plan said it was absent.
- A device seen on two interfaces is two candidates under one identifier, and is left to the user.
- The time tag of a received bundle is decoded and dropped; placing recorded automation by it
  waits for an agreement on the clock (the draft's open questions).
