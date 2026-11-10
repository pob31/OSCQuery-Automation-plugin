# Bench, 2026-11-10: Phase 6x, the optional extensions `DEVICE_ID` and `CLOCK`

What Phase 6x built was measured on the author's Windows machine (Windows 11, VS 2026, Debug),
against mock devices on loopback and a DNS-SD responder of the tests' own. Everything below is the
test suite's own word (`lib/oscquery/tests/HostInfoTests.cpp`, `ServerTests.cpp`;
`tests/core/StateChunkTests.cpp`, `ReidentifyTests.cpp`, `TimingTests.cpp`, `ServerTests.cpp`,
`MonitorModelTests.cpp`), repeatable with `ctest --preset vs-debug`: 8 of 8 entries pass, under C
and fr_FR. **Not run this day:** the farm (the Mac mini and the NUC were not reachable), WFS-DIY
live (it was not running), any DAW. Those rows are at the end, to fill.

The extensions are those of [`docs/spec/extensions-draft-0.1.md`](../spec/extensions-draft-0.1.md).
No device on the bench declares either yet; the mock device is the only one that does.

## Reading and serving

| Check | How | Result |
| --- | --- | --- |
| Declared and read | `HOST_INFO` with both keys, listed in `EXTENSIONS` as an object and as a list | **`deviceId` trimmed and kept as text; `clock` with `source` lower-cased, `DOMAIN` read whether a string or a number, `SCHEDULING` true only for a boolean true** |
| Present, not declared | the same keys with `EXTENSIONS` listing neither, listing them false, or absent | **neither read; both still in `raw`** |
| Declared, malformed | a number for `DEVICE_ID`, a string for `CLOCK`, a blank identifier, a clock with no `SOURCE` | **ignored; the device still reads and still meets C1** |
| The library's server | options with an identifier and a clock, the owner listing neither in `extensions` | **both served and both listed by the server itself; unset, `HOST_INFO` has neither key** |
| The mock device | extra `HOST_INFO` members given verbatim, declared or not by the test | **read only once declared; `oqa-mock-device --device-id … --clock-source ptp --clock-domain 0 --clock-scheduling` serves and declares both, and `oscquery-dump` prints them** |

## The state chunk

| Check | How | Result |
| --- | --- | --- |
| `device_id` and `timetags_by_hand` | `tests/state/v1-device-id.json`: one device with both, one with neither | **read; written back only on the device that had them; the round trip byte-identical; version 1** |
| A chunk saved before | `v1-generic.json` loaded and saved | **neither key appears** |
| Not an identifier | `device_id` a number, or empty | **not kept, not written** |

## Re-identification by `DEVICE_ID`

| Check | How | Result |
| --- | --- | --- |
| A device renamed since, at its address | the mock answering under another NAME than the chunk's, the same identifier | **`confirmed`, `answered_name` the new one, no note; driven; the chunk keeps its name and identifier** |
| The same, moved | nothing at the last address, no device under the chunk's NAME, the mock announced under its new one | **`confirmed` at the mock's address; the next save carries it** |
| Two devices sharing a NAME, no identity leaf | two mocks named alike with different identifiers; the chunk holds the second's | **`confirmed` at the second; the first never sent to** |
| Two live devices declaring one identifier | both known to the Finder before the session asks | **`unresolved`, two candidates each `id_match`, the warning "2 devices declare its DEVICE_ID"; nothing driven; pointed once, `pointed` and driven** |
| The second seen later | a device confirmed and in use, then a copy with its identifier announced | **the device in use stays `confirmed` and driven; the warning "declares the same DEVICE_ID", gone when the copy goes** |
| Another identifier under the right NAME | at the last address, and a second one announced under the same NAME | **`mismatch` at the address ("it declares DEVICE_ID …, not …"), then `unresolved` with the other listed `id_known`, not `id_match`, "none matching"; nothing sent to either, no LISTEN; the chunk's identifier unchanged** |
| An undeclared `DEVICE_ID` | the key served, not listed | **NAME decides, as before; `device_id_live` empty** |
| A session saved without one | a chunk with no identifier against a device that declares one | **`confirmed` by NAME, the identifier learnt, written by the next save** |
| A device with neither | the plain mock | **no `device_id` in the status or the saved chunk** |

