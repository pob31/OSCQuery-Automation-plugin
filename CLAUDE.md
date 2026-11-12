# CLAUDE.md

The two sections below are **PRD *Goals and non-goals* and *Binding model*, reproduced
verbatim** from the PRD in `docs/`. They are the review criterion for every pull request.

**If this file and the PRD disagree, the PRD wins and this file is stale. Fix it by copying,
never by editing.**

---

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


---

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


---

## Where to look

- **`docs/PRD — OSCQuery Automation Plugin (CLAP VST3 AU).md`** — the product requirements; the
  source of the two sections above and of every decision, each dated in place.
- **`docs/Devplan — OSCQuery Automation Plugin (draft 0.1).md`** — the phase plan: what is built
  now, in what order, what each phase needs, and the facts checked on the way. Read *How to read
  this* first. Start at Phase 0.

## Standing rules, decided 2026-11-04

- **C++20 on JUCE 9.0.5**, pinned at the same commit as WFS-DIY. VST3 and AU first through
  JUCE's own wrappers; CLAP later through a shim of our own against the CLAP C API (plan
  Phase 5b). Neither `clap-juce-extensions` nor `clap-wrapper` is in the build.
- **GPLv3** for the plugin and the client library. File headers follow go.dot's form and read
  `Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle`. The profile text in `docs/spec/` is
  **CC BY 4.0**, so manufacturers can quote it (decided 2026-11-04).
- **Plugin identity, fixed for good:** company "Pix et Bel", manufacturer code `PxBl` (as
  WFS-DIY), plugin codes `Q008` `Q016` `Q032` `Q064` `Q128` and `QMon` for the Monitor,
  `JUCE_VST3_CAN_REPLACE_VST2=0`. The VST3 and AU IDs derive from these; changing one orphans
  every saved session. Shims are `src/shims/juce/` (VST3 and AU) and `src/shims/clap/`
  (decided 2026-11-04). **The CLAP ids are `io.github.pob31.oqa.Q008` … `Q128`**, the bundle
  ids the JUCE shim gives the same codes, one `.clap` listing all five, and `io.github.pob31.oqa.QMon`
  for the Monitor; its macOS bundle id is `io.github.pob31.oqa.clap` (decided 2026-11-09).
- **The Core profile adds no field to OSCQuery.** No UUID, no `TIMETAGS`, no `REPLY_PORT`.
  Anything the plugin needs beyond the proposal is a plugin-side setting or lives in the device's
  own tree. OSCQuery is itself a proposal that needs support, not a rewrite.
  **Beside the profile, two optional extensions** (decided 2026-11-10): `DEVICE_ID` and `CLOCK`,
  drafted in `docs/spec/extensions-draft-0.1.md` (first written by Julien Bayle). Each is declared
  in `EXTENSIONS`, ignorable and additive. The plugin uses one only when the device declares it
  (a key present but not declared is ignored) and behaves exactly as before with a device that
  declares neither; its own server serves both. No Core or Live clause may come to depend on them.
- **No typed OSC address anywhere in `src/`.** Addresses appear only in test fixtures. The one
  exception is the plugin's own server's namespace, which the PRD fixes (`/plugin/<instance>/slot/<n>/...`,
  UX, Headless use): its segments live in `src/core/server/ServerNamespace.h` and nowhere else
  (decided 2026-11-09, Phase 6).
- **Standardisation is out of scope.** The profile draft is published in `docs/spec/`; who
  adopts it is not this project's deliverable.
- **Pull requests, from 2026-11-10.** Commits went straight to `main` until Julien Bayle started
  contributing; his first pull request was merged that day, so every change to the plugin's
  code arrives as one, on a branch, with CI green before the merge. A change that touches only
  documentation (`docs/`, `CLAUDE.md`, READMEs) or only tests (`tests/`) may go straight to
  `main` (decided 2026-11-12). Never rewrite history on `main`.

## Code to reuse, and where it is

Sibling repositories on the author's machine, both GPLv3, both on JUCE 9.0.5:

