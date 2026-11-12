# OSCQuery Automation plugin: retest on a46d285

Interoperability test report, 2026-11-12, from Julien Bayle. It follows the retest of `ec6725c` (2026-11-11), which your repository keeps as `docs/Plugin_retest_ec6725c.md`, and uses the same numbering for the defects (2.1 to 2.13). It can be read without that document.

Every number below was measured on 2026-11-12. Each comparison with `ec6725c` uses either both builds run in this session, or the `ec6725c` numbers from 2026-11-11 against the same server files; each table says which. A cause is marked **likely** when the code path fits what was seen but no test isolated it. Code locations are `file:line` at `a46d285`, relative to the repository root.

## Terms used in this report

- **Slot**: one of the plugin's host parameters, bound to one OSCQuery leaf. Its **direction** is `send` (host to server), `receive` (server to host) or `both`.
- **Host value**: the slot's value as the host holds it, normalised 0 to 1.
- **Received value**: the last value the plugin got from the server for a slot (`Instance::received`).
- **Server value**: the value the server holds, as it answers `?VALUE`.
- **Hold**: what `f9b416d` added. For `hostHoldMs` (200 ms) after the host moves a slot, values from the server for that slot are kept aside (`Instance::deferred`) instead of being applied. When the hold ends, if the value kept aside differs from the host value, the host value is sent again.
- **Loop guard**: the check in `src/core/registry/Registry.cpp` that drops a value coming back from the server when it equals what the plugin sent less than 50 ms earlier.
- **LISTEN**: the WebSocket command by which a client asks the server to stream a path's values (OSCQuery proposal, "Optional Bi-Directional Communication").
- **HOST_INFO**: the server's `?HOST_INFO` answer, with `NAME`, `OSC_PORT` and the extensions it supports.
- **ACCESS**: the leaf attribute from the proposal: 1 "the value may only be retrieved", 2 "may only be set", 3 both.
- **DNS-SD**: service discovery over multicast DNS, here the `_oscjson._tcp` services the plugin browses and announces.
- **Message thread**: JUCE's main thread, where the plugin applies received values to host parameters (`PluginProcessor::handleAsyncUpdate`).
- **Burst**: many values sent by the server in one go, with no pause between them.
- **Start**: one fresh process that loads the plugin, restores a state chunk and connects.

## 1. Setup

### What was tested

