# OSCQuery Automation Plugin — Development Plan (suggestion for Claude Code)

**Draft 0.1**, 2026-11-04, derived from the PRD of the same date. A proposed phase order, not a
schedule. Each phase ends with something runnable in a DAW and a test that stays in CI. Phases
will be broken into subphases as they go.

**Decided 2026-11-04, by the author:** C++20 on JUCE 9 for the plugin and the client library;
**GPLv3** for both, replacing the PRD's MIT. Rust was considered and set aside: it removes JUCE's
AGPL from the plugin but not the VST3 SDK's GPL, and nih-plug's static parameter model does not
fit a plugin whose whole point is renaming parameters after bind. The PRD's licence lines are
amended. **Also decided the same day: the Core profile adds no field to `HOST_INFO`.** OSCQuery
is itself a proposal that needs support, not a rewrite. No UUID, no `TIMETAGS`. Device identity
is `NAME`, the tree hash, and an optional identity leaf the user designates in the device's own
tree; timetag support is a per-device setting. The PRD is amended accordingly.

**Decided 2026-11-10, by the author:** beside the profile, which still adds no field, two
**optional extensions**, `DEVICE_ID` and `CLOCK`, drafted by Julien Bayle in
`docs/spec/extensions-draft-0.1.md`. Declared in `EXTENSIONS`, ignorable, additive; the plugin
uses them when a device declares them and serves them itself (Phase 6x). **Pull requests from the
same day**, Julien Bayle's first having been merged.

---

## How to read this

- **Read the PRD first.** Its *Goals and non-goals* and *Binding model* sections are the review
  criterion for every PR. A `CLAUDE.md` holding those two sections verbatim is Phase 0 work.
- **Core before shims, library before core.** Nothing reaches a plugin editor before the core
  exposes it and a headless test drives it. The editor is a client of the core; the three format
  shims are thin.
- **Anything the PRD lists under *Open questions* is not decided by the implementer.** Surface
  it, propose, wait. The ones that block a phase are repeated under that phase's *Needs from the
  author*.
- **Spikes are throwaway.** They live in `spikes/`, never migrate into `src/`, and each ends with
  a written pass/fail in `docs/spikes/`.
- **Small PRs per subphase.** One concern per PR; the test fixture is part of the PR.
- **Nothing is typed by hand that the device can tell us.** If an implementation step needs an
  OSC address written into code or into a test, the step is wrong or the test is a fixture of a
  device tree, never a shortcut.

Sizes are relative: **S** days, **M** weeks, **L** several weeks. No dates.

---

## Facts checked on 2026-11-04 that the PRD left open

These change the build, not the design. Each is re-checked at the phase that depends on it.

| Fact | Where checked | Consequence |
| --- | --- | --- |
| JUCE 9.0.5 is the version WFS-DIY and go.dot build on; its `juce_audio_plugin_client` has AAX, AU, LV2, Standalone, Unity and VST3 targets and **no CLAP** | `ThirdParty/JUCE` in both repos | The PRD's "native JUCE CLAP if it has landed" branch is closed for now. Phase 1 chooses between the two wrappers below. |
| `clap-juce-extensions` documents JUCE 6, 7 and 8; JUCE 9 is not mentioned. It exposes `clap_juce_audio_processor_capabilities` and `clap_juce_parameter_capabilities` but its README does not say whether hidden flags, info rescans or `clap_host_params->clear()` are reachable | github.com/free-audio/clap-juce-extensions; a JUCE forum post says the 8.0.11 `juce_audio_processors_headless` split broke it | Not in the build (decided 2026-11-04). Phase 1 spends one day seeing whether it compiles on 9.0.5, as information only; its source is read in Phase 5b for how a JUCE editor is embedded in a CLAP window. |
| `clap-wrapper` turns a native CLAP into VST3, AUv2, AUv3 and a Standalone; AAX is dual-licensed (GPL-3 or Avid's agreement) | github.com/free-audio/clap-wrapper | Not in the build (decided 2026-11-04). VST3 and AU come from JUCE's own wrappers; CLAP from an in-house shim in Phase 5b. |
| Upstream JUCE, checked 2026-11-04: no file, commit, branch or changelog entry mentions CLAP on `develop`; the Q3 2025 roadmap promised it for JUCE 9, which has shipped through 9.0.5 without it | github.com/juce-framework/JUCE; juce.com roadmap posts | Nothing waits on JUCE. If native CLAP lands later it is a third way to build the same shim interface, not a design change. |
| WFS-DIY's `HOST_INFO` emits `NAME`, `OSC_PORT`, `OSC_TRANSPORT` (UDP), `WS_PORT` and `EXTENSIONS` for ACCESS, VALUE, RANGE, DESCRIPTION, TYPE, CLIPMODE, LISTEN. No `OSC_IP`, no mDNS announcement, `PATH_CHANGED` only (no `PATH_REMOVED`/`PATH_RENAMED`), `ACCESS` is global, not per client | `Source/Network/OSCQueryServer.cpp` in WFS-DIY | WFS-DIY as the first conformance subject fails C3, C6 and L2 today. Companion work in WFS-DIY is listed per phase below. |
| go.dot already has multicast DNS written in-house (`src/wfg/engine/oscquery/Mdns.{h,cpp}`, `Discovery.{h,cpp}`): a codec, a browser on every interface sharing port 5353 with the system responder, and an announcer | go.dot repo, built 2026-10-17 | The client library's DNS-SD comes from there, not from a new implementation or a platform SDK. |
| WFS-DIY's plugin suite has `OscQueryClient` (HTTP GET, websocket LISTEN/IGNORE, `PATH_CHANGED`), `OscTransport` (UDP send incl. three-float messages), `RateLimiter`, and a *Bridge* pattern: a shared dynamic library loaded by every plugin binary with a versioned ABI | `Plugin/Source/Shared` in WFS-DIY | The library's HTTP/websocket layer starts from `OscQueryClient`; the process-wide registry starts from the Bridge pattern. |
| WFS-DIY and go.dot are **GPL-3**; the PRD put the plugin and the library under **MIT**; JUCE 8+ is AGPLv3 or commercial, the VST3 SDK GPLv3 or Steinberg's agreement | both `LICENSE` files; juce.com; steinberg.net | An open-source binary is GPL in practice whatever the source licence says. Decided 2026-11-04: **GPLv3 for plugin and library**, so files from WFS-DIY and go.dot are copied as they are, JUCE types included. The library is not bundleable in closed firmware; manufacturers use the conformance CLI. |
| WFS-DIY's plugin CI builds on windows-2025 (VS 2026), macOS and Linux and uploads installers; signing and notarisation on release only | `.github/workflows/plugins-ci.yml` in WFS-DIY | Phase 0 copies the pattern. |
| WFS-DIY **does not honour OSC bundle timetags**: its parser reads the eight bytes and drops them; bundles apply on arrival from a 16 ms drain on its message thread. Its outgoing bundles carry timetag 1 (checked 2026-11-08) | `spatcore/control/osc/OSCParser.h`, `OSCIngestQueue.h`, `OSCManager::handleIncomingBundle` in WFS-DIY | The PRD's "WFS-DIY does" is stale and amended. Phase 3 builds the timetag path against the mock device only; no bench device verifies scheduling yet. |
| WFS-DIY's OSC router accepts the OSCQuery short form `/wfs/input/<n>/<name>`, the channel in the address, beside its standard form with the channel as the first argument (checked 2026-11-08) | `Source/Network/OSCMessageRouter.cpp`, `GoDotProtocol.h` in WFS-DIY | The tree's `FULL_PATH` is sendable as is, with `TYPE`'s tags. **Confirmed live 2026-11-08** against the running WFS-DIY: a float sent to a per-input leaf's `FULL_PATH` over UDP 8000, no address typed, read back over HTTP with the new value. WFS-DIY's own feedback goes to UDP 9000 on the client (the author, same day; Phase 4's UDP receiver). |

---

## Where code comes from