## The clock

| Check | How | Result |
| --- | --- | --- |
| `SCHEDULING` true, nobody's hand | a chunk saying false, a mock declaring it | **`honours_timetags` true, `timetags_origin` `device`; the bundle on the wire carries a time, not the immediate tag; saved as the value with no `timetags_by_hand`; a later push of the chunk does not undo it** |
| A hand over the device | unticked from one instance, with an offset; the other instance pushes its chunk | **false for both, origin `hand`; both save it with `timetags_by_hand` and the offset** |
| A hand's choice on reload | a chunk with `timetags_by_hand` and false, against a device declaring true | **false, origin `saved_hand`** |
| A chunk's value on reload | a chunk saying true with no hand behind it, a device declaring false | **false, origin `device`; the offset the chunk's, never the device's** |
| Undeclared, or no clock | the key served and not listed; the plain mock | **the chunk's word, origin `chunk`, `clock` null; the immediate tag on the wire** |
| In words | the Monitor's model from a status fixture | **"The device declares its clock: ptp, domain 0, scheduling by timetag. Ticked from that declaration; a hand overrules it." and the four other cases** |

## The plugin's own server

| Check | How | Result |
| --- | --- | --- |
| Serves and declares both | `GET /?HOST_INFO` read by the library's parser | **`DEVICE_ID` a UUID of version 8, the one the seed and this executable's name give; `CLOCK` `local`, not scheduling, no domain; the status carries the identifier** |
| Holds | the server renamed (a restart on another port), then turned off and on | **the same identifier each time** |
| One per seed and per host application | `deviceIdFor` with two seeds and two application names | **all different; the same in, the same out; none for no seed** |
| The seed's file | a settings file path in a folder of the test's | **`<name>.server-id` made once and read after; the settings file itself not created** |

## Found on the way

- **The DNS-SD browser kept an earlier start's records.** `Discovery::start` cleared its
  interfaces and not what it had heard, so a Finder restarted with other options went on listing,
  and probing, devices it could no longer hear withdraw, for a PTR's 75 minutes. With a dozen dead
  ones the probe of a new device waited past twenty seconds. It starts each browse fresh now (the
  one change made to a file copied from go.dot).
- **A search adopts the first device it sees.** DNS-SD does not say when it has finished, so of two
  devices under one identifier (or one NAME) the one seen first is adopted when it is seen alone.
  For an identifier the second is now a warning when it appears; for a NAME this was already so
  and is unchanged.
- **The stale tests' retype case** waited for the tree and not the websocket, unlike its siblings;
  the notification sent before the websocket is open is never heard. Fixed in the test.
- **Still open, the author's:** the per-device offset is applied to time tags only, and no send
  delay exists for a device with the setting off, though the PRD and two tooltips say one does;
  the plugin's server emits `WS_PORT`, where the Phase 6 plan said it was absent; a device seen on
  two interfaces is two candidates with one identifier, and is left to the user like a copy.

## To fill

| Check | How | Result |
| --- | --- | --- |
| The farm | `scripts/farm-check.sh` on the Mac mini and the NUC | _to fill_ |
| CI on three platforms | pull request #3's run on `3855c00` (42885141999) | **green on Linux, macOS and Windows: every suite, pluginval, clap-validator** |
| A device with neither extension, live | WFS-DIY (HTTP 5005) bound in REAPER before and after this phase; a session saved before reloads and saves to the same bytes | _to fill_ |
| Two mocks under one NAME in the editor | `oqa-mock-device --device-id …` twice, the plugin in REAPER: the candidate rows say "same DEVICE_ID" and "other DEVICE_ID" | _to fill_ |
| The timetags box | a mock with `--clock-scheduling`: the device pane's box ticked without a hand, its tooltip saying why; the same in the Monitor | _to fill_ |
| Two DAWs on one machine | REAPER and Live with the plugin loaded: two servers, two `DEVICE_ID`s, each the same after a restart | _to fill_ |