- **WFS-DIY** (`../WFS_DIY_v1`): `Plugin/Source/Shared/OscQueryClient.{h,cpp}` is the
  behavioural reference for talking to WFS-DIY's server (the LISTEN shape, re-LISTEN on open);
  nothing is copied from it (no licence header, no HOST_INFO parsing). `OscTransport` is **not**
  used: OSC comes from go.dot (decided 2026-11-08). `RateLimiter` lent Phase 3's per-slot limiter its shape only (latest value per
  key, compared with what was last sent): its mutex and timer were not copied, and its
  parameter-to-OSC path takes locks and allocates on the audio thread, so nothing of that was
  either. The Bridge (`BridgeApi.h`, `BridgeLoader`) lent the registry's ABI pattern (exact-match
  version, found beside the plugin by walking up from its own module); its threading was not
  copied, and where it goes silent without its library this plugin falls back to its own core
  and says so (Phase 3, 2026-11-08). `Source/Network/OSCQueryServer.cpp` is the first conformance subject.
  `Plugin/Scripts`, `Plugin/Installer` and `.github/workflows/plugins-ci.yml` are the installer
  and CI patterns to copy.
- **go.dot** (`../go.dot`, at `1c8aaa0a`), copied into `lib/oscquery/` in Phase 2:
  `src/wfg/engine/oscquery/Mdns.{h,cpp}`, `Discovery.{h,cpp}`, `MulticastJoin.{h,cpp}`
  (multicast DNS written in-house: codec, browser on every interface, announcer);
  `src/wfg/engine/osc/OscValue`, `OscAddress`, `OscCodec` (OSC 1.1, pure), `UdpEndpoint`,
  `SocketBuffers`; `oscquery/OscQueryClient::get` (the raw-socket HTTP GET). Its own JSON
  (`json/JsonValue`) is not used: the tree model stays on `juce::JSON`, whose number text is
  locale-independent (`tests/LocaleTests.cpp`).

Copied files keep their original header and gain a one-line origin note. Nothing is taken from
ossia, libossia or any other OSCQuery implementation.

## The bench, as of 2026-11-04

- **Hosts on this Windows machine:** Reaper and Samplitude (both VST3), Ableton Live 12.4.16
  (updated 2026-11-08), and Bitwig Studio 6.1.4 (VST3 and CLAP; installed 2026-11-04). No Nuendo.
- **OSCQuery applications on hand**, as devices to bind, as sources to record from, and as
  conformance subjects: WFS-DIY (a Release build at
  `../WFS_DIY_v1/Builds/VisualStudio2022/x64/Release/App/`), Chataigne, ossia score, and go.dot
  (`../go.dot`). Each serves a tree with LISTEN, so a value moved in any of them is a record
  test without a hardware controller.
- **Other machines on the author's farm**, over SSH with key auth, online when the author is at
  home (172.22.x is the home network; aliases in the author's `~/.ssh/config`). Inventoried
  2026-11-04:
  - `ssh macmini`: Mac mini, Apple silicon, macOS 15.8.2, Xcode 26.3, cmake 4.2 and ninja from
    Homebrew (`/opt/homebrew/bin` is not on the non-interactive `PATH`; export it). Hosts:
    REAPER (7.85 on 2026-11-08; it updates itself), Ableton Live 12.4.16 Suite, GarageBand
    10.4.14, Bitwig Studio 6.1.4 (installed 2026-11-04). **No Logic.** `auval` present; pluginval 1.0.4 unpacked by
    `scripts/farm-check.sh` under `~/Documents/pluginval`.
    WFS-DIY.app and Go.dot.app installed; checkouts under `~/Documents/go.dot` and
    `~/Documents/WFS-DIY_JUCE`, a JUCE tree at `~/JUCE`.
  - `ssh nuc`: Intel NUC, Ubuntu 24.04.5, GCC 13.3, clang 18, cmake 3.28, ninja; the JUCE
    Linux dev packages are installed (ALSA, X11, freetype, GTK, webkit, JACK); pipewire and
    jackd present; gdb and coredumpctl. **REAPER 7.84** at `/opt/REAPER` (`reaper` on `PATH`).
    GNOME with gdm3 is installed but the SSH session is a tty with no `DISPLAY`: for a plugin
    editor, log the `pob` user into the desktop at the machine (or enable autologin) and run
    with `DISPLAY=:0`, or install Xvfb. WFS-DIY's LV2 plugins sit in `~/.lv2`. go.dot and WFS-DIY
    checkouts under `~/go.dot` and `~/WFS-DIY`.
  - Moving a branch there without a push: `git bundle`, `scp`, `git fetch` from the bundle, as
    go.dot does.
- `cmake`, `pluginval` and `clap-validator` are not on `PATH` here; WFS-DIY's plugin build uses
  the Visual Studio generator through `Plugin/Scripts/build-all.ps1`.