| Piece | Origin | What changes |
| --- | --- | --- |
| HTTP GET, websocket, LISTEN/IGNORE, notifications | go.dot `src/wfg/engine/oscquery/OscQueryClient::get` (the raw-socket GET, chunked bodies); WFS-DIY `Plugin/Source/Shared/OscQueryClient` as the behavioural reference only (amended 2026-11-08: it has no licence header, parses no `HOST_INFO` and decodes one message; nothing copied) | A typed tree model, `HOST_INFO` parsing with `EXTENSIONS`, subtree GET, all four `PATH_*`, reconnect with backoff: `lib/oscquery/oscquery/Device` |
| DNS-SD browse and announce | go.dot `src/wfg/engine/oscquery/Mdns`, `Discovery` | Extracted with no dependency on go.dot's engine; the announcer serves the plugin's own server in Phase 6 |
| OSC encode/decode, UDP send and receive | go.dot `src/wfg/engine/osc/OscValue`, `OscAddress`, `OscCodec`, `UdpEndpoint`, `SocketBuffers` (decided by the author 2026-11-08, replacing WFS-DIY `OscTransport` plus `juce_osc`: JUCE's byte-level OSC streams are private to its `.cpp` files and `juce::OSCArgument` has no bool, int64, double or time tag) | Adds bundles under the MTU with one timetag (`osc/Bundler`), SLIP and length-prefix framing (`osc/Framing`), a TCP sender (`net/TcpSender`); the non-blocking `sendto` path waits for Phase 3 |
| Rate limiter | WFS-DIY `RateLimiter`, its shape only (latest value per key, compared with what was last sent) | Per bound path, default off, driven by the network thread's clock: no mutex, no timer (`src/core/send/RateLimiter`, 2026-11-08) |
| Realtime-safe encoder and UDP sender | Written here, 2026-11-08: neither repository had a send path a realtime thread may call (go.dot's `UdpEndpoint::send` builds a `juce::String` and may resolve the host; `osc::encode` returns a vector) | `osc/FixedEncoder` (a message or a bundle into a caller's buffer, byte for byte the codec's) and `net/RawUdpSender` (resolved once, non-blocking `sendto`, a full buffer a counted drop) in the library |
| Shared library across plugin binaries | WFS-DIY Bridge (`BridgeApi.h`, `BridgeLoader`), the ABI pattern only: exact-match version, found by walking up from the plugin's own module | One exported table (`registry/RegistryApi.h`) over the registry, in `OSCQuery-Automation-Core`; every binary also links the core and falls back to it, saying so, where WFS-DIY's plugins go silent (2026-11-08) |
| Websocket/HTTP transport | `juce_simpleweb` (vendored in both repos) | Unchanged; TLS compiled out as in WFS-DIY |
| Installer scripts, CI | WFS-DIY `Plugin/Scripts`, `Plugin/Installer`, `plugins-ci.yml` | Renamed; adds a CLAP install path and pluginval |

Nothing is taken from ossia, libossia or any other OSCQuery implementation. The library is the
reference client; it has to be independently written to be worth anything as one. With both
source repos and this one under GPLv3, the copies above need no relicensing; the headers keep
their author and gain an origin line.

---

## Phase 0 — Core profile draft, repo scaffold · M

**Goal:** a 4-page spec other people can review, and a repo that builds an empty plugin on three
platforms.

- **Core profile draft** in `docs/spec/core-profile-draft-0.1.md`: `HOST_INFO` fields as the
  OSCQuery proposal defines them and nothing more, `NAME` stable across reboot and equal to the
  DNS-SD instance name, path stability, `TYPE`/`RANGE` on every bindable leaf, `ACCESS` as
  reported to this client, subtree GET. Each clause maps to one check in the PRD's
  interoperability matrix (C1–C7); the Live profile (L1–L5) is a second, shorter section. The
  draft says in one paragraph why it adds no field: a profile of the proposal is easier to adopt
  than an amendment to it.
- Repo scaffold: CMake, submodules pinned (JUCE 9.0.5 at the same commit as WFS-DIY,
  `juce_simpleweb`, `clap` and `clap-helpers` for Phase 5b), GPLv3 file
  headers in go.dot's form reading `Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle`
  (decided 2026-11-04; Julien Bayle is a contributor to the repository), a
  `THIRD_PARTY_NOTICES.md` from the first dependency,
  `docs/` holding the PRD, this plan and the spec, `CLAUDE.md` = PRD *Goals and non-goals* +
  *Binding model* verbatim.
- Layout fixed now so Phase 2 and 3 do not argue about it:

  ```
  lib/oscquery/     the client library, its own CMake target and tests, no JUCE GUI dependency
  src/core/         slots, registry, state chunk, scaling, send and record paths
  src/shims/        juce/ (one AudioProcessor: VST3 and AU), clap/ — thin
  src/editor/       browser, slot table, status bar, Monitor
  tools/conform/    the conformance CLI (Phase 8)
  tests/            unit (library, core), host-less plugin tests, a mock OSCQuery device
  spikes/           throwaway
  ```

- CI on GitHub Actions for Linux / macOS / Windows copied from WFS-DIY's `plugins-ci.yml`:
  build, run tests, upload unsigned artefacts. pluginval added as soon as there is a VST3 to feed it.
- A **mock OSCQuery device** in `tests/mock-device/`: a tiny server (reusing `juce_simpleweb`)
  that serves a tree from a JSON fixture, answers `HOST_INFO`, accepts LISTEN and can be told to
  rename, remove or retype a path mid-test. Every later phase's tests drive it; the conformance CLI
  is first pointed at it.

**Done when:** CI is green on all three platforms building a VST3 that does nothing; the spec draft
has gone out for review; the mock device serves a fixture tree and a test reads it back.

**Needs from the author:**

- Nothing blocking. Copyright holders are decided (Pierre-Olivier Boulant, Julien Bayle). Files
  copied from WFS-DIY and go.dot keep their original header and gain a one-line origin note.
- The two reviewers for the spec (Holophonix, one ossia maintainer).

**Record, 2026-11-04.** Built in four commits on `main`: the spec draft (`docs/spec/`), the
scaffold, CI, the mock device.

- *Decided by the author the same day:* the spec text is CC BY 4.0, the rest GPLv3; the plugin's
  identity is Pix et Bel, `PxBl`, plugin codes `Q008`…`Q128` and `QMon` (fixed for good, see
  `src/shims/juce/CMakeLists.txt`); the shims are `src/shims/juce/` and `clap/` (layout above
  amended).
- *Pins:* JUCE at WFS-DIY's `d60b7a78` (the 9.0.5 tag; go.dot's `7ff644f6` is 9.0.4 plus 26);
  `juce_simpleweb` from go.dot's fork pin `b72ec947`, because WFS-DIY's vendored copy cannot report
  a port-0 bind, which a mock device on an ephemeral port needs; doctest 2.5.3 as its own
  submodule (go.dot borrows Tracktion's); CLAP 1.2.10 and clap-helpers `a380c978`, not built.
  `scripts/check-pins.py` holds `THIRD_PARTY_NOTICES.md` to the gitlinks.
- *Fact corrected:* the VST3 SDK bundled in JUCE 9.0.5 is **MIT** (Steinberg, 2025, its
  `LICENSE.txt`), not GPLv3-or-agreement as the facts table above and the PRD's licence reasoning
  say. It changes nothing decided (JUCE's AGPLv3 still makes the binary GPL in practice) but the
  PRD's sentence is stale.
- *The proposal* (Vidvox README at `e3d8e3b9`) reads two ways in eight places, listed in §6 of
  the spec draft with how a profile client reads each. The draft's *(proposed)* clauses are the
  reviewers' questions.
- *Done-when:* the spec is written and waits for the author to send it to the reviewers; the
  mock device serves `generic-device.json` and `MockDeviceTests.cpp` reads every node back,
  under C and fr_FR; CI builds the do-nothing VST3 on three platforms and pluginval passes it at
  strictness 5.
- *On the farm, the same day, at the mock-device commit:* the Mac mini (macOS 15.8.2, arm64,
  Xcode 26.3) and the NUC (Ubuntu 24.04, GCC 13.3) configure `dev`, build with no warning in our
  code, pass 11 cases and 230 assertions under C and fr_FR, build the Release VST3 (and the AU on
  the Mac) and pass pluginval 1.0.4 at strictness 5 (the NUC headless, `--skip-gui-tests`). The
  check is one script, `scripts/farm-check.sh`, run over SSH on a `git bundle` clone. GitHub warns that
  `ubuntu-latest` moves to Ubuntu 26 from 2026-10-19; CI follows it unless a job breaks.

---

## Phase 1 — Bench the formats shipped first · S

**Goal:** the rename behaviour of VST3 and AU measured in the hosts that matter, before core
code depends on it.

**Decided 2026-11-04:** VST3 and AU first, through JUCE's own wrappers; CLAP later, through a
shim of our own written against the CLAP C API (Phase 5b). Neither `clap-juce-extensions` nor
`clap-wrapper` is in the build. JUCE 9.0.5 has no native CLAP and the announcement has no date,
so nothing waits on it; if it lands, it is a third way to build the same shim interface.

- A throwaway `juce_add_plugin` in `spikes/`, 8 slots with the PRD's parameter model: normalised
  float, stable ID, name changed at runtime through `updateHostDisplay` with
  `parameterInfoChanged`, `(unbound)` names, AU `IsHidden` and `clump`.
- The matrix, filled per host and written into `docs/spikes/phase1-formats.md`:

| Check | Where |
| --- | --- |
| Rename a slot while active; the automation lane name follows | Reaper, Samplitude, Live, Bitwig, Nuendo (VST3); Logic (AU) |
| Hide an unbound slot; it leaves the host's parameter list | Logic (AU) |
| A stale lane after rebind: what the host shows, what the user has to do | all six |
| pluginval at strictness 5 on the VST3, `auval` on the AU | CI, and the Mac mini |

Reaper, Samplitude and Live are on the author's Windows machine; the Mac mini on the farm
covers AU. Bitwig and Nuendo rows are filled when a licence is at hand, and the report says
which rows are still empty.

- **One day, information only:** clone `clap-juce-extensions` into `spikes/` and try to build it
  on JUCE 9.0.5. The result goes in the report. It is not a dependency either way.
- **Registry location, settled by consequence:** with JUCE each plugin ID is its own binary, so
  five slot-count variants are five binaries per format, and VST3 cannot hide slots, so one
  binary with hidden slots is not an option there. The process-wide registry therefore lives in
  a shared library (WFS-DIY's Bridge pattern), built in Phase 3. The variants are five plugin
  IDs, as the PRD leant.

**Done when:** the report exists, the five-ID decision is written into the PRD, and the spike is
deleted from `spikes/`.

**Needs from the author:** access to the Mac mini; Bitwig and Nuendo licences, or an agreed
empty row for each until one turns up.

**Record, 2026-11-08.** Report: [`docs/spikes/phase1-formats.md`](spikes/phase1-formats.md);
the spike is deleted (its code is in the history at `aaac2c4`).

- *VST3 rename works in every host but REAPER:* Samplitude 2026 (build 26215), Live 12.4.16 and
  Bitwig 6.1.4 follow on lanes already drawn, during playback too, on both machines where
  installed. REAPER renames the parameter but not a lane already drawn, until reopen or FX
  offline/online.
- *No host loses automation through bind, unbind or rebind, unless slots are hidden.* A rebound
  slot's curve drives the new target; in REAPER under the old title.
- *AU cannot hide without cost:* AUv2 has no `IsHidden` flag; leaving the slot out of the
  parameter list (tried through a JUCE patch in the spike) deletes its automation in REAPER and
  keeps it out of GarageBand. Decided by the author the same day: AU slots are never hidden and
  are named `(unbound)` as in VST3; the PRD's Binding model rule and format table are amended.
- *Mac bundles must be ad-hoc signed as a whole* for the AU registrar to see them; JUCE's CMake
  under Ninja does not do it.
- *Done-when:* the report exists, the five-ID decision is in the PRD (2026-11-04), the spike is
  deleted. Bitwig was installed on both machines on 2026-11-04; Nuendo and Logic rows stay empty;
  clap-juce-extensions builds on JUCE 9.0.5 (information only).

---

## Phase 2 — The client library · M

**Goal:** `lib/oscquery` reads any OSCQuery server, headless, under test, with no plugin code
above it.

- **Tree model**: node with `FULL_PATH`, `TYPE`, `RANGE`, `UNIT`, `ACCESS`, `DESCRIPTION`,
  `CLIPMODE`, `CRITICAL`, `VALUE`, children; multi-argument types split into per-argument views;
  a *bindable* predicate per the PRD (`TYPE` present, `ACCESS` includes write for send, read for
  record, containers never). A `treeHash()` over bindable leaves for Phase 5.
- **HTTP**: `GET /?HOST_INFO`, `GET /`, `GET /<path>` for subtrees, lazy loading above the node
  threshold (default 5000). Timeouts and a reachability probe that yields *unreachable* rather
  than an exception.
- **Websocket**: LISTEN / IGNORE, value stream, `PATH_CHANGED`, `PATH_ADDED`, `PATH_REMOVED`,
  `PATH_RENAMED`, OSC over websocket for `CRITICAL`; reconnect with backoff, re-LISTEN on
  reconnect. Features gated by `EXTENSIONS`, never assumed.
- **DNS-SD**: browse `_oscjson._tcp` on all interfaces (from go.dot), an announcer for Phase 6,
  a manual host:port entry path that goes through the same probe.
- **OSC**: messages, bundles with timetags, MTU-aware splitting, UDP and SLIP-over-TCP senders,
  a UDP receiver for the Phase 4 fallback.
- **Recents store**: devices keyed by `NAME` plus last host:port in the per-user settings
  location, with the standard JUCE `PropertiesFile` paths; the identity leaf's path and value
  kept alongside when set.
- Tests against the mock device: full tree, lazy subtree, every notification, a device that
  answers `HOST_INFO` and nothing else, a device without websocket, a 10,000-leaf tree in under
  2 s (L5, measured on loopback). Serialisation tests run under `fr_FR` as well as `C` because
  `RANGE` and `VALUE` are decimals.

**Done when:** a command-line example in `lib/oscquery/examples/` prints WFS-DIY's tree, subscribes
to a path and prints its changes, with no address typed; the test suite passes on three platforms.

**Needs from the author:** nothing blocking. The library's public name, if it is to be its own
repo later (Phase 8 splits it).

**Companion in WFS-DIY:** none yet; the library must cope with WFS-DIY exactly as it is.

**Record, 2026-11-08.** Built in `lib/oscquery/` in two commits on `main` after the target's
first (2a: the target and go.dot's OSC codec; then the rest of the library; then the example and
the records). Report of the bench: [`docs/bench/2026-11-08-phase2-library.md`](bench/2026-11-08-phase2-library.md).

- *Decided by the author the same day:* the library's **working name** is `oqa::oscquery`
  (namespace, target, `<oscquery/...>` includes), renamed when Phase 8 splits it out; **OSC comes
  from go.dot**, not from WFS-DIY's `OscTransport` plus `juce_osc` (the table above is amended);
  the **TCP sender is built now, framing selectable, SLIP the default**, the PRD having decided
  SLIP for the plugin while the spec draft still marks it *(proposed)*.
- *Implementer's decisions, the author's to overrule:* `juce::JSON` stays the tree's JSON (go.dot's
  own parser exists for shortest-round-trip doubles and >int64 literals, which a device tree has
  no need of; `tests/LocaleTests.cpp` proves the number text locale-independent). A notification
  that *arrives* is handled whether or not `EXTENSIONS` lists it, because WFS-DIY sends
  `PATH_CHANGED` without listing it; nothing is ever *sent* that is unlisted. `treeHash()` runs over
  every leaf with a `TYPE`, whatever its `ACCESS`, so two clients a device treats differently (C6)
  agree; the algorithm is C5's *(proposed)* one and changes if the reviewers change it. Every
  callback but `onState` fires on the Device's own thread; `onState` fires on the thread that
  changed the state, the caller's for the blocking calls. No `juce::Timer`, no `MessageManager`.
- *Lazy loading, as OSCQuery allows it:* the proposal has no depth-limit field and `GET /` is the
  whole tree, so a tree above 5,000 nodes (`Device::Options::largeTreeThreshold`) is fetched once
  and never whole again, every later change arriving by subtree GET of the path a notification
  names; containers a device sends without `CONTENTS` are `loaded = false` until fetched. The
  PRD's "device advertises a depth limit" has no field to read.
- *Found on the way:* `juce::var` answers `isObject()` for an array too, and the `DynamicObject`
  behind an array is null (a crash on `EXTENSIONS` given as a list, caught by the suite). WFS-DIY's
  `OscQueryClient` decodes the first argument of `,fff` from the wrong offset (type-tag block
  assumed four bytes); not copied. juce_simpleweb's client closes an idle connection after 1000 s
  on its own; the reconnect path covers it.
- *The mock device gained:* `hostInfoOnly`, `notifyUnlisted`, `oscTransport` (a TCP listener behind
  `OSC_PORT`), `oscIp`, a reader behind `OSC_PORT` (datagrams, or a TCP stream's bytes), binary
  websocket frames kept, `setValueBundle`, `closeAllConnections`; the runner the matching flags and
  `--generate N` from `tests/support/TreeGen.h`. Still missing: a distinct `WS_PORT`, per-client
  `ACCESS` (Phase 5).
- *Done-when:* `oqa_oscquery_tests`, **90 cases, 31,830 assertions under C and fr-FR on Windows**,
  as `oscquery.C` and `oscquery.fr_FR` in CTest; **CI green on Linux, macOS and Windows** (run
  42509175235) after two portability fixes GCC and clang asked for (a nested struct's default
  member initialisers in a default argument; floating-point `std::to_chars` absent below macOS
  13.3, this plugin's floor being 11.0). The example prints **WFS-DIY's tree (1,393 nodes, 1,347 leaves)**, go.dot's (820
  nodes) and the mock's, and subscribed to a WFS-DIY input position with no address typed in code
  it printed the current value and then three changes sent over UDP as they landed (WFS-DIY's
  OSCQuery server is off by default, `networkOscQueryEnabled`; the author switched it on in its
  Network tab). L5 on loopback: 10,000 leaves fetched and parsed in 1.4 s, Debug.
  DNS-SD browse **finds an instance announced from the Mac mini** (Bonjour's `dns-sd -R`) across
  the home network, probes it and lists it down with the reason; it found nothing announced on
  this same Windows machine (four processes already on 5353), a same-host matter kept in the
  bench report.

---

## Phase 3 — Slots, registry, send · L

**Goal:** WFS-DIY driven from Reaper with no typed address. PRD milestone 2.

- **Slot model** (`src/core`): the PRD's table as a struct; host 0–1 ↔ device units with linear,
  log and exponential scaling; `RANGE` missing → slot inactive until min/max entered; `T`/`F` as a
  toggle; `i` with rounding; per-argument slots for `fff` grouped into one message on send using
  the last known value for arguments not being automated.
- **State chunk**: the PRD's JSON, `version` from the first commit, a loader that reads every
  past version (fixtures kept in `tests/state/`), unknown fields preserved on round-trip.
- **Device registry**: process-wide, one entry per resolved device (`NAME` plus identity leaf
  value, resolved to one host:port), one network thread per device at
  realtime priority, a lock-free queue from each instance's audio thread. Instances register in
  `prepareToPlay` (or the CLAP `activate`) and again whenever the host restarts the process
  (Logic's `AUHostingService` does this; WFS-DIY learnt it the hard way). (Amended 2026-11-09 while
  planning Phase 6: an instance registers at construction and stays registered for the plugin's life,
  marked inactive while the host has stopped its audio, so the plugin's own server and the Monitor
  keep stable paths and rows; `connect()` in `prepareToPlay`/`activate` is still called and still safe
  to repeat.) The registry lives in
  a **shared core library** loaded by every plugin binary, after WFS-DIY's Bridge: versioned ABI,
  exact-match version check, installed next to the plugins. Every variant, and later the CLAP
  shim, links the same library, so one process has one registry whatever mix of binaries it
  loads.
- **Send path**: one bundle per device per audio block holding every changed slot from every
  instance, split at the MTU, immediate timetag unless the device's *honours timetags* setting is
  on, in which case timetag = intended playback time plus the per-device offset. Per-slot rate limiter,
  default off. TCP when `OSC_TRANSPORT` says so or the node is `CRITICAL`. The optional direct
  `sendto` from the audio thread, default off, behind a setting with a warning in its tooltip.
- **Shims**: VST3 and AU through one JUCE `AudioProcessor` subclass, best-effort naming
  (`kParamTitlesChanged`, `(unbound)` names; on AU `(unbound)` names too, never hidden, decided
  2026-11-08, and `clump`), AU measured in Logic
  in Phase 7. The shim interface is written for CLAP's capabilities (rename, hide, clear, module)
  and the JUCE shim answers *unsupported* where a format must; the CLAP shim arrives in Phase 5b
  and no core code moves for it.
- **Editor, first cut**: device list (found, recent, manual), tree with search, slot table with
  click-slot-then-click-leaf binding, inline range and scaling, the *Test* button. Enough to bind;
  not yet the full UX.
- **Audio thread lipogram** test as in go.dot: no allocation, locks, exceptions, syscalls or
  logging in the process callback; instrument and assert.
- Measurement: time from start of block to packet on the wire on loopback, logged by a test
  harness, target under 1 ms as the PRD states.

**Done when:** in Reaper on Windows and macOS, a user opens the plugin, picks WFS-DIY from the
found list, binds x/y/z of one input to three slots, draws curves, and the source moves; 32 tracks
doing the same produce one bundle per block on the wire (verified by capture); the session reloads
with its bindings.

**Needs from the author:** the default slot count to ship first (this plan says 32 alone until
Phase 7 adds the other variants); whether `$instance` lands here or in Phase 6 (plan says 6).

**Companion in WFS-DIY:** `OSC_IP` in `HOST_INFO` (cheap, and the library otherwise falls back to
the HTTP host). Nothing else.

**Planned, 2026-11-08.** Broken into six subphases, each one commit on `main` with its tests, as
Phase 2 was: **3a** slot model and scaling (`src/core`, target `oqa::core`, tests `core.C` and
`core.fr_FR`); **3b** state chunk, `version` 1, fixtures in `tests/state/`; **3c** registry and
send path (the C ABI `RegistryApi.h`, the shared core library, the per-device realtime network
thread, the ring, the block bundler, the rate limiter, timetags, a realtime-safe UDP sender and a
fixed-buffer encoder added to the library, the lipogram test, the latency harness); **3d** the JUCE
shim's real parameters, state and registration; **3e** the editor's first cut; **3f** the bench in
Reaper on Windows and the Mac mini, the records, the amendments.

- *Decided by the author the same day:* **32 slots alone** through Phase 3; **`$instance` in
  Phase 6**; the **timetag path is built and verified on the mock only**, the per-device setting
  default off; the audio thread wakes the network thread with **one non-blocking notify per block
  per device**, the one named exception to the lipogram's "no syscalls", recorded as such.
- *Facts found while planning, from the sibling repositories:* **WFS-DIY discards bundle
  timetags** (`spatcore/control/osc/OSCParser.h` reads the eight bytes and drops them; bundles
  apply on arrival from a 16 ms drain on its message thread), so the PRD's "WFS-DIY does" is
  amended: no device on the bench honours them yet. WFS-DIY's **Bridge is a registry only**: no
  lock-free hand-off (mutexes, allocation and UDP sends on the thread that changed the parameter),
  so its ABI pattern is copied and its threading is not. **go.dot's lipogram counts allocations**
  through a replaced global `operator new`, test-only here (a replaced allocator inside a plugin
  could leak into the host); locks and syscalls fall to a Clang RTSan job when the runner has
  Clang 20. **No realtime-safe send exists** in either repository: `UdpEndpoint::send` builds a
  `juce::String` and may resolve the host, `osc::encode` returns a vector; both are written new
  in 3c. WFS-DIY's OSC router **accepts the tree's short form** `/wfs/input/<n>/<name>` with the
  channel in the address (`OSCMessageRouter.cpp`), so `FULL_PATH` is sendable as is; confirmed on
  the bench first thing in 3f.
- *Implementer's decisions, the author's to overrule:* only the registry crosses the shared
  library's ABI (slots, scaling, chunk and editor are C++ in the plugin binary; structured data
  crosses as JSON text, the hot path as scalars); every plugin binary **also links the core
  statically and falls back to it** when the shared library is missing or its ABI version differs,
  saying so in the status bar, where WFS-DIY's plugins go silent; the shared library is built with
  hidden visibility and exports the C ABI only, so two copies of JUCE in one process do not meet;
  one value per slot per block, because JUCE's `AudioProcessor` delivers no sample-accurate
  parameter events (this sentence read "the CLAP shim will, Phase 5b"; amended 2026-11-09 while
  planning Phase 5b: the CLAP shim consumes the offsets and hands the core the block's last value
  too, the sample-accurate send waiting for a device that honours timetags); the device key is `NAME` plus the identity
  leaf's value, resolved to one host:port (corrected 2026-11-09 while planning Phase 5: this
  sentence read "`NAME` plus last host:port"; `Registry.cpp` has keyed by name and identity since
  3c), re-identification being Phase 5's.

**Record, 2026-11-08 (built; the DAW rows of the done-when open).** Built in eight commits on
`main`, 3a to 3e: `src/core` (`oqa::core`), the shared core library `OSCQuery-Automation-Core` and
its loader, the shim's real parameters, the editor's first cut. Report of the bench so far:
[`docs/bench/2026-11-08-phase3-send.md`](bench/2026-11-08-phase3-send.md).

- *Done so far, measured on this machine against the mock (Debug, loopback):* two instances, 25
  blocks, **25 bundles of two messages**; an `fff` one message with the tree's values for the
  arguments not automated; 32 messages at an MTU of 200 in four bundles; CRITICAL on a UDP device
  over the websocket; a TCP device's SLIP-framed bundles; the rate limiter holding to 20 Hz; the
  NTP tag at now plus the offset when the setting is on; an unreachable device sending nothing
  until pointed right, every instance following; **500 blocks allocating nothing** on the audio
  path, direct send on or off; **post to the wire median 96 µs, p99 227 µs**. The core's suite is
  **69 cases** as `core.C` and `core.fr_FR`; the library's 98. pluginval 1.0.4 passes at
  strictness 5 on the VST3 with the editor. **Live against WFS-DIY the same day:** the whole path,
  Instance to registry to bundle to UDP 8000, drove three per-input floats found in its tree and
  read them back exactly (20 bundles, 60 messages, none dropped).
- *Implementer's decisions, the author's to overrule:* the rate limit is per bound path, not per
  argument, since a multi-argument node is one message anyway; `critical` and `unit` travel on the
  slot and in the chunk, because bindings arrive before the device's tree does on a reload; a
  device entry lives as long as the registry (its sockets and thread are not worth closing on
  every `releaseResources`); a session device whose address a second chunk disagrees on keeps the
  first address until pointed (Phase 5 resolves identity); the direct send sends a plain message
  with no bundle and no time tag; the arguments not automated start from the tree's `VALUE` at
  bind time and are not yet refreshed when the tree reloads (Phase 4's LISTEN will keep them
  current; **done in 4a, 2026-11-09**: they follow what the device streams, and a plan made before
  the tree arrived is rebuilt when it lands); the AU gets no parameter clump (fixed at construction, it could not follow a rebind;
  REAPER would put the unit's name in lane titles). NTP seconds wrap at the end of era 0 (2036) as
  NTP's do.
- *On the Mac mini the same day* (`scripts/farm-check.sh`, Debug): every CTest entry passes, the
  Release VST3 and AU build, pluginval passes the VST3; 25 blocks from two instances as 25 bundles;
  post to the wire median 209 µs, p99 337 µs. It took two fixes to get there: a device thread that
  spun to its gather deadline overran macOS's time-constraint budget and was demoted until its wakes
  came late (the gather now waits on the semaphore, the budget declared is a fraction of the
  deadline); and libc++'s `counting_semaphore::try_acquire_for` polls with a growing back-off instead
  of waking on release, so on Apple the wake is a dispatch semaphore (`registry/Wake.h`). The loader
  holds the system's own handle (LoadLibrary, dlopen), never closed, with no JUCE object for the leak
  detector to count at exit; `Registry::shutdown()` (the API table's first additive entry) lets a
  test process end with no thread of ours alive.
- *Found on the way:* a CMake target that links `oqa::core` gets the client library's archive
  appended after `oqa_juce` on GNU ld, which has already passed the SHA-256 it needs: name
  `oqa::oscquery` directly; for the plugin, whose JUCE modules live in JUCE's shared-code archive
  that the format targets put first, the core's and the library's object files go into that archive. `Node::argument()` returns a view by value, so a reference into its
  `range` dangles. MSVC's `std::counting_semaphore::try_acquire_for` is `[[nodiscard]]`. No range
  in the generic fixture excludes zero, so the log curve is tested on a user-entered range.
- *Still to fill, the author at the DAW:* the done-when's REAPER rows on Windows (the Release VST3
  and the core DLL are installed in `%LOCALAPPDATA%\Programs\Common\VST3\`; the steps are in the
  bench report) and on the Mac mini, and the 32-track capture.

---

## Phase 4 — Record · M

**Goal:** controller moves recorded in Reaper and Bitwig. PRD milestone 3.

- LISTEN on each bound path whose direction includes receive; incoming values scaled to 0–1 and
  pushed to the host through the format's path: CLAP out-events with `PARAM_GESTURE_BEGIN` /
  `PARAM_VALUE` / `PARAM_GESTURE_END` and `request_flush` when not processing; VST3
  `beginEdit`/`performEdit`/`endEdit`; AU gesture events in Phase 7.
- **Write/Touch/Latch is the host's decision, not the plugin's.** The plugin's job is the
  gesture: open it on the first incoming value, close it after an idle timeout (default 200 ms,
  setting). A host in Read mode ignores the push; that is the Soundscape `Rx` behaviour without a
  plugin-side mode switch. The editor shows the incoming value either way.
- **Loop guard**: a value the plugin sent is ignored if echoed back within 50 ms (setting), per
  slot, with a counter in the status bar.
- Slots with direction *both*: send and receive on one slot without oscillating; the test drives
  the mock device to echo with a delay above and below the guard window.
- **Devices without LISTEN** (decided 2026-11-04; WebSockets are optional in the proposal, so
  HTTP-only devices are conformant): the library's UDP receiver is wired, one port per process,
  and the plugin shows that port and its own address in the editor and the Monitor. The user
  enters them in the device's own configuration, as ToscA and OSCar users do today. The editor
  says on such a device that record is waiting for the device to be told the port, and the
  status bar shows the first packet arriving. Nothing is added to the spec for it.
- Test: the mock device in HTTP-only mode is told the plugin's port by the test harness and sends
  values; the lane records exactly as it does over LISTEN.

**Done when:** a value moved in WFS-DIY, Chataigne, ossia score or go.dot writes a lane in
Reaper (and Bitwig once CLAP exists, Phase 5b) in Touch mode, the lane plays back to the
device, and the loop guard counter stays at zero during playback; a session with 32 receiving
slots shows no missed values at the device's update rate. The three applications are the record
sources, so no hardware controller is needed on the bench.

**Needs from the author:** the gesture idle timeout default. The reply port is decided (LISTEN
when declared, else a UDP port shown in the editor and configured on the device side).

**Companion in WFS-DIY:** none required; LISTEN exists.

**Planned, 2026-11-09.** Broken into five subphases, each one commit on `main` with its tests, as
Phase 3 was: **4a** the receive path in the registry (a Group for every bound slot, LISTEN on every
group path, incoming values scaled to 0–1 and handed to the instance through two additive entries
of the C ABI, the undriven arguments of an `fff` kept current, the loop guard and its counter);
**4b** the UDP receiver for devices without LISTEN (one port per process, a setting, routed by
sender address then path, the port and this host's addresses in the status); **4c** the shim's
gesture path (a pure `GestureTracker` under test, the push to the host on the message thread,
the idle timeout); **4d** the editor (received rates, the guard counter, the receive port shown
and editable, the "waiting to be told the port" state); **4e** the bench in REAPER on Windows and
the Mac mini against WFS-DIY, Chataigne, ossia score and go.dot as sources, the records, the
amendments.

- *Decided by the author the same day:* the **gesture idle timeout defaults to 200 ms**, a
  setting; the **UDP receive port defaults to 0 (ephemeral)**, a setting the editor shows and
  edits, 9000 being what the bench types for WFS-DIY; the **Phase 3 REAPER rows are filled in the
  same DAW sitting as 4e** and both records close then; **Bitwig over VST3 is an extra bench row**,
  not a gate, the CLAP gate staying Phase 5b.
- *Facts found while planning:* the library's `Device` callbacks fire on the Device's own worker
  thread, not on the registry's per-device network thread, so the receive path runs there and
  never on the audio thread; the registry today makes a Group for sending slots only; JUCE's VST3
  wrapper forwards `performEdit` on whichever thread calls `setValueNotifyingHost`, and VST3 wants
  the UI thread, so the push is marshalled to the message thread; WFS-DIY's OSC feedback goes to
  the client's UDP 9000 (the author, 2026-11-08); `UdpEndpoint`'s handler is fixed at `start()`.
- *Implementer's decisions, the author's to overrule:* LISTEN on every bound path, sending ones
  included, because the stream is also what keeps an `fff`'s undriven arguments current (the
  Phase 3 record's open item); the undriven arguments become atomics so the Device thread may
  write what the network thread reads; a host value equal to the last one received for that slot
  is not posted back (the `both` direction does not echo), the device-side loop guard staying as
  the safety net and the PRD's counter; the UDP receiver routes by the sender's address against
  each device's resolved OSC host, then by path, and delivers to every candidate still ambiguous,
  counting it; the two ABI entries are additive (no version bump), and an older library beside
  the plugin is refused by the loader's size check, the static core taking over and saying so;
  lock order is the registry's lock then a device entry's groups lock, never the reverse; the
  settings `receive_port`, `loop_guard_ms` and `gesture_timeout_ms` join the registry's
  process-wide JSON, their persistence and full UI staying Phase 6's Monitor.

**Record, 2026-11-09 (built; the DAW rows of the done-when open, Phase 3's with them).** Built in
four commits on `main`, 4a to 4d, in one session. Report of the bench so far:
[`docs/bench/2026-11-09-phase4-record.md`](bench/2026-11-09-phase4-record.md).

- *Done so far, measured on this machine against the mock (Debug, loopback):* every bound path
  LISTENed and IGNOREd with its last group; a streamed value scaled to 0–1 by its slot (linear, a
  toggle, a user-entered log range); an `fff`'s undriven arguments following the device; a plan
  made before the tree arrived rebuilt when it lands, whole; the loop guard dropping a 10 ms echo
  and passing a 150 ms one; the host handing a received value back sending nothing; **32 slots in
  both directions following the mock at 100 Hz for 2 s, 6,400 values, none missed, none echoed
  back**; a receiving slot without a range saying so; two instances on one path; the audio path
  allocating nothing with receiving slots; a saved chunk with receiving slots recording
  (`tests/state/v1-receive.json`); a device without LISTEN delivering over the registry's UDP port
  once told it, bundles included, two such devices told apart by path and a shared path going to
  both, counted, a device whose websocket streams having its UDP copies ignored and counted, the
  port setting moving the receiver; the gesture tracker on the test's clock. **pluginval 1.0.4
  passes the Debug VST3 at strictness 5 with the editor.** The core's suite is 87 cases as
  `core.C` and `core.fr_FR`.
- *Implementer's decisions, the author's to overrule:* **on LISTEN the device's current value is
  delivered once** (the library reads it over HTTP), so a receiving slot takes the device's value
  when bound or reloaded, inside a gesture; a host armed in Touch at that moment records a point.
  **UDP copies for a device whose websocket streams are ignored and counted**, LISTEN being its
  way, as the PRD's "fallback for devices without websocket" reads; so the WFS-DIY UDP row on the
  bench needs its websocket refused to the plugin, or stays the mock's. The receiver, once started,
  runs for the life of the process (a port is cheap; stopping it when the last receiving slot goes
  would make the shown port come and go). The `both` direction's host-echo skip compares within
  1e-6 of the last received value, because a host may round-trip the float through a double. The
  plan rebuild on tree arrival carries the audio thread's last posted values over, so nothing is
  re-sent for the rebuild alone. `Device::Callbacks::onPathRemoved` and `onPathRenamed` are not
  wired yet: Phase 5's stale slots.
- *Found on the way:* **a partly bound `fff` planned before the tree sent a short message** (the
  open item of the Phase 3 record was a bug, not only a staleness); **a Device thread must never
  need a lock its joiner holds**: the entry stopped its Device under the device lock while that
  thread, mid-delivery, waited for the registry's lock whose holder wanted the device lock; stopped
  outside now, in the destructor and on an address change. Windows lists multicast groups among a
  machine's addresses (the receive line filters them as Discovery does). The test runner prints a
  backtrace on an access violation on Windows now (`tests/TestMain.cpp`).
- *Still to fill, the author at the DAW:* the done-when's REAPER rows (the Release VST3 and the
  core DLL from `8d548a8` are installed in `%LOCALAPPDATA%\Programs\Common\VST3\`; the steps are
  in the bench report), with the Phase 3 rows in the same sitting; the Mac mini and the NUC through
  `scripts/farm-check.sh`. **CI green on Linux, macOS and Windows** on `26ad453` (run 42589237831).

---

## Phase 5 — Re-identification, stale slots, export · M

**Goal:** a session moved between two machines with different IPs. PRD milestone 4.

- Reopen sequence as the PRD states: last host:port and compare `NAME` (and the identity leaf's
  value when set); on mismatch or silence, DNS-SD by `NAME`, candidates told apart by identity
  leaf then `tree_hash`, else unresolved; each bound path verified for existence and `TYPE`;
  stale slots keep automation and stop sending; `tree_hash` mismatch shows a one-line notice. No
  automatic rebinding, ever.
- **Identity leaf**: in the device pane the user can pick any read-only leaf of the device's tree
  as its identity (a serial number, a hostname node, whatever the device already publishes). The
  plugin stores path and value and compares the value on reopen. Nothing is asked of the device
  beyond what it already serves.
- **The target is pointed at in one place** (decided 2026-11-04, in answer to "is this where a
  master plugin comes in"): the registry maps each device identity to its current host and port,
  process-wide, so slots never hold an address. The Monitor's device list, and the device pane of
  every instance, show each device the session refers to with name, identity value if set, last
  address and reachability; the user picks a found device or types an address, and every instance
  follows at once. Each instance writes the new address into its own chunk on the next save, and
  the recents store keeps it per user, so a sandboxed instance with its own registry finds it
  too. A device alone under its name is re-found over DNS-SD with no click; pointing by hand is
  the path for two devices sharing a name with no identity leaf to tell them apart, or for a
  network where multicast does not cross. This is what a master plugin would have done, without a
  track to delete, a load order, or IPC.
- Notifications over the websocket (`PATH_REMOVED`, `PATH_RENAMED`, `PATH_CHANGED`) mark slots
  stale live; the editor shows the old path next to the current tree with a rebind button.
- **Export/import** of bindings as standalone JSON addressed by `host_param_id`; a small converter
  script in `tools/` from a ToscA or OSCar mapping as the worked example of import.
- Failure states table from the PRD (unreachable, unauthorised, stale, range unknown, echo loop,
  dropped sends) all reachable and all driven by the mock device in tests.
- `ACCESS` read as reported to this client: a mock-device test where the same leaf answers
  `ACCESS: 1` to one client and `3` to another.

**Done when:** a session saved against the mock device on one address reopens against the same
device on another address with every slot live; two mock devices sharing a `NAME` are told apart
by their identity leaf; a session whose device is not found by DNS-SD reopens with it unresolved,
is pointed at the new address once in the Monitor, and every instance goes live; a renamed path shows stale and is rebound in one click; an exported JSON reimports into a fresh instance identically; all of it in CI with two
mock devices.

**Needs from the author:** nothing blocking.

**Companion in WFS-DIY:** `PATH_REMOVED` / `PATH_RENAMED` when a channel is deleted or moved
(L2); a read-only leaf carrying something unique per installation, if WFS-DIY has one to
publish, so it can serve as an identity leaf (optional, not a profile requirement). Per-client `ACCESS` (C6) if WFS-DIY's
client table is to be reflected; otherwise it stays a documented gap in its matrix row.

**Planned, 2026-11-09.** Broken into five subphases, each one commit on `main` with its tests, as
Phase 4 was: **5a** verification in the registry (every bound path checked against the tree for
existence and `TYPE` when a plan is built: `pending`, `ok`, `missing`, `retyped`, `renamed` with
where it went, `unauthorised`; stale slots in no group, their lanes untouched; `PATH_REMOVED` and
`PATH_RENAMED` wired to the rebuild, the renames remembered; the chunk's `tree_hash` against the
live tree, one warning line; `last_seen`; the mock's per-client `ACCESS`); **5b** re-identification
(HOST_INFO `NAME` and the identity leaf compared before anything is sent or LISTENed; a resolver
thread; the library's Finder moved into the registry, one browser per process; candidates by
`NAME`, told apart by identity leaf then `tree_hash`, one adopted, several or none left
`unresolved` with the candidates in the status; pointing through the Instance, every instance
following, the address written into each chunk on its next save; two additive ABI entries for the
editor's device list); **5c** export and import (the chunk itself is the file), the ToscA/OSCar
converter in `tools/`, and one test per row of the PRD's failure-states table; **5d** the editor
(found devices from the status, the session rows with identity, resolution and last seen, *Point*
and *Identity* on a device row, stale slots red with the old path and a one-click *Rebind*,
unauthorised amber, Export and Import); **5e** the bench against WFS-DIY and go.dot across this
machine and the Mac mini, the records, the amendments.

- *Decided by the author the same day:* **the Finder lives in the registry**, one browser per
  process, editors reading the found devices from the status; **a device pointed at by hand is used
  whatever `NAME` answers**, with a warning line, the chunk's name kept; **the export file is the
  state chunk itself**, no envelope and no second schema; **the converter's fixtures are written
  from the ToscA user manual and the OSCar forum examples**, swapped for real files when one turns
  up. Pointing in Phase 5 is any instance's device pane; the Monitor's list is Phase 6.
- *Facts found while planning:* the device key has been `NAME` plus the identity leaf's value since
  3c (the Phase 3 record said last host:port; corrected in place), but **nothing compares
  HOST_INFO's `NAME` with the chunk's**, so a wrong device at the last address is driven as the
  right one; **`tree_hash` is never filled** (the editor builds its `DeviceRef` from name, host and
  port), so every chunk saved so far holds `""`; a device still being probed **joins the session
  named `host:port`**; `applyBindings` never checks a bound path against the tree (a missing node
  gives empty tags and default values); `onPathRemoved`, `onPathRenamed` and `onError` are the
  three callbacks not wired, the library already patching its tree and dropping the subscriptions
  under a removed or renamed path; `setDeviceAddress` and its ABI entry exist with one test and no
  caller in the plugin, and `entryFor` ignores a later chunk's different address; one Finder per
  open editor today; `Device::readValue` reads one leaf over HTTP before the tree is loaded (what
  an identity read needs); the mock renames, removes, retypes and adds with the notifications,
  restarts on a fresh port keeping its tree, runs two at once, but has no per-client `ACCESS` and
  does not announce over DNS-SD (`FinderTests.cpp` has a loopback responder to move into
  `tests/support/`); `generic-device.json`'s `/system/serial` is the identity leaf the fixture was
  written for. The chunk needs no new field: `tree_hash`, `identity_path` and `identity_value`
  have been there since 3b; stale is derived, not stored. ToscA's mapping is XML, `<tosca prefix>`
  over `<parameter index name min max scaling>`, the address `prefix` plus `name`; OSCar's is
  `<oscar>` over `<parameter pattern min max default>` with `$1` in the pattern (user manual 4 and
  the IRCAM forum; no formal schema published).
- *Implementer's decisions, the author's to overrule:* stale is never written into the chunk, so a
  device repaired later un-stales the slot by itself; a stale slot joins no group (nothing sent,
  nothing LISTENed) and keeps its lane and its name in the host; an `unauthorised` slot still
  receives when its direction says so (the PRD's "bindable for record but not for send"); until a
  device is confirmed nothing is sent or LISTENed to it, and a `NAME` mismatch stops that Device
  and starts the search; the resolver is one normal-priority thread per registry that snapshots
  under the registry lock, does its HTTP outside it and re-takes the lock to apply, stopped first
  in `shutdown()`; the Finder starts on the first device needing a search or the first editor
  asking, and never stops before shutdown; candidates by `NAME` exclude the address that just
  failed, and a candidate's identity leaf or tree is read through a temporary `Device` with a
  3 s timeout; `tree_hash` is written at every bind and rebind from the tree the editor shows, so
  the notice clears once the user has acted; the mock's allow-list is by remote address and the C6
  test flips it within one process, loopback giving both clients one address; the converter writes
  a version-1 chunk with direction `both` and `host_param_id` = index − 1, and the imported paths
  then meet 5a's check, a path the device does not serve showing stale; the two ABI entries
  (`finderEnsure`, `finderAddManual`) are additive, version unchanged, an older library refused by
  the loader's size check as in Phase 4.

**Record, 2026-11-09 (built; the DAW rows of the done-when open, Phase 3's and 4's with them).**
Built in four commits on `main`, 5a to 5d, in one session. Report of the bench so far:
[`docs/bench/2026-11-09-phase5-reidentify.md`](bench/2026-11-09-phase5-reidentify.md).

- *Done so far, measured on this machine against the mock (Debug, loopback):* a path renamed,
  removed or retyped shows stale with its detail (and where a renamed one went), sends nothing, is
  LISTENed no more, keeps its binding, and a path grown back is live again; a chunk reopened
  against a tree missing one path has that slot stale and the rest live; the `tree_hash` notice,
  cleared by a rebind's hash; ACCESS as reported to this client (two mocks, one allow-listing this
  client) makes a sending slot unauthorised that still receives; **a session saved against a mock
  on one address reopens against it on another, found by NAME over DNS-SD, every slot live, the
  new address saved**; **two mocks sharing a NAME are told apart by the identity leaf, and by the
  tree hash**; **with neither, unresolved with both as candidates until pointed once from one
  instance, every instance following and saving the address**; a different device at the last
  address is a mismatch never driven; a device pointed at by hand is used whatever NAME answers,
  with a warning; export and import byte-identical under `C` and `fr_FR`; the converter's output
  from the ToscA and OSCar fixtures loads, and against a device that does not serve its paths
  shows every slot stale; one case per row of the PRD's failure-states table. **pluginval 1.0.4
  passes the Debug VST3 at strictness 5 with the editor.** The core's suite is **110 cases** as
  `core.C` and `core.fr_FR`.
- *Implementer's decisions, the author's to overrule:* a `pending` slot (no tree yet, or the device
  not confirmed) is planned as before and re-checked when the tree lands, so Phase 3's and 4's
  behaviour is unchanged for a confirmed device; `T` and `F` count as one type for the retype check;
  a path under a container the device sent without CONTENTS is `pending`, never `missing`; the
  renames an entry remembers are forgotten when the tree holds the old path again; a device that
  stops answering while confirmed goes `searching` (its own Device keeps retrying the address, and
  comes back `confirmed` if it answers again); candidates are kept in the status after an adoption,
  as the record of the choice; the mock's HOST_INFO `NAME` is settable at run time and the test rig
  names the mock after the session's device, so every earlier test runs under the NAME compare; a
  device joins the session from the editor only once it has answered HOST_INFO; the editor's
  warnings line reads the registry's status, not the instance's last push; the editor's own word
  (a refused import) stands in that line until the next action; the `inactive` state joins the
  status's `slots` so the table is complete.
- *Found on the way:* **a path LISTENed as the websocket opens had its first value read twice**
  (`onOpened` and the `listenNow` queued behind it), caught by the 100 Hz receive case once LISTEN
  moved to after the device's confirmation; the library's worker now keeps the paths on the wire
  since the open, dropped with a removed or renamed path. The mock's HTTP server is dual-stack, so a
  loopback client's address arrives as `::ffff:127.0.0.1`; the allow-list reads it as IPv4. A JUCE
  `var` array pointer taken from a temporary dangles (twice in a day, in test code).
- *Still to fill, the author at the DAW:* the done-when moved to real devices (the hand path with
  WFS-DIY on this machine then the Mac mini, the DNS-SD path with go.dot, two mocks sharing a
  NAME, stale live from Chataigne or ossia score, export from REAPER into Bitwig), with the Phase 3
  and 4 rows in the same sitting; the Mac mini and the NUC through `scripts/farm-check.sh`. The
  Release VST3 and the core DLL from `0d4f939` are installed in `%LOCALAPPDATA%\Programs\Common\VST3\`;
  the steps are in the bench report. **CI green on Linux, macOS and Windows** on `e4be98e`
  (run 42608007320), every subphase's run before it green too.

---

## Phase 5b — Our own CLAP shim · M

**Goal:** CLAP, the reference format, from a shim written against the CLAP C API with
`clap-helpers`, depending on neither JUCE's roadmap nor `clap-juce-extensions`. Decided
2026-11-04. Placed here because Bitwig is a record gate and CLAP's gesture path is the cleanest
to test, and because the core has to exist before a shim has anything to call.

- Entry point and a **factory listing all five slot-count variants in one binary**; lifecycle
  (init, activate, start and stop processing, reset); state through the same JSON chunk over
  `clap_ostream` and `clap_istream`; one stereo audio port in and out, copied.
- **Params extension**: count and info per slot, `CLAP_PARAM_IS_HIDDEN` for unbound slots,
  `rescan(CLAP_PARAM_RESCAN_INFO)` on rename, `clap_host_params->clear()` on a stale lane,
  `module` from the OSC path, in-events with sample offsets, out-events with
  `PARAM_GESTURE_BEGIN` / `PARAM_VALUE` / `PARAM_GESTURE_END` for record, `request_flush` when
  not processing. All of it direct calls into the same core as the JUCE shim.
- **The JUCE editor in the host's window**: `create`, `set_parent`, `set_size`, `show`, `hide`
  on Windows, macOS and X11; JUCE initialised inside a non-JUCE plugin; on Linux the host's
  timer and posix-fd extensions pump JUCE's message loop. `clap-juce-extensions`' source is read
  for how it does this and nothing is taken from it beyond that.
- The CLAP binary links the shared core library from Phase 3, so a CLAP and a VST3 instance in
  one process share one registry and one socket per device.
- `clap-validator` in CI; Reaper and Bitwig on the bench; the rename, hide, clear and module rows
  added to the Phase 1 report as `docs/spikes/phase5b-clap.md`.

**Done when:** the Phase 3 and Phase 4 done-whens pass in Bitwig and Reaper over CLAP; an unbound
slot is absent from the host's list; a stale lane is cleared from the host on rebind; a chunk
saved from the VST3 opens in the CLAP with identical bindings and back; `clap-validator` is
clean on three platforms.

**Needs from the author:** nothing blocking. A Bitwig licence on the bench.

**Planned, 2026-11-09.** Taken before Phase 6 at the author's word, in the plan's order. Broken into
seven subphases, each one commit on `main` with its tests, as Phase 5 was: **5b-0** the editor
becomes a `juce::Component` (a thin `EditorWindow` in the JUCE shim keeps pluginval green), because
`EditorRoot` was an `AudioProcessorEditor` and a CLAP has no `AudioProcessor`; **5b-1** the `.clap`
module (one binary, a factory listing the five slot-count variants, lifecycle, audio pass-through,
state over `clap_istream`/`clap_ostream`), a doctest host harness in `tests/clap/` that loads the
built module and drives it against the mock, clap-validator in CI on three platforms; **5b-2** the
params extension (hidden while unbound, `module` from the path, `RESCAN_INFO` on bind, rename and
state load, the `clear()` rule); **5b-3** record (received values drained on the audio thread in
`process` and `flush`, gesture events, `request_flush` from the wake); **5b-4** the editor in the
host's window on win32, cocoa and x11, JUCE's file descriptors pumped through the host's posix-fd
extension on Linux; **5b-5** the loader reusing a core library already loaded in the process, so a
VST3 and a CLAP in one host share one registry; **5b-6** the bench in Bitwig and REAPER on Windows
and the Mac mini, `docs/spikes/phase5b-clap.md`, the records, the handoff.

- *Decided by the author the same day:* **the CLAP ids are `io.github.pob31.oqa.Q008` …
  `Q128`**, the bundle ids the JUCE shim already uses, fixed for good once a session saves one (the
  macOS bundle's own id is a sixth string, `io.github.pob31.oqa.clap`: one bundle, five plugins);
  **one value per slot per block**: the shim consumes every parameter event with its sample offset
  and hands the core each slot's value at block end, the sample-accurate send an open item for a
  device that honours timetags (the Phase 3 record's "the CLAP shim will" is amended in place);
  **`clear()` fires only when a slot whose status was `missing` or `retyped` is rebound to a
  different device or path** (`CLAP_PARAM_CLEAR_AUTOMATIONS`), never on a rename rebind, never on
  unbind, never on rebinding a live slot; **slot values join the chunk** as a `values` field
  (normalised, one per slot), written and restored by both shims, version unchanged, because
  clap-validator's state-reproducibility checks set values, save, reload and compare (the PRD's
  chunk field list gains the entry).
- *Facts found while planning:* `CLAP_PARAM_RESCAN_INFO` covers name, module **and `is_hidden`**,
  immediate while active (`clap/ext/params.h`), so a slot bound during playback is unhidden and
  renamed in one rescan and `RESCAN_ALL` is never needed; `Registry::pollReceived` pops a lock-free
  ring and `Instance::pollReceived` uses stack arrays and relaxed atomics, so the audio thread may
  drain them, `process` and `flush` never being concurrent; `request_flush` is thread-safe off the
  audio thread, so the registry's wake may call it; JUCE's message queue needs no pump of ours on
  Windows (a hidden window fed by `PostMessage`) or macOS (a run-loop source on the main loop), and
  `MessageManager` takes the thread that creates it as the message thread, so JUCE is initialised in
  the plugin's `init()`, not the entry's; JUCE's VST3 wrapper on Linux registers
  `LinuxEventLoopInternal::getRegisteredFds()` with the host and invokes them when ready, the shape
  the CLAP copies through `clap_host_posix_fd_support`; `Component::addToDesktop (0, parent)` takes
  an HWND, an NSView or an X11 window, so no Objective-C of ours; `FileChooser` is in
  `juce_gui_basics`, so the CLAP compiles neither `juce_gui_extra` nor `juce_audio_processors`;
  **the loader walks up from its own module and would load two cores** for a VST3 in
  `Common Files\VST3\` and a CLAP in `Common Files\CLAP\`, each with the copy the build puts beside
  it, nothing asking the OS whether the library is already loaded; `Instance` is runtime-sized, so
  five descriptors in one binary need no `OQA_SLOT_COUNT`; `Registry::shutdown` has no caller in
  `src/shims/`, and a `.clap` unloaded after a scan on its static fallback core would take the
  registry's threads down with it; nothing was measured over CLAP in Phase 1.
- *Implementer's decisions, the author's to overrule:* `connect()` in `init()` and again in
  `activate()`, `disconnect()` in `deactivate()` and `destroy`, so an editor opened on a
  never-activated CLAP has a registry; record drains on the audio thread (`PARAM_GESTURE_BEGIN` on
  a gesture's first value, `PARAM_VALUE` at offset 0, `PARAM_GESTURE_END` once the gesture's age
  passes `gesture_timeout_ms`), the wake calling `request_flush()`, a 50 ms host timer calling it
  only while a gesture is open so gestures close when the host is not processing, one monotonic
  clock read per `process` or `flush` recorded as an exception to the lipogram like the wake
  notify; `Instance::pollReceived` and `GestureTracker` gain function-pointer overloads so that path
  takes no `std::function`; rescans do not wait for a timer (`Host::bindingsChanged` and
  `stateLoad` call `rescan` directly, the 250 ms revision poll covering the rest); `clap_param_info`
  has `id` = slot index, no cookie, `module` = device NAME plus the path's container, flags
  `AUTOMATABLE` plus `HIDDEN` when unbound and `STEPPED` for `i` and toggles, text through the slot's
  own scaling as the JUCE shim's parameter; one stereo port in and out, in-place, copied only when
  the buffers differ; `clap_entry.deinit` calls the registry's `shutdown` only on the static
  fallback core; the loader asks the OS for an already-loaded core first (`GetModuleHandleW` by
  name, `dlopen` with `RTLD_NOLOAD`, a `dyld` image scan by leaf), the table found still passing the
  version and size check or falling back with a note naming the other plugin's path.

**Record, 2026-11-09 (built; the DAW rows of the done-when open, Phases 3 to 5's with them).**
Built in six commits on `main`, 5b-0 to 5b-5, in one session, plus the macOS fix below. Reports:
[`docs/spikes/phase5b-clap.md`](spikes/phase5b-clap.md) (the format matrix the Phase 1 report lacked)
and [`docs/bench/2026-11-09-phase5b-clap.md`](bench/2026-11-09-phase5b-clap.md).

- *Done so far, measured on this machine (Debug, loopback) through a CLAP host of the tests' own
  (`tests/clap/`):* the module loads and its factory lists the five variants under the agreed ids;
  each is created, activated, processed with audio through, deactivated and destroyed; the chunk goes
  in and out through CLAP's streams byte for byte, in small pieces, the parameters' values with it;
  every slot a 0–1 parameter, `HIDDEN` and `(unbound)` until bound, then named by its path with
  `module` = device NAME plus the path's container and text in device units; `rescan (INFO | VALUES)`
  at a state load; events reach the device from `process` and from `flush`; `clear()` fires for a
  `missing` slot rebound elsewhere and for nothing else; a value the device streams becomes
  `GESTURE_BEGIN`, `PARAM_VALUE`, `GESTURE_END` in `process`, or in the flush the wake asked for while
  the host slept an active plugin; a gesture open at deactivate owes its END to the next flush; a
  burst of 160 values over eight slots arrives whole and in order; the gui is offered embedded on the
  platform's api, opens at 1100×620, clamps to the editor's limits, and on Windows sits as a child of
  the host's window at the size agreed in physical pixels. **clap-validator 0.4.1 on the Release
  module: 138 passed, 0 failed, 54 skipped.** The loader finds a core already loaded in the process
  and the registry's resolver restarts after `shutdown()` (`tests/core/`). pluginval 1.0.4 passes the
  Debug VST3 at strictness 5 with the editor in its new window. The CLAP suite is **13 cases** as
  `clap.C` and `clap.fr_FR`; the core's is **113** as `core.C` and `core.fr_FR`.
- *Implementer's decisions, the author's to overrule:* a vendor extension of ours,
  `io.github.pob31.oqa.status/1`, answers the instance's status and the settings as JSON through the
  CLAP API, for the test host and for diagnosis without the editor; the shim's `clear()` rule reads
  the standing it recorded at its last 250 ms tick, so a rebind inside that window after a path
  vanished is not cleared; the module and the core library each export one symbol at the linker's
  level too (an export list on Apple, a version script with `-Bsymbolic` on GNU ld); the editor's
  parenting on macOS and X11 is the bench's, the test executable having no window toolkit there.
- *Found on the way:* **two copies of JUCE in one process on macOS meet through weak symbols**
  despite hidden visibility everywhere: the runner aborted freeing a `juce::String` after a `var`
  made in the module was torn down through the test executable's JUCE, whose `var` type singletons
  (constexpr inline variables) dyld had coalesced with the module's; the empty-string singleton of one
  copy, compared against the other's, was freed as if allocated. Windows and Linux never showed it.
  The test runner prints a backtrace on a fatal signal on POSIX now (it did on Windows), which is how
  it was read. clap-helpers' checks (`Terminate`, `Maximal` in Debug) taught the harness that `flush`
  is the audio thread's while the plugin is active and that `set_size` takes only what `adjust_size`
  returned. A deactivated instance has left the registry, so nothing streams to it. The realtime
  counters are per module, so the harness cannot count the module's allocations. The clap-validator
  release zips for Linux and macOS wrap a tarball named 0.3.2 by the release's own tooling. The
  macOS job's export listing, once the fix was in: the module and the library export one symbol
  each, their `var` singletons "non-external (was a private external)"; the test executable exports
  2,845, its singleton a *weak external*, which is the side dyld coalesced towards.
- **CI green on Linux, macOS and Windows** on `9b80600` (run 42625676758): the CLAP suite passes on
  all three, clap-validator 139 passed and 0 failed on the macOS runner, 138 on Windows and Linux.
- *Still to fill, the author at the DAW:* the done-when's rows in Bitwig and REAPER over CLAP on this
  machine and the Mac mini (the Phase 3 and 4 done-whens, hide, rename, clear, module, VST3 ↔ CLAP
  chunk, one registry for both formats, display scaling, keyboard focus), with the Phase 3, 4 and 5
  rows in the same sitting; the Mac mini and the NUC through `scripts/farm-check.sh`, which builds
  the CLAP and runs clap-validator now. The install steps are in the bench report.

---

## Phase 6 — The editor finished, the plugin's own server, the Monitor · M

**Goal:** the PRD's UX section complete and the headless path dogfooded.

- Drag from tree to slot; bulk bind of a multi-argument node across consecutive slots; `$instance`
  across instances; *clear all stale*; `RANGE` from the device greyed when overridden; *Copy tree
  as JSON*; search on path and `DESCRIPTION`; reachability dot and last-seen time.
- Status bar per device: packets per second each way, websocket state, last error.
- **The plugin's own OSCQuery server**: the slot table at `/plugin/<instance>/slot/<n>/...`
  served over HTTP and websocket from the registry (one server per process, announced over
  DNS-SD by the library's announcer). Binding a slot through it does exactly what the editor does.
  This is the first server the conformance CLI runs against in Phase 8, so it must pass its own
  matrix.
- **Monitor plugin**: a separate plugin ID with zero slots that opens the registry view: devices,
  every slot across instances, rates, errors, global settings. Owns nothing; any instance can open
  the same view from a button.
- Global settings (recents, default rate limit, loop guard window, gesture timeout, direct send)
  in the per-user settings file, editable from the Monitor.

**Done when:** everything in the PRD's UX section is reachable; a script binds a slot over the
plugin's OSCQuery server with no editor open and the host lane renames; the Monitor shows 32
instances against two devices with live counters.

**Needs from the author:** a look at the layout before the drag-bind and bulk operations are
polished; the words for the failure-state tooltips.

**Planned, 2026-11-09.** Broken into nine subphases, each one commit on `main` with its tests, as
Phase 5b was: **6a-1** the registry's settings persisted per user (a file beside the recents, loaded
at registry start, saved on change; new keys for the server); **6a-2** the Monitor's data (the
process-wide status lists every instance with its bound slots and standing, per-device rates over a
one-second window computed once in the registry, the device's own RANGE beside a slot's; instances
registered for the plugin's life with an inactive flag); **6a-3** device timing settable process-wide
with a hand-set precedence, copied back into every chunk on save and into the recents; **6b-1** a
generic OSCQuery server in the library (`lib/oscquery/oscquery/server/`: a `Tree` served over HTTP and
websocket on juce_simpleweb, LISTEN, the `PATH_*` notifications, OSC in over websocket and its own
UDP port, per-client ACCESS by address for both, measured against C1, C4, C6, C7, L1–L4 by the
library's own client); **6b-2** the plugin's server in the registry (`src/core/server/`), read-only,
announced over DNS-SD by the Finder's Discovery, its tree `/plugin/<id>/slot/<n>/...`, `/device/<i>/...`,
`/server/...`, C5 across a restart; **6b-3** writes: a slot bound, ranged, scaled, directed, named or
unbound over the server through a command queue per instance, a second wake and a drain on the shim's
main thread, the host told; a stdlib Python script as the worked example; **6d-1** the editor's
data-driven items (rates each way, websocket state, last error with its age, reachability dot and last
seen, the bind refusal and the Test error shown, the device RANGE greyed when overridden with a reset,
per-device timing, inline display name, *Clear stale*, *Copy tree as JSON*, the tooltips); **6c+6e**
the Monitor plugin (`QMon` for VST3 and AU, a sixth CLAP descriptor) and its view (devices, every
instance's slots, the server, the settings) over a headless model; **6d-2** the interaction items after
the author's look (drag a leaf onto a slot, `$instance`, *Monitor…* from any editor); **6f** the bench,
the records, the handoff.

- *Decided by the author the same day:* **the Monitor is a sixth CLAP descriptor**,
  `io.github.pob31.oqa.QMon`, zero parameters, fixed for good; **server writes from loopback only by
  default**, a setting (`server_allow`) opens it, other clients see the writable leaves as ACCESS 1,
  the same list for UDP and websocket OSC; **the server's HTTP port is ephemeral by default**,
  announced, shown in the Monitor, a setting pins it, a pinned port that fails to bind is a visible
  error and never a silent fall-back; **`$instance` is a number the user sets per instance**, kept in
  the chunk (`instance`, default 1), a path typed with the token bound with it substituted and the
  pattern kept (`path_pattern`) so the Monitor applies one pattern across instances, the track name a
  hint only; **instances stay registered for the plugin's life**, inactive while the host has stopped
  their audio (the Phase 3 contract amended in place); **the server's NAME is "OSCQuery Automation @
  <machine> (<host app>)"** with a read-only `/server/process_id` leaf as its identity leaf.
- *Facts found while planning:* the registry's UDP receiver starts only when a receiving slot exists
  or the port is set, and routes by device group path then sender, so it cannot be the server's
  `OSC_PORT` (the server gets a UDP endpoint of its own); every push re-applies the chunk's device
  timing, so a Monitor-set timing needs a hand-set precedence like the address and `saveState()`
  copying it back; `statusJson (void)` is in the ABI already and only `Instance` lacks the wrapper;
  `Recents` owns its `PropertiesFile`, and two such objects on one file overwrite each other's keys,
  so the settings get a sibling file; `status()` serialises under the registry lock that every
  streamed value takes, so the Monitor and the server would stall record unless the snapshot is taken
  under the lock and serialised outside; `Slot::setRange` overwrites the device's range, so the device
  RANGE for the greyed display comes from the plan's check (`device_range_min/max` in `slots[]`), no
  chunk change; a NAME cannot be an OSC address segment, so `/device/<ordinal>/name`; the CLAP's record
  wake lands on the audio thread, a command needs `request_callback` → `onMainThread()`; the mock clamps
  ACCESS on HTTP only, so it is no reference for L3; announcing on the Finder's Discovery means browsing
  runs from registry start; `ownPort` keeps our own server out of our own Finder, another process's
  appears as a bindable device. **The PRD fixes the namespace `/plugin/<instance>/slot/<n>/...`
  (UX, Headless use): it is the PRD's address, not a device's, and the one exception to "no typed OSC
  address in `src/`"; every other segment is the implementer's and lives in one file,
  `src/core/server/ServerNamespace.h`.**
- *Implementer's decisions, the author's to overrule:* the generic server in the library and the
  plugin's tree in the core, the mock not rebuilt on it in this phase (its fault injection is
  scaffolding, and a mock on the server under test would make the self-check circular; Phase 8's CLI is
  the independent judge); the server's `OSC_PORT` its own UDP endpoint, `WS_PORT` absent, `EXTENSIONS`
  ACCESS, VALUE, RANGE, TYPE, DESCRIPTION, LISTEN, PATH_CHANGED, PATH_ADDED, PATH_REMOVED,
  `server_enabled` on by default; the tree as the plan names it (`label`, `instance`, per slot `path`,
  `device` as NAME, `arg_index`, `type`, `range/min`, `range/max` with a RANGE of ±1e6 device units so
  C4 holds on our own leaves, `scaling`, `direction`, `display_name`, `value` 0–1 read-only, `state`
  read-only; `/device/<i>` and `/server` read-only), the Monitor's 0-slot instance left out, `path` ""
  an unbind, PATH_ADDED/REMOVED as instances come and go; writes as commands (strings) in a
  mutex-guarded deque per instance, two additive ABI entries, drained on the shim's main thread (JUCE
  `AsyncUpdater`, CLAP `request_callback`, the 50 ms tick as fallback), `Instance::bindPath` reusing the
  editor's rebind lookup and serving the typed `$instance` path too, a `device` NAME accepted when the
  registry or Finder knows it and refused with the reason in `state` otherwise, results asynchronous
  by design; rates over one second ticked by the resolver thread; `instances` the array and
  `instance_count` the count; `deviceSetTiming` additive with a hand-set flag; `path_pattern` and
  `instance` optional additive chunk fields, version unchanged; *Clear stale* unbinds `missing`,
  `retyped` and `renamed` slots and not `unauthorised` ones, and never fires CLAP's `clear()`; three
  failure rows in the PRD's table first: server write refused, server port in use, announce failed;
  the tooltip words proposed from the PRD's table (unreachable "no HOST_INFO answer since <ago>",
  unauthorised "the device reports ACCESS <n> to this client: not on its allow-list", stale "<path>
  missing/retyped/renamed → <new>; the lane keeps its automation, nothing is sent", range unknown "no
  RANGE: enter min and max", echo loop "<n> echoes dropped by the loop guard", dropped sends "<n> held
  by the rate limiter, <n> datagrams dropped").

**Record, 2026-11-09 (built; the DAW rows of the done-when open, Phases 3 to 5b's with them).**
Built in nine commits on `main` in one session, 6a-1 to 6f, in the plan's order, plus one fix between
6b-3 and 6d-1 (the Windows exit, below). Report:
[`docs/bench/2026-11-09-phase6-monitor.md`](bench/2026-11-09-phase6-monitor.md).

- *Done so far, measured on this machine (Debug, loopback) by the suites, and in CI on three
  platforms:* the registry's settings persist in a per-user file beside the recents and read back
  through a second registry; the process-wide status lists every instance with its bound slots and
  the registry's word on each, rates each way over a one-second window, the server, the settings
  file and the Finder; a device's timing set by hand, by index or by NAME, stands process-wide and
  lands in every chunk on save; the library has an OSCQuery server of its own, judged by the
  library's own client on C1, C4, C5, C6, C7 and L1 to L4; the registry serves the plugin's slot
  table at the PRD's namespace, announced over DNS-SD as "OSCQuery Automation @ <machine> (<host
  app>)" with `/server/process_id` as its identity leaf, and a write to it binds, ranges, scales,
  directs, names or unbinds a slot through a command queue the shim drains on its main thread, the
  host told: **the done-when's second clause holds headless in CI**, the CLAP harness seeing `rescan
  (INFO)` and the new name after a write with no gui; `tools/oqa-bind.py` is the worked example. The
  editor shows the registry's rates, the websocket's word, the last error with its age, greys an
  unreachable device, edits the display name in place, shows the device's RANGE greyed when
  overridden and goes back to it, clears the stale slots, copies the tree as JSON, takes a device's
  timing by hand, says why a bind or a Test was refused, and gives every row a tooltip in the PRD's
  failure table's words, from pure functions tested headless. The Monitor is a sixth plugin, `QMon`
  for VST3 and AU and `io.github.pob31.oqa.QMon` for CLAP, zero slots, whose editor is the registry's
  view: devices with their timing and address editable in place, every bound slot of every instance,
  the server line, the settings form, *Clear recents*, and one pattern bound across every instance at
  once; any editor opens the same view from *Monitor…*. A leaf drags from the tree onto a slot row; a
  path typed into the Path cell binds, `$instance` substituted by the instance's number and the
  pattern kept as `path_pattern`, so a change of number rebinds and a click-bind drops the pattern.
  **The suites: unit 11, oscquery 106, core 134, clap 15 cases**, under C and fr_FR. **pluginval
  1.0.4 at strictness 5 passes both Debug VST3s; clap-validator 0.4.1 on the Release module with six
  plugins: 229 run, 155 passed, 0 failed, 74 skipped.**
- *Implementer's decisions while building, the author's to overrule:* the Monitor offers **no params
  extension** rather than one counting zero (clap-validator 0.4.1 divides by the count in
  `param-conversions`), is an `analyzer` and `utility` to CLAP hosts, and keeps the stereo
  pass-through buses so a host inserts it on a track; the Monitor's actions go by NAME and identity
  (`setDeviceTimingByName`, `pointDeviceByName`), its chunk having no devices; applying a pattern
  across instances is **a command queued for an instance by id** (additive ABI
  `instanceEnqueueCommandJson`, `Instance::commandInstance`), the same commands the server queues,
  each instance substituting its own number, so the Monitor still owns nothing; the server's own bind
  command takes a pattern the same way; a change of number whose new path the device does not serve
  **keeps the slot's binding and warns** which path was not served; *Clear recents* removes each entry
  (`Recents` has no clear); the shade and tooltip functions live in a header with no JUCE
  (`src/editor/SlotPresentation.h`) and the Monitor's rows in a juce_core-only model
  (`MonitorModel`), both compiled into the core's tests; the editor's tree is re-read only when the
  status's live hash moved, not every tick; the status bar's rates are the registry's, the editor no
  longer diffing counters itself.
- *Found on the way:* **at a process's end on Windows, `ExitProcess` kills every other thread before
  the DLLs' static destructors run**, so a registry destructor that joined its threads failed fast
  (0xC0000409) in every clap-validator subprocess once the server ran by default; the shared library's
  `DllMain` (and the CLAP module's) notes `DLL_PROCESS_DETACH` with `lpReserved` set, and the
  destructor then leaves everything to the OS; the resolver (the Finder) stops before the server, whose
  unannounce used to restart it during static destruction. Thirty-seven clap-validator subprocesses of
  the older build are still deadlocked in exit on this machine and hold the Release DLL open, so the
  Release CLAP target fails at its copy step until a logoff; the module itself builds, and
  clap-validator ran on a copy of it beside a fresh DLL. On the macOS runner, a test that read the
  status of 32 instances every 10 ms block slowed the blocks below the rate it asked for; it reads
  every tenth block now and asks for live rates above zero. A CI run (42673452864) twice created only
  the `pins` and Linux jobs with no failure listed, a GitHub-side glitch; the next push ran four. A
  `const std::string&` parameter that was the slot's own `pathPattern` was cleared by `bind()` before
  it was read back: `bindPattern` copies it first. JUCE's `TreeView` starts a drag by itself once an
  ancestor is a `DragAndDropContainer`, the item only naming its description. A doctest raw string
  holding `(REAPER)"` ends the literal: a delimiter. **On the NUC, one clap-validator subprocess in
  229 died with SIGSEGV in the server's HTTP reply** (`coredumpctl` read the core: the websocket
  thread answering a sibling subprocess's Finder probe while the main thread was in
  `Registry::shutdown()` at exit): SimpleWeb keeps its status-code table in a function-local static
  made on the first reply, so it was destroyed before the registry singleton made earlier, and a
  request answered in that window read a dead map. The library now makes the table when it loads, so
  it outlives the registry; and `shutdown()` stops the server's serving first, without unannouncing
  (the unannounce would restart the Finder), then the resolver. The farm's clap-validator runs after
  the fix are in the bench report.
- **CI green on Linux, macOS and Windows** on `173e733` (run 42688672120): every suite, pluginval on
  the 32-slot plugin and the Monitor, clap-validator on the six-plugin module, on all three.
- *The author's open items from planning:* the layout before the drag-bind and bulk operations are
  polished, and the words for the failure-state tooltips, which stand as the implementer's defaults in
  `SlotPresentation.h`.
- *Still to fill, the author at the DAW:* the done-when's rows in REAPER and Bitwig over VST3 and CLAP
  on this machine (every PRD UX item, the script binding a slot with the editor closed and the lane
  renaming, the Monitor with 32 instances against WFS-DIY and go.dot, two DAWs announcing distinct
  NAMEs, the firewall prompt once), with the Phase 3, 4, 5 and 5b rows in the same sitting; the farm's
  results are in the bench report. Phase 7 starts from here: the other four slot-count variants are one
  line each in `src/shims/juce/CMakeLists.txt` (the function takes the count, the code, a tag and the
  product name now) and already listed by the CLAP factory; the Monitor VST3 joins the installer and
  pluginval runs on it in CI already.

---

## Phase 6x — Optional extensions: `DEVICE_ID` and `CLOCK` · M

**Goal:** the plugin recognises a device by the identifier it declares and takes its word on
timetags, and says both of itself, without asking anything of a device that declares neither.
`docs/spec/extensions-draft-0.1.md`, first written by Julien Bayle (pull request #1).

- **One rule throughout:** an extension is used only when `EXTENSIONS` lists it. A key that is
  present but not declared is ignored. A device with neither behaves as on `e4cc790`, to the byte
  of its saved chunk.
- **Library**: `HOST_INFO` parsing reads `DEVICE_ID` (a non-empty string) and `CLOCK` (`SOURCE`,
  optional `DOMAIN`, `SCHEDULING`) when declared; the server serves them from its options and
  lists what it serves, so served and declared cannot drift apart.
- **Mock device**: extra `HOST_INFO` members given verbatim, so a test can build the honest
  device, the one that serves a key without declaring it, and two devices under one identifier.
- **State chunk**: `devices[]` gains an optional `device_id`, written only when set; version 1
  stands (the `path_pattern` and `instance` precedent), with its fixture in `tests/state/`.
- **Re-identification**: the identifier goes in front of the PRD's sequence and replaces nothing.
  At the last address, equal identifiers confirm the device even under a new `NAME` and different
  ones are a mismatch even under the same; over DNS-SD a device declaring the saved identifier is
  looked for first, whatever its `NAME`. Two live devices under one identifier are unresolved and
  the user points; a candidate declaring another identifier is listed and never adopted without
  the user. The registry's key stays `NAME` plus the identity leaf's value.
- **Timing**: a declared `SCHEDULING` pre-fills *honours timetags*. The hand wins over the
  device's word, and the device's word over the chunk's. The offset is never the device's.
- **The plugin's own server** serves a `DEVICE_ID` kept per user and derived per host
  application, and a `CLOCK` that claims the least (`local`, not scheduling). The Monitor shows it.

**Done when:** against two mocks sharing a `NAME` with no identity leaf, a session reopens bound
to the one that declares its identifier; the same session against a mock renamed since still
finds it; two mocks under one identifier leave it unresolved until pointed; a mock declaring
`SCHEDULING` true has its timetags setting ticked without a hand, and a hand that unticks it
stays; a session saved before this phase reloads and saves to the same bytes against a device
with neither extension, and WFS-DIY live behaves as before.

**Needs from the author:** nothing blocking. The implementer's decisions under *Planned* are the
author's to overrule.

**Planned, 2026-11-10.** Taken before Phase 7 at the author's word, and lettered x because
6a to 6f are Phase 6's own subphases. Two pull requests, the first
since Julien Bayle's: the text (this section, the PRD's dated amendments, the corrected draft,
C5's wording), then the code in seven subphases, each one commit with its tests: **6x-1** the
library parses and serves; **6x-2** the mock device; **6x-3** the state chunk; **6x-4**
re-identification; **6x-5** the clock on the client; **6x-6** the plugin's own server and the
Monitor; **6x-7** the bench report and this record.

- *Decided by the author the same day:* the pull request merged as it was and corrected on top;
  the extensions optional and outside Core and Live, so "the Core profile adds no field" stands;
  the plugin a client of both and a server of both. This is the recorded yes the cross-cutting
  rule asks for before a clause beyond the PRD is implemented.
- *Facts found while planning:* `tools/conform/` is a README until Phase 8, so the two checks
  (X1, X2) are a table in the draft and a line in Phase 8, not code; the tree hash has always
  been over every typed leaf (`lib/oscquery/oscquery/tree/Tree.h`), where C5 and the PRD said
  "bindable", which depends on the asking client and which no device could serve: both texts are
  amended; `HostInfo` keeps the device's object whole in `raw` but nothing reads it; each DNS-SD
  candidate already carries its parsed `HOST_INFO`, so looking for an identifier costs no request;
  a received bundle's time tag is decoded and then dropped on both receive paths
  (`Device::deliverPacket`, the registry's `route`); the per-device offset is applied to time tags
  only, and no send delay exists for a device with the setting off, though the PRD and two
  tooltips say one does; the plugin's server emits `WS_PORT`, where the Phase 6 plan said it was
  absent; `flush()` reads the timing setting on the network thread outside the lock that
  `setTiming` writes it under.
- *Implementer's decisions, the author's to overrule:* the server's `DEVICE_ID` is derived from a
  random seed in the per-user registry settings file and the host application's name, since the
  server is one per process and has no project to keep an identifier in, and two DAWs on one
  machine share that file (the draft says "a UUID" for it, where it first said version 4); the
  server's `CLOCK` declares `local` and no scheduling, since it applies a write on arrival and
  cannot know what disciplines the system clock; no node is added to the server's namespace; the
  time tag of a received bundle stays unused, since placing recorded automation by it needs an
  agreement on the clock that no bench device offers; the missing send delay and the `WS_PORT`
  line are left as the author's open items, and the unlocked read is fixed in 6x-5.

---

## Phase 7 — AU, Linux, variants, installers · M

**Goal:** the same session file opens in all three formats. PRD milestone 5.

- AU, built by JUCE since Phase 3, now measured: `kAudioUnitProperty_ParameterInfo` change
  notification, `clump` (fixed per slot by JUCE's parameter groups, Phase 1), gesture events for
  record. Logic's partial rename
  behaviour written down in `docs/bench/`.
- Linux VST3 and CLAP (the Phase 5b shim); LV2 only if JUCE gives it for free.
- The slot-count variants (8/16/32/64/128) as plugin IDs, generated from one CMake loop for the
  JUCE formats and listed by the factory for CLAP; all of them share the registry through the
  shared core library from Phase 3.
- Installers from WFS-DIY's scripts: Inno Setup on Windows, `.pkg` with codesign and
  notarisation on macOS, tarball with `install.sh` on Linux; CLAP paths added.
- pluginval on every format in CI; `auval` on macOS.
- A cross-format test: a state chunk saved from the CLAP opens in the VST3 and the AU with the
  same bindings, since the chunk is format-neutral by construction.

**Done when:** one Reaper session on the Mac mini opens the plugin as CLAP, VST3 and AU with
identical bindings; Live 12 and GarageBand run the AU and record (the Mac mini has no Logic,
inventoried 2026-11-04; a Logic row waits for a machine that has it); the NUC runs the Linux
CLAP and VST3 in a host against WFS-DIY on the farm's network; all installers produced from a
clean checkout.

The NUC has REAPER 7.84 (installed 2026-11-04) and a GNOME desktop, but SSH sessions have no
display: before this phase the `pob` user is logged into the desktop, or autologin is enabled,
so `DISPLAY=:0` works over SSH; pluginval and clap-validator are installed on both farm machines
at the same time.

**Needs from the author:** signing identities. Nuendo is decided (VST3, best-effort naming);
this phase writes down what `kParamTitlesChanged` actually does there, in `docs/bench/`.

---

## Phase 8 — Conformance CLI, the library on its own · M

**Goal:** the interoperability matrix produced by a tool, not by hand. PRD milestone 6.

- `tools/conform`: one executable, `conform <host:port>` or `conform --browse`, runs C1–C7 and
  L1–L5 as the PRD defines them, prints the matrix row as Markdown and JSON, exits non-zero on
  any Core failure. C5 (path stability across reboot) takes a `--before`/`--after` pair of tree
  snapshots. L5 reports the time, not just pass/fail. The two optional extensions' checks, X1 and
  X2 of `docs/spec/extensions-draft-0.1.md`, run only against a device that declares them, and
  never fail a Core or Live row (added 2026-11-10).
- Run against: the plugin's own server, the mock device (with fixtures that fail each check on
  purpose, as the CLI's own test), WFS-DIY, XOA/Tight-WFS, the PRISM haptic floor controller,
  Holophonix, ossia score, Chataigne.
- The library moves to its own GPLv3 repo with the CLI as its first consumer; the plugin pins it
  as a submodule, and go.dot can replace its own client with it when it wants to.
  `docs/matrix.md` holds the rows with dates and firmware versions.
- The CLI ships as a signed binary per platform on the release page, since a manufacturer
  running it against their device is the one artefact of this project they will touch.

**Done when:** the CLI's own tests pass; rows exist for every subject on the bench; a
manufacturer can run it on their device with one command and get the same row.

**Needs from the author:** bench time with each subject; who at each manufacturer receives their
row.

**Companion in WFS-DIY:** DNS-SD announcement of `_oscjson._tcp` (C3), via the same announcer
the library ships, with the instance name equal to `NAME` (C2). WFS-DIY honours bundle
timetags per the PRD; its bench row records that the per-device setting was ticked.

---

## Phase 9 — Public beta · M

**Goal:** v0.9 on GitHub, the matrix published, the profile draft published with it. PRD
milestone 7. Standardisation is out of scope (removed 2026-11-04): a profile becomes a standard
when manufacturers adopt it, and that is not code.

- Release workflow from WFS-DIY's `plugins-release.yml`: tagged builds, signed installers, a
  changelog.
- README for users (bind in three clicks), README for manufacturers (the Core profile in one
  page and how to run `conform`), README for implementers of other clients (the library).
- Profile draft 0.2 incorporating what the matrix taught, published in `docs/spec/` with the
  release.
- Each manufacturer contacted with their row and the CLI.

**Done when:** the release is public, the matrix page is live, the profile draft is in the
repository, and the issue tracker is open.

**Needs from the author:** who at each manufacturer receives their row.

---

## Cross-cutting, every phase

- **Three-platform CI** green before merge; pluginval from Phase 3 on.
- **RT-safety instrumentation** on the process path stays on in tests.
- **Locale test**: everything that serialises numbers (state chunk, export, the library's JSON)
  runs under `fr_FR` as well as `C`.
- **State chunk fixtures**: every version ever shipped stays loadable; a fixture per version in
  `tests/state/`.
- **Two-instance test** from Phase 3 on: two instances in one process share one registry entry
  per device and produce one bundle.
- **Mock device first**: no test needs a real device; real devices are bench checks recorded in
  `docs/bench/` with the date and firmware.
- **No typed address** anywhere in `src/`; addresses appear only in fixtures.
- **No *(proposed)* spec clause implemented without a recorded yes.**
- Every failure visible in the editor, never silent (PRD *Access control and failure states*); a
  new failure mode gets a row in that table before it gets code.

---

## Needs from the author, collected

None open as of 2026-11-04. The standardisation route and the spec's institutional home were
removed from the PRD the same day: they depend on adoption by manufacturers, which is not this
project's deliverable.

Decided already, all on 2026-11-04: language and framework (C++20, JUCE 9), licence (GPLv3),
copyright holders (Pierre-Olivier Boulant, Julien Bayle), no new `HOST_INFO` fields (no UUID, no
`TIMETAGS`), the target pointed at once through the registry and the Monitor, Nuendo on VST3
with best-effort naming, record over LISTEN when declared and otherwise over a UDP port shown in
the editor and configured on the device side, VST3 and AU first through JUCE's wrappers with our
own CLAP shim in Phase 5b, five slot-count plugin IDs and the registry in a shared core library.

Decided 2026-11-10: two optional extensions beside the profile (`DEVICE_ID`, `CLOCK`), the plugin
their client and their server (Phase 6x); pull requests from that day on.