- Commit **`a46d285`** (`a46d285986e5b7307c72fe0344c27ef446e7e2bd`, 2026-11-11 20:03 +0100), `main` after a fast-forward pull. That is 17 commits after `ec6725c`: 10 commits and 7 merges (pull requests #6, #7, #8, #9). The repository was not modified. It was built outside the tree, the same way as `ec6725c`: CMake, Ninja Multi-Config, `-DOQA_BUILD_TESTS=ON`, Release and Debug.
- macOS 15 (Darwin 24.6), Apple silicon, Xcode 26.3, CMake 3.31.5.
- Formats: VST3, AU, CLAP (five variants) and the Monitor plugin.
- The `ec6725c` build of 2026-11-11 was kept and rerun where this report says "same session".

### Validation, as your CI runs it

| Check | `a46d285` |
|---|---|
| `ctest`, Debug | 8 of 8 pass. In Release, `core.C` and `core.fr_FR` fail on the same `RtCheckTests.cpp` checks as on `ec6725c` (lines 62, 73, 93: `rt::violations()` stays 0). Your CI runs them in Debug only |
| CLAP tests, C locale | 14 cases, 671 assertions, all pass (675 on `ec6725c`; `f9b416d` changed `tests/clap/ClapParamsTests.cpp`) |
| pluginval 1.0.4, strictness 5 | VST3, Monitor VST3 and AU pass. The AU still warns "Current program is -1" |
| `auval -strict` | Passes |
| clap-validator 0.4.1 | 229 tests: 156 passed, 0 failed, 73 skipped, as on `ec6725c` |

The validators ran with DNS-SD off, through `tests/validators.registry.json`, as in your CI.

### The host used instead of a DAW

A headless host built on Spotify's `pedalboard` 0.9.26 (Python), which loads the VST3 or the AU in-process:

- Slots are bound by writing your JSON state chunk into the plugin's saved state, then restoring it.
- Audio blocks of 256 frames at 48 kHz run at real-time pace on a worker thread, so `processBlock` runs as in a DAW.
- The main thread pumps the CoreFoundation run loop, so JUCE's message thread runs. Some scenarios stop pumping it for a set time, to simulate a busy UI.
- The host moves a parameter by setting its value through the host API, as automation would.
- The plugin runs with its default settings: its own server and DNS-SD on.
- The host reports no play head, so the transport never starts. The send of every slot at transport start (`f9b416d`) could not be tested here.

One process per start. Processes ran at nice 0, on mains power, with sleep prevented.

### The four servers

1. **An instrumented test server**: an OSCQuery server in Python. It logs every HTTP request, WebSocket frame and UDP datagram, with time and source port. It can be scripted to:
   - make a crossing certain (send its own value just before applying the client's write);
   - restart on the same HTTP port with the same or a new `OSC_PORT`;
   - keep HTTP connections open;
   - echo writes;
   - change `ACCESS`;
   - stream bursts of values;
   - be announced over DNS-SD, including two servers under one name.
2. **Max** 9.2.1 with the **ossia 2.1.0+prism.12** package: our fork of libossia's Max package. It is the same Release package as for the `ec6725c` report, now installed as a plain copy of the Release archive, identical to it file by file. A patch with `ossia.device` and `ossia.parameter` objects drives the leaves.
3. **TouchDesigner** 2025.33230 with **our `oscquery` 0.4.0** component. These are the same Release `.tox` files as for the `ec6725c` report, with identical SHA-256. A script drives the leaves.
4. **prism-oscquery 0.4.0**: our OSCQuery server and client in pure Python, installed from its Release wheel (checksum verified) under Python 3.11.14. New in this report. It is the code at the core of the TouchDesigner component, run as a plain Python process. A small script drives it with the same commands and leaves as the TouchDesigner one. This server resolves crossed writes on its side, so its crossing numbers are lower by design.

With Max, TouchDesigner and prism-oscquery, a passive TCP and UDP relay between the plugin and the server recorded the traffic. All three declare `CRITICAL` on their leaves, so the plugin writes to TouchDesigner and prism-oscquery over the WebSocket. Max gets UDP.

Because prism-oscquery is new, every scenario against it ran on both builds in this session. For Max and TouchDesigner, the `ec6725c` numbers are from 2026-11-11 with the same server files.

### How a scenario drives the plugin

Each scenario follows the same pattern:

1. Start the server.
2. Start the host process and bind the slots to the server's address.
3. Wait for the connection (`HOST_INFO`, tree, `LISTEN`, `?VALUE`).
4. Act on one side: move a host parameter, or change values on the server.
5. Compare the server value with the host value, and count what crossed the wire.

The scenarios and their repetitions are those of the `ec6725c` report, with two additions:

- a same-name twin that does not serve the bound path (2.5);
- bursts and streams driven by the script below (section 2, N1).

### A script for reproducing without our tools

The appendix contains `oscquery_repro.py`, unchanged since the `ec6725c` report. It is a single-file OSCQuery server: Python 3.8+, standard library only. It has three leaves: `/float` (0.5), `/toggle` (`false`) and `/ro` (read-only, 0.25). Console commands change values, arm crossings, stream, send bursts, restart and change `ACCESS`. It logs everything it receives (`RECEIVED`) and sends (`SENT`).

Every "with the script" number below comes from driving both builds, VST3, against this script in the headless host. Each check ran once per build, in a fresh process, with DNS-SD off.

Run it as `python3 oscquery_repro.py --port 5678`. In the DAW, bind a slot to `/float` on device `repro` at `127.0.0.1:5678`.

## Summary

| Item | `ec6725c` | `a46d285` | Verdict |
|---|---|---|---|
| 2.1 Writes at connection | 29 of 50 starts on the test server; 15 of 17 script checks | 0 of 50; 0 of 17; 0 of 45 on Max, TouchDesigner and prism-oscquery | **Fixed** |
| 2.2 Move back to the received value | not sent; booleans 5 of 10 | sent; booleans 10 of 10 on all four servers | **Fixed** |
| 2.3 Crossed writes | simple crossing: 20, 20, 20 diverged of 20 | 0, 0, 0 | **Fixed for the simple crossing** |
| 2.3 With the message thread stalled 0.7 s | 20, 20, 20 | 20, 19, 20 | **Still present**: see 2.3 below |
| 2.4 New `OSC_PORT`, same HTTP port | 0 of 7 writes, twice; script 0 of 3 | 7 of 7, twice; script 3 of 3 | **Fixed** |
| 2.4 By name, new OSC port | 7 then 4 of 7 | 4 then 4 of 7 | **Unchanged**: see 2.4 below |
| 2.5 Same-name twin | adopted from 0.5 s | twin serving the bound path adopted; twin without it held | **As you decided** |
| 2.6 Host move in the same cycle as an incoming value | VST3 0 of 20, AU 0 of 20 | 20 of 20, 20 of 20 | **Fixed** |
| 2.7 `ACCESS` changed across a restart | ignored, write sent | `unauthorised`, nothing sent | **Fixed** |
| 2.8 Full ring, message thread frozen | last value right 0 of 3 | 3 of 3 | **Fixed** |
| **N1** Burst with the host running | last value right 3 of 3 | 0 of 3; stale values written back to the server | **New, regression**: see N1 |
| 2.9 Keep-alive HTTP server | no connection | no connection | **Still present** (you set it aside) |
| 2.10 No sender port declared | Max echoes 19 of 20 | 19 of 20 | **Still present** (waiting for libossia) |
| 2.11 Server named after the library | "(libOSCQuery-Automation-Core)" | "(Python)", the host process | **Fixed** in the headless host |
| 2.12 Boolean resent at every host move | 11 of 11 steps sent | 2 of 11 on the test server, TouchDesigner, prism-oscquery; **9 of 11 on Max** | **Fixed, except on a server that echoes** |
| 2.13 No `CLIENT_NAME` | none | none | **Still present** (waiting for libossia) |
| Saved address dropped after one refusal | Max 0 of 5, TouchDesigner 0 of 2 | Max 0 of 5; TouchDesigner 2 of 2, prism-oscquery 2 of 2 back through the saved address | **Changed** |
| Process killed by `SIGPIPE` (#9) | not seen | not seen | Not reproducible here |

### What each commit claims, and what the test says

| Commit | Claims | Confirmed by this retest |
|---|---|---|
| `1a57df2` | 2.2, 2.11 | 2.2: yes, on all four servers. 2.11: yes in the headless host, not checked in a DAW |
| `009ac7e` | 2.4, 2.7, 2.8, 2.12 | 2.4: yes, same HTTP port (the by-name case is unchanged). 2.7: yes. 2.8: yes with the message thread frozen, but see N1. 2.12: yes, except against Max |
| `f9b416d` | 2.1, 2.3, 2.5, 2.6 | 2.1: yes. 2.3: yes for the simple crossing, no with a 0.7 s stall. 2.5: yes, both halves of the rule. 2.6: yes, VST3 and AU. The hold it adds is the **likely** source of N1 |
| `c3e7ba9` | `SIGPIPE` on a closed peer | No crash and no crash report in this retest (10 Max reconnections, 2 TouchDesigner, 4 prism-oscquery, the test server's restarts). As on `ec6725c`, this does not prove the fix |
| `550a84f`, `0a5b6cf`, `d5d48c6` | test changes only | not applicable |
| `4357018`, `891647d`, `a46d285` | the `ec6725c` report added, deleted and restored in `docs/` | not applicable |

## 2. What remains and what is new, by impact on a user

### N1 A burst from the server is written back to it, and the stale value then wins on both sides

**Status: new in `a46d285`, worse than `ec6725c`.**

**What happens.**
1. The server sends a burst of values on a leaf bound `both`, for instance a jump of a parameter sent as a ramp, or a dump.
2. While the message thread applies the burst one value at a time, the plugin sends one of the intermediate values back to the server, as if the host had moved.
3. That send starts the hold, so the rest of the burst, the final value included, is kept aside and not applied.
4. When the hold ends 200 ms later, the value kept aside differs from the host value, so the plugin sends the intermediate value to the server again.

Both sides end on a stale intermediate value; the server's last value is lost.

`ec6725c` already wrote intermediate values back during bursts, and the server sometimes ended on one. But the host always ended on the right value, and each stale value went out once. `a46d285` adds the second send and leaves the host on the stale value too.

Streams at a steady rate do not trigger it in these tests (see the last rows of the table below).

**Reproduce with the script.**
1. Start the script. In the DAW, bind a slot to `/float`, direction `both`. Let it connect.
2. Type `burst /float 6000`. The script sends 6000 values in one go; the last is 0.9.
3. Wait two seconds, then type `show`.
4. Repeat steps 2 and 3 two or three times, typing `set /float 0.05` in between.
5. In the failing runs, the log shows `RECEIVED UDP ... /float [x]` right after the burst, then the same `x` again about 200 ms later. `show` prints `/float x` instead of 0.9, and the host shows `x`.

A burst of 2000 values, smaller than the incoming ring of 4096, shows it too: `burst /float 2000`.

**Expected.** The host and the server end on the burst's last value, 0.9. Nothing goes back to the server, since the host did not move.

**Observed.**

| Case, 3 bursts each | `ec6725c` | `a46d285` |
|---|---|---|
| Test server, 6000 values, host running: host ends on 0.9 | 3 of 3 | **0 of 3** (0.296, 0.705, 0.549) |
| Same: values the plugin wrote back to the server | 2 | **9** (3 per burst: 2 at the end of the burst, then 1 again 156 to 192 ms later) |
| Script, 6000 values: server ends on 0.9 / host ends on 0.9 | 1 of 3 / 3 of 3 | 1 of 3 / **1 of 3** |
| Script, 4000 values: server / host | 3 of 3 / 3 of 3 | 2 of 3 / 2 of 3 |
| Script, 2000 values: server / host | 2 of 3 / 3 of 3 | 2 of 3 / 2 of 3 |
| Script, steady streams of 120 values at 56 per second, 400 at 157 per second, 1000 at 643 per second, with no host move: values written back | 0 | 0 |
| Script, a stream of 200 values at 50 per second with 10 host moves during it: server values written back as host moves | 0 | 2 (0.3869 and 0.593) |
| Test server, 6000 values, message thread frozen during the burst (2.8): host ends on 0.9 | 0 of 3 | 3 of 3 |

The first two rows were measured in the same session for both builds, and so were the script rows.

**Proposal.** `LISTEN` streams "all OSC messages sent to that method" to the client. Nothing lets a client send back a value it received as if it were its own.

**Code and likely cause.**
- `src/shims/juce/PluginProcessor.cpp:237-257`, `handleAsyncUpdate`: the message thread drains the received values and calls `setValueNotifyingHost` for each one in turn.
- `src/core/Instance.cpp:1000-1005`, `processBlock`: the audio thread posts the host value unless it equals `received[i]`. During a burst, the parameter holds a value applied a moment ago while `received[i]` already holds a newer one. The parameter's value is the server's own, but the check doesn't recognise it and posts it. That this race was already there on `ec6725c` fits its 2 stale writes.
- `src/core/Instance.cpp:1007-1013`: the post counts as a host move and starts the hold. `1022` then clears `received[i]`.
- `src/core/Instance.cpp:534-546`, `pollReceived`: during the hold, the rest of the burst goes to `deferred[slot]` and is not applied.
- `src/core/Instance.cpp:970-988`: when the hold ends, the kept value (0.9) differs from the host value, so the host value is posted again at 986.

**Likely**, not isolated. The 2 server values written back during a 50 per second stream fit the same race.

### 2.3 Crossed writes with a busy message thread still diverge

**Status: fixed for the simple crossing; still present when the message thread is stalled past the hold.**

**What happens.** Your fix converges a crossing when the server's value arrives within 200 ms of the host's move. If the message thread is busy for longer (a modal dialog, a heavy redraw), the crossing value is applied after the hold has ended. The host then shows the server's crossing value while the server holds the host's. The two sides swap, and stay apart until the next change.

**Reproduce with the script.** Simple crossing:

1. Bind `/float`, direction `both`, and type `cross /float 0.2 answer`.
2. Move the host to 0.6.
3. The script logs the received 0.6, sends 0.2 and then 0.6. With `a46d285`, about 200 ms later it logs `RECEIVED ... [0.6]` again. Both sides end on 0.6: 3 of 3, against 0 of 3 on `ec6725c`.

The stalled case needs the message thread to stall for more than 200 ms right after the move. In a DAW, opening a modal dialog right after a move would do it; the script cannot.

**Expected.** Both sides end on the same value.

**Observed** (diverged out of 20; the test server, Max and TouchDesigner rows for `ec6725c` are from 2026-11-11):

| Case | `ec6725c`: test server / Max / TouchDesigner / prism-oscquery | `a46d285` |
|---|---|---|
| Simple crossing | 20 / 20 / 20 / 2 | **0 / 0 / 0 / 0** |
| Message thread stalled 0.7 s | 20 / 20 / 20 / 8 | 20 / 19 / 20 / 5 |
| Everything stalled 0.7 s | 20 / 19 / 20 / 6 | 20 / 19 / 20 / 3 |

In every diverged row of `a46d285`, the host holds the server's crossing value and the server holds the host's. No `?VALUE` reread was seen.

**Proposal.** No rule on arbitration.

**Code and likely cause.** `src/core/Instance.cpp:534`: the hold is measured from the host's move, on a clock that runs while the message thread is stalled. A value that is applied late, however late, is treated as the newest. **Likely.** One option: when the hold has run out but the value arrives with a move still unacknowledged, ask the server with `?VALUE` before applying it.

### 2.4 After a reconnection by name to a new OSC port, the first moves are lost

**Status: the same-HTTP-port case is fixed; the by-name case is unchanged.**

**What happens.** The server goes away and comes back under the same DNS-SD name, on a new HTTP port and a new OSC port. The plugin finds it and sends `LISTEN`. The host's moves in the next 2 to 3 seconds are dropped. One arrives late; later ones arrive.

**Reproduce.** The script's `restart` keeps the HTTP port, which is the case now fixed. The script does not announce itself over DNS-SD, so it cannot show the by-name case. It needs a server announced under a fixed name that can be restarted on new HTTP and OSC ports:

1. Start the announced server.
2. In the DAW, bind a slot to one of its leaves, direction `both`, through DNS-SD discovery.
3. Stop the server, then start it again under the same name with both ports changed.
4. Move the host 0.2, 0.5 and 1 s after the plugin's `LISTEN` reaches the server, then twice more a few seconds later.
5. The server receives the later moves but not the first two.

**Observed** (writes received out of 7 moves made 0.2, 0.5, 1, 2, 3, 6 and 10 s after the reconnection, test server, two runs each, same session):

| Case | `ec6725c` | `a46d285` |
|---|---|---|
| Same HTTP port, new OSC port | 0, 0 | **7, 7** |
| Same HTTP port, same OSC port (control) | 7 | 7 |
| By name, new HTTP and OSC ports | 7, 4 | 4, 4 |
| With the script, `restart new-osc-port` | 0 of 3 | **3 of 3** |

In both `a46d285` runs, the moves at 0.2 and 0.5 s never reach the server. The move at 1 s arrives 0.6 to 0.7 s late. `HOST_INFO` is requested 10 times after the reconnection, against 7 on `ec6725c`.

**Code and likely cause.** `src/core/registry/Registry.cpp:2247-2250`: while `senderReady` is false, the moves due are counted in `unsent` and dropped, not kept for later. After a move by name the sender is reopened from the new `HOST_INFO`, and moves made in between are lost. **Likely**, not isolated.

### 2.12 Against a server that echoes, a boolean is still resent at most host moves

**Status: fixed on servers that do not echo; still present against Max.**

**What happens.** A boolean slot is now sent once per state change, as long as the server does not send the value back. Max echoes every UDP write (2.10). Each echo counts as the device speaking, so the plugin forgets what it last sent, and the next host move sends the state again.

**Observed** (11 host moves crossing the 0.5 threshold: 0.2, 0.4, 0.49, 0.5, 0.51, 0.9, 1.0, 0.6, 0.5, 0.49, 0.1; how many were sent):

| Server | `ec6725c` | `a46d285` |
|---|---|---|
| Test server | 11 | 2 |
| TouchDesigner | 11 | 2 |
| prism-oscquery | 11 | 2 |
| Max | 10 | 9 |

**Code and cause.** `src/core/registry/Registry.cpp:1509` resets `toggleSent` on every incoming value, an echo of the state just sent included. Resetting only when the incoming state differs from `toggleSent[i]` would keep the fix against echoing servers. Clear from the code; not tested with a patch.

### 2.5 Same-name device: the rule now checks the bound paths

**Status: behaves as you decided.**

**Observed** (test server, two runs each, `a46d285`):

- A twin that serves the bound path with the same `TYPE`, plus one extra leaf, is adopted. It receives 5 of 5 writes, from 0.5 s after the first server goes away, as on `ec6725c`. This is your stated choice, so it is not counted as a defect.
- A twin that does not serve the bound path (new scenario) is not adopted. It gets no `LISTEN` and 0 of 5 writes. Its `HOST_INFO` and tree are read again about every 4 s.

Not tested: the held twin starting to serve the path later and being adopted by itself.

### 2.9 A server that keeps its HTTP connection open prevents any connection

**Status: still present, unchanged.** Six `HOST_INFO` attempts in 30 s (0, 3.5, 7.5, 12.5, 19.5, 30.6 s), no connection, on both builds. With the script: `python3 oscquery_repro.py --port 5678 --keep-alive`. RFC 9112 §9.6: without `Connection: close` the server may keep the connection open. `lib/oscquery/oscquery/net/HttpGet.cpp:154-170` reads until the server closes. You set this aside as low priority.

### 2.10 No sender port is declared, so libossia-based servers echo every UDP write

**Status: still present, unchanged.** No `START_OSC_STREAMING` on either build. Max echoes 19 of 20 writes on both builds. The test server set to echo sends 20 of 20 back; the plugin does not resend them, so there is no loop. TouchDesigner and prism-oscquery echo nothing. You are waiting for the libossia update; noted. This echo is also what keeps 2.12 open against Max.

### 2.13 Servers that list their clients show the plugin without a name

**Status: still present, unchanged.** The plugin's only WebSocket command is `LISTEN`; it sends no `CLIENT_NAME`. Max, TouchDesigner and prism-oscquery list it by IP and port only. Waiting, as you said.

### N2 Each host move during a server stream is sent twice

**Status: new, follows from the hold as designed. Low impact.**

During a server stream, each host move is sent at once and again when the hold ends 200 ms later, because the stream has moved the server elsewhere in the meantime. With the script (`stream /float 200 20`, 10 host moves during it), `a46d285` sent 8 of the 10 moves twice and 2 once. `ec6725c` sent 9 of 10, once each. The host's value briefly wins over the stream each time. That fits "the host has the word while it moves a slot", so this is noted, not counted as a defect.

## 3. What changed since `ec6725c`, and what still works

### Reconnection and the saved address

After the server restarts, the plugin can come back through the saved address (here, the relay that records traffic) or through the address announced over DNS-SD.

| Server | `ec6725c` | `a46d285` |
|---|---|---|
| Max, through the saved address | 0 of 5; joins the announced address | 0 of 5; joins the announced address (Max lists the plugin at the LAN address) |
| Max, direct | 3.1 s, 5 of 5 | 3.1 to 4.1 s, 5 of 5 |
| TouchDesigner, through the saved address | 0 of 2, 13.3 s through the announced address | **2 of 2 through the saved address**, 15.8 s |
| prism-oscquery, through the saved address | 0 of 2, 1.9 s through the announced address | **2 of 2 through the saved address**, 1.8 s |
| Test server, same port | 1.8 s | 1.8 s |
| Test server, by name | 2.1 s | 2.1 s |

Your status section lists the dropped saved address as "not addressed". Against TouchDesigner and prism-oscquery, the plugin now comes back through the saved address. Against Max it still does not. Max's announcement returns within about a second of the patch reopening. TouchDesigner takes about 15 s to come back, and prism-oscquery withdraws its announcement on close. That may explain the difference. Not isolated.

Also better: a host move 1 s after a same-port reconnection was not sent by `ec6725c` (0 of 1) and is sent by `a46d285` (1 of 1).

### The plugin's own server name

`File::hostApplicationPath` now names the process. In the headless host, the plugin's server is announced as "OSCQuery Automation @ \<machine\> (Python)", the host process. On `ec6725c` it was "(libOSCQuery-Automation-Core)". In a DAW it should read the DAW's name; not checked.

### Still good on both builds

- `?HOST_INFO`, the tree, then `?VALUE` after each `LISTEN`.
- Chunked replies.
- One bundle per block, with only the changed slots (4 of 8).
- Timetags 99 ms ahead for a 100 ms latency.
- Splitting at the MTU (1360 and 720 bytes).
- Received values: 10 of 10 booleans on all four servers.
- All boolean forms read right: `true`, `false`, `1`, `0`, `"true"`, `TYPE` `F`; `null` and an absent value leave the preset.
- A read-only leaf bound `receive` follows the server 5 of 5 without sending.
- `unauthorised` with "ACCESS 1: read-only for this client" for `both` and `send`.
- Max, TouchDesigner and prism-oscquery refuse a direct OSC write to a read-only leaf.
- Writes served in 7 to 15 ms by Max and prism-oscquery, 33 ms by TouchDesigner.

## 4. What was not tested

- A real DAW. In particular:
  - the send of every slot when the transport starts;
  - the session load;
  - the editor and the Monitor plugin in use;
  - 2.11 in a DAW;
  - 2.6 with a DAW's own parameter cache.
- The CLAP with bound slots.
- Windows and Linux.
- `ACCESS` 2, `DEVICE_ID` and `CLOCK`. None of the four servers declares them.
- A held same-name twin that later serves the bound path.
- Writes from a client to the plugin's own server.
- The stalled crossing (2.3) with the script. The script cannot stall the DAW's message thread.

## Appendix: `oscquery_repro.py`

Unchanged since the `ec6725c` report. Save it as `oscquery_repro.py`.

```python
#!/usr/bin/env python3
"""A minimal OSCQuery server for reproducing client defects by hand.

Python 3.8+, standard library only. One HTTP + WebSocket port, one UDP port for OSC.
It logs, with a timestamp, every HTTP request, every WebSocket command and every OSC
message it receives (UDP or WebSocket), and every value it sends.

Leaves:
  /float   TYPE f, ACCESS 3, RANGE 0..1, VALUE 0.5
  /toggle  TYPE T, ACCESS 3, VALUE false
  /ro      TYPE f, ACCESS 1 (read only), RANGE 0..1, VALUE 0.25

Start:   python3 oscquery_repro.py [--port 5678] [--name repro] [--echo] [--keep-alive] [--extra-leaf]
Then type commands on stdin:
  set PATH VALUE          change a leaf on the server and send it to the clients that LISTEN
  stream PATH N MS        send N values (0..1 ramp) to listeners, one every MS milliseconds
  burst PATH N            send N values to listeners in one go, the last one is 0.9
  cross PATH VALUE [answer]
                          arm a crossing: when the next write to PATH arrives, the server first
                          sends VALUE to the listeners (as if it had changed at the same moment),
                          then applies the client's write; with "answer", it then sends its final
                          value back 5 ms later, as an arbitrating server (Max, TouchDesigner) does
  access PATH N           change ACCESS of a leaf (takes effect for the next tree read)
  restart [new-osc-port]  stop everything, wait 1 s, serve again on the same HTTP port,
                          on the same UDP port unless "new-osc-port"
  show                    print the server's values
  quit

Options:
  --echo        every write received is sent back to the listeners of that path, as an
                upstream libossia server does for a UDP write from an undeclared port
  --keep-alive  ignore "Connection: close": answer with Content-Length and keep the socket open
  --extra-leaf  add /extra, so the tree (and any hash over paths and types) differs
"""
import argparse, asyncio, base64, hashlib, json, socket, struct, sys, threading, time

T0 = time.time()


def log(*parts):
    print('%8.3f' % (time.time() - T0), *parts, flush=True)


# -- OSC ----------------------------------------------------------------------

def _pad(b):
    return b + b'\0' * (4 - len(b) % 4)


def osc(address, value):
    if value is True or value is False:
        return _pad(address.encode()) + _pad((',T' if value else ',F').encode())
    return _pad(address.encode()) + _pad(b',f') + struct.pack('>f', float(value))


def _string(d, i):
    end = d.index(b'\0', i)
    return d[i:end].decode(), (end // 4 + 1) * 4


def parse(d):
    """[(address, [args])] for a message or a bundle."""
    if d[:8] == b'#bundle\0':
        out, i = [], 16
        while i < len(d):
            n = struct.unpack('>i', d[i:i + 4])[0]
            out += parse(d[i + 4:i + 4 + n])
            i += 4 + n
        return out
    address, i = _string(d, 0)
    tags, i = _string(d, i)
    args = []
    for t in tags[1:]:
        if t == 'f':
            args.append(round(struct.unpack('>f', d[i:i + 4])[0], 6)); i += 4
        elif t == 'i':
            args.append(struct.unpack('>i', d[i:i + 4])[0]); i += 4
        elif t in 'TF':
            args.append(t == 'T')
        elif t == 's':
            s, i = _string(d, i); args.append(s)
    return [(address, args)]


# -- server -------------------------------------------------------------------

class Server:
    def __init__(self, a):
        self.a = a
        self.leaves = {
            '/float': {'TYPE': 'f', 'ACCESS': 3, 'VALUE': 0.5, 'RANGE': True},
            '/toggle': {'TYPE': 'T', 'ACCESS': 3, 'VALUE': False, 'RANGE': False},
            '/ro': {'TYPE': 'f', 'ACCESS': 1, 'VALUE': 0.25, 'RANGE': True},
        }
        if a.extra_leaf:
            self.leaves['/extra'] = {'TYPE': 'f', 'ACCESS': 3, 'VALUE': 0.0, 'RANGE': True}
        self.clients = []          # [(writer, set of LISTENed paths)]
        self.crossings = {}        # path -> (value, answer)
        self.loop = asyncio.new_event_loop()
        self.osc_port = 0

    # values
    def node(self, path, leaf):
        n = {'FULL_PATH': path, 'TYPE': leaf['TYPE'], 'ACCESS': leaf['ACCESS']}
        if leaf['ACCESS'] & 1:
            n['VALUE'] = [leaf['VALUE']]
        if leaf['RANGE']:
            n['RANGE'] = [{'MIN': 0.0, 'MAX': 1.0}]
        return n

    def tree(self):
        return {'FULL_PATH': '/', 'ACCESS': 0,
                'CONTENTS': {p.strip('/'): self.node(p, l) for p, l in self.leaves.items()}}

    def host_info(self):
        return {'NAME': self.a.name, 'OSC_PORT': self.osc_port, 'OSC_TRANSPORT': 'UDP',
                'EXTENSIONS': {'ACCESS': True, 'VALUE': True, 'RANGE': True, 'LISTEN': True,
                               'PATH_CHANGED': False}}

    def send(self, path, value, why):
        data = osc(path, value)
        head = bytes([0x82, len(data)]) if len(data) < 126 else bytes([0x82, 126]) + struct.pack('>H', len(data))
        n = 0
        for w, listening in self.clients:
            if path in listening:
                w.write(head + data); n += 1
        log('SENT    ', path, value, '(%s, to %d listener%s)' % (why, n, '' if n == 1 else 's'))

    def received(self, via, sender, path, args):
        log('RECEIVED', via, 'from port', sender, path, args)
        leaf = self.leaves.get(path)
        if leaf is None or not args:
            return
        if not leaf['ACCESS'] & 2:
            log('         (refused: ACCESS %d)' % leaf['ACCESS'])
            return
        if path in self.crossings:
            value, answer = self.crossings.pop(path)
            leaf['VALUE'] = value
            self.send(path, value, 'crossing: the server changed at the same moment')
            leaf['VALUE'] = args[0]
            if answer:
                self.loop.call_later(0.005, lambda: self.send(path, leaf['VALUE'], 'answer: the server\'s final value'))
            return
        leaf['VALUE'] = args[0]
        if self.a.echo:
            self.send(path, args[0], 'echo')

    # network
    async def start(self, osc_port):
        self.http = await asyncio.start_server(self.conn, '0.0.0.0', self.a.port, reuse_address=True)
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('0.0.0.0', osc_port))
        self.udp.setblocking(False)
        self.osc_port = self.udp.getsockname()[1]
        self.loop.add_reader(self.udp.fileno(), self.on_udp)
        log('SERVING  name %r, HTTP and WebSocket on port %d, OSC (UDP) on port %d'
            % (self.a.name, self.a.port, self.osc_port))

    async def stop(self):
        self.loop.remove_reader(self.udp.fileno())
        self.udp.close()
        self.http.close()
        for w, _ in list(self.clients):
            w.close()
        self.clients.clear()
        await self.http.wait_closed()
        log('STOPPED')

    def on_udp(self):
        while True:
            try:
                data, addr = self.udp.recvfrom(65536)
            except (BlockingIOError, OSError):
                return
            for path, args in parse(data):
                self.received('UDP', addr[1], path, args)

    async def conn(self, reader, writer):
        peer = writer.get_extra_info('peername')[1]
        try:
            while True:
                head = (await reader.readuntil(b'\r\n\r\n')).decode('latin1').split('\r\n')
                headers = {k.strip().lower(): v.strip() for k, _, v in (l.partition(':') for l in head[1:] if ':' in l)}
                log('HTTP    ', head[0], '(from port %d, Connection: %s)' % (peer, headers.get('connection', '-')))
                if 'websocket' in headers.get('upgrade', '').lower():
                    await self.websocket(reader, writer, headers, peer)
                    return
                target = head[0].split(' ')[1]
                path, _, query = target.partition('?')
                if query == 'HOST_INFO':
                    body, status = self.host_info(), '200 OK'
                elif query == 'VALUE' and path in self.leaves:
                    leaf = self.leaves[path]
                    body, status = ({'VALUE': [leaf['VALUE']]}, '200 OK') if leaf['ACCESS'] & 1 else (None, '204 No Content')
                elif path in ('', '/'):
                    body, status = self.tree(), '200 OK'
                elif path in self.leaves:
                    body, status = self.node(path, self.leaves[path]), '200 OK'
                else:
                    body, status = None, '404 Not Found'
                data = b'' if body is None else json.dumps(body).encode()
                close = '' if self.a.keep_alive else 'Connection: close\r\n'
                writer.write(('HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n%s\r\n'
                              % (status, len(data), close)).encode() + data)
                await writer.drain()
                if not self.a.keep_alive:
                    return
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()

    async def websocket(self, reader, writer, headers, peer):
        key = headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
        writer.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                      'Sec-WebSocket-Accept: %s\r\n\r\n'
                      % base64.b64encode(hashlib.sha1(key.encode()).digest()).decode()).encode())
        entry = (writer, set())
        self.clients.append(entry)
        log('WS OPEN  from port', peer)
        try:
            while True:
                b0, b1 = await reader.readexactly(2)
                op, n = b0 & 0x0f, b1 & 0x7f
                if n == 126:
                    n = struct.unpack('>H', await reader.readexactly(2))[0]
                elif n == 127:
                    n = struct.unpack('>Q', await reader.readexactly(8))[0]
                mask = await reader.readexactly(4) if b1 & 0x80 else b'\0\0\0\0'
                data = bytes(c ^ mask[i % 4] for i, c in enumerate(await reader.readexactly(n)))
                if op == 8:
                    return
                if op == 9:
                    writer.write(bytes([0x8a, len(data)]) + data)
                elif op == 1:
                    msg = json.loads(data.decode())
                    log('WS TEXT ', json.dumps(msg))
                    if msg.get('COMMAND') == 'LISTEN':
                        entry[1].add(msg.get('DATA'))
                    elif msg.get('COMMAND') == 'IGNORE':
                        entry[1].discard(msg.get('DATA'))
                elif op == 2:
                    for path, args in parse(data):
                        self.received('WS ', peer, path, args)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            if entry in self.clients:
                self.clients.remove(entry)
            log('WS CLOSE from port', peer)

    # console
    async def command(self, line):
        w = line.split()
        if not w:
            return
        c = w[0]
        try:
            if c == 'set':
                v = parse_value(w[2])
                self.leaves[w[1]]['VALUE'] = v
                self.send(w[1], v, 'set')
            elif c == 'stream':
                n, ms = int(w[2]), float(w[3])
                for k in range(n):
                    v = round(k / max(1, n - 1), 4)
                    self.leaves[w[1]]['VALUE'] = v
                    self.send(w[1], v, 'stream %d/%d' % (k + 1, n))
                    await asyncio.sleep(ms / 1000)
            elif c == 'burst':
                n = int(w[2])
                values = [round(0.9 * k / (n - 1), 6) for k in range(n)]
                self.leaves[w[1]]['VALUE'] = values[-1]
                data = [osc(w[1], v) for v in values]
                for ww, listening in self.clients:
                    if w[1] in listening:
                        for d in data:
                            ww.write(bytes([0x82, len(d)]) + d)
                log('SENT    ', w[1], '%d values in one go, the last is %s' % (n, values[-1]))
            elif c == 'cross':
                self.crossings[w[1]] = (parse_value(w[2]), len(w) > 3 and w[3] == 'answer')
                log('ARMED   ', 'crossing on', w[1])
            elif c == 'access':
                self.leaves[w[1]]['ACCESS'] = int(w[2])
                log('ACCESS  ', w[1], w[2])
            elif c == 'restart':
                await self.stop()
                await asyncio.sleep(1.0)
                await self.start(0 if len(w) > 1 and w[1] == 'new-osc-port' else self.osc_port)
            elif c == 'show':
                for p, l in self.leaves.items():
                    log('VALUE   ', p, l['VALUE'], 'ACCESS', l['ACCESS'])
            elif c == 'quit':
                self.loop.stop()
            else:
                log('?', line)
        except (KeyError, IndexError, ValueError) as e:
            log('?', line, repr(e))


def parse_value(text):
    if text.lower() in ('true', 'false'):
        return text.lower() == 'true'
    return float(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', type=int, default=5678)
    p.add_argument('--osc-port', type=int, default=0)
    p.add_argument('--name', default='repro')
    p.add_argument('--echo', action='store_true')
    p.add_argument('--keep-alive', action='store_true')
    p.add_argument('--extra-leaf', action='store_true')
    s = Server(p.parse_args())
    asyncio.set_event_loop(s.loop)
    s.loop.run_until_complete(s.start(s.a.osc_port))

    def console():
        for line in sys.stdin:
            asyncio.run_coroutine_threadsafe(s.command(line.strip()), s.loop)
        s.loop.call_soon_threadsafe(s.loop.stop)

    threading.Thread(target=console, daemon=True).start()
    s.loop.run_forever()


if __name__ == '__main__':
    main()
```
