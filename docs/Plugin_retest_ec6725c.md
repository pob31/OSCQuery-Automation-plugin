# OSCQuery Automation plugin: retest on ec6725c

Interoperability test report, 2026-11-11, from Julien Bayle. Written to be read without any other document. Every number below was measured. A cause is marked **likely** when the code path fits what was seen but was not isolated by a test. Code locations are `file:line` at `ec6725c`, relative to the repository root.

The whole report, script included, ready to paste into a Claude Code session.

Contents

1. [1. Setup](#1-setup)
   1. [What was tested](#what-was-tested)
   2. [Validation, as your CI runs it](#validation-as-your-ci-runs-it)
   3. [The host used instead of a DAW](#the-host-used-instead-of-a-daw)
   4. [The three servers](#the-three-servers)
   5. [How a scenario drives the plugin](#how-a-scenario-drives-the-plugin)
   6. [A script for reproducing without our tools](#a-script-for-reproducing-without-our-tools)
2. [2. Defects, by impact on a user](#2-defects-by-impact-on-a-user)
   1. [2.1 Connecting overwrites the server's value with the host's value, read-only leaves included](#2-1-connecting-overwrites-the-server-s-value-with-the-host-s)
   2. [2.2 A host change back to the last received value is never sent, so a boolean cannot be switched off](#2-2-a-host-change-back-to-the-last-received-value-is-never-s)
   3. [2.3 Crossed writes stay diverged: the server's final value is dropped as an echo](#2-3-crossed-writes-stay-diverged-the-server-s-final-value-is)
   4. [2.4 A server restarted on the same HTTP port with a new OSC port loses every write](#2-4-a-server-restarted-on-the-same-http-port-with-a-new-osc-)
   5. [2.5 A device with the same name is adopted although its tree hash differs](#2-5-a-device-with-the-same-name-is-adopted-although-its-tree)
   6. [2.6 A host change made in the same cycle as an incoming value is never sent](#2-6-a-host-change-made-in-the-same-cycle-as-an-incoming-valu)
   7. [2.7 An ACCESS change across a server restart is ignored](#2-7-an-access-change-across-a-server-restart-is-ignored)
   8. [2.8 When the incoming queue is full, the newest value is lost](#2-8-when-the-incoming-queue-is-full-the-newest-value-is-lost)
   9. [2.9 A server that keeps its HTTP connection open prevents any connection](#2-9-a-server-that-keeps-its-http-connection-open-prevents-an)
   10. [2.10 No sender port is declared, so libossia-based servers echo every UDP write](#2-10-no-sender-port-is-declared-so-libossia-based-servers-ec)
   11. [2.11 The plugin's own server is announced under the library's name, not the host's](#2-11-the-plugin-s-own-server-is-announced-under-the-library-)
   12. [2.12 A boolean is resent at every host change, even when its state does not change](#2-12-a-boolean-is-resent-at-every-host-change-even-when-its-)
   13. [2.13 Servers that list their clients show the plugin without a name](#2-13-servers-that-list-their-clients-show-the-plugin-without)
3. [3. What changed since the earlier reports, and what is new](#3-what-changed-since-the-earlier-reports-and-what-is-new)
   1. [Reconnection now goes through DNS-SD](#reconnection-now-goes-through-dns-sd)
   2. [The plugin's own OSCQuery server, on by default](#the-plugin-s-own-oscquery-server-on-by-default)
   3. [The WebSocket crash](#the-websocket-crash)
   4. [Server side](#server-side)
   5. [Still good](#still-good)
4. [4. What was not tested](#4-what-was-not-tested)
5. [5. Status on 2026-11-11: what was fixed, what is still to do](#5-status-on-2026-11-11-what-was-fixed-what-is-still-to-do)
6. [Appendix: `oscquery_repro.py`](#appendix-oscquery-repro-py)

## Terms used in this report

- **Slot**: one of the plugin's host parameters, bound to one OSCQuery leaf. Its **direction** is `send` (host to server), `receive` (server to host) or `both`.
- **Host value**: the slot's value as the host (the DAW) holds it, normalised 0 to 1.
- **Received value**: the last value the plugin got from the server for that slot, kept per slot in `Instance::received`.
- **Server value**: the value the server holds, as it answers `?VALUE`.
- **Loop guard**: the check in `src/core/registry/Registry.cpp` that drops a value coming back from the server when it equals what the plugin sent less than `loop_guard_ms` (50 ms) earlier.
- **LISTEN**: the WebSocket command by which a client asks the server to stream a path's values (OSCQuery proposal, "Optional Bi-Directional Communication").
- **HOST_INFO**: the server's `?HOST_INFO` answer, with `NAME`, `OSC_PORT` and the extensions it supports.
- **ACCESS**: the leaf attribute from the proposal: 1 "the value may only be retrieved", 2 "may only be set", 3 both.
- **DNS-SD**: service discovery over multicast DNS, here the `_oscjson._tcp` services the plugin browses and, since Phase 6b-2, announces.
- **State chunk**: the JSON the plugin saves in the host's project: devices (name, last address, `tree_hash`) and slots.
- **Message thread**: JUCE's main thread, where the plugin applies received values to host parameters (`PluginProcessor::handleAsyncUpdate`).
- **Start**: one fresh process that loads the plugin, restores a state chunk and connects.

## 1. Setup

### What was tested

- Commit **`ec6725c`** (`ec6725cfa5fd98f438d64c5ee6f27d1ddd0baca7`, 2026-11-11 12:59 +0100), `main` after a fast-forward pull. The repository was not modified. It was built outside the tree.
- macOS 15 (Darwin 24.6), Apple silicon, Release and Debug.
- Formats: VST3, AU, CLAP (five variants) and the Monitor plugin.
- The earlier measurements were on `0d8daab` (2026-11-09). Every "before" number below is from `0d8daab`.

### Validation, as your CI runs it

| Check | Result |
| --- | --- |
| `ctest`, Debug | 8 of 8 pass. In Release, `core.C` and `core.fr_FR` (RtCheckTests) fail; your CI runs them in Debug only, and the allocation they count is probably optimised away. Not treated as a plugin defect |
| CLAP tests, C locale | 14 cases, 675 assertions, all pass |
| pluginval 1.0.4, strictness 5 | VST3, Monitor VST3 and AU pass. The AU gives the same warning as before, "Current program is -1" |
| `auval -strict` | Passes |
| clap-validator 0.4.1 | 229 tests: 156 passed, 0 failed, 73 skipped. Each of the five variants passes 27, as on `0d8daab`. Monitor passes 17 and skips 20; it has no parameters |

The validators ran with DNS-SD off, through `tests/validators.registry.json`, as in your CI.

### The host used instead of a DAW

A headless host built on Spotify's `pedalboard` 0.9.26 (Python), which loads the VST3 or the AU in-process:

- Slots are bound by writing your JSON state chunk into the plugin's saved state, then restoring it.
- Audio blocks of 256 frames at 48 kHz run at real-time pace on a worker thread, so `processBlock` runs as in a DAW.
- The main thread pumps the CoreFoundation run loop, so JUCE's message thread runs. Some scenarios stop pumping it for a set time to simulate a busy UI.
- The host moves a parameter by setting its value through the host API, as automation would.
- The plugin runs with its default settings: its own server and DNS-SD on.

One process per start. Processes ran at nice 0, on mains power, with sleep prevented.

### The three servers

1. **An instrumented test server**: an OSCQuery server in Python. It logs every HTTP request, WebSocket frame and UDP datagram, with time, source port, bundle and timetag. It can be scripted to:
   - make a crossing certain (send its own value just before applying the client's write);
   - restart on the same HTTP port with the same or a new `OSC_PORT`;
   - keep HTTP connections open;
   - echo writes;
   - change `ACCESS`;
   - stream bursts of values;
   - be announced over DNS-SD.
2. **Max** 9.2.1 with the **ossia 2.1.0+prism.12** package. This is our fork of libossia's Max package, from its Release archive, checksums verified. A patch with `ossia.device` and `ossia.parameter` objects drives the leaves.
3. **TouchDesigner** 2025.33230 with **our `oscquery` 0.4.0** component, from its Release `.tox` files, checksums verified. A script drives the leaves.

With Max and TouchDesigner, a passive TCP and UDP relay between the plugin and the server recorded the traffic.

### How a scenario drives the plugin

Each scenario follows the same pattern:

1. Start the server.
2. Start the host process and bind the slots to the server's address.
3. Wait for the connection (`HOST_INFO`, tree, `LISTEN`, `?VALUE`).
4. Act on one side: move a host parameter, or change a value on the server.
5. Compare the server value with the host value, and count what crossed the wire.

Each count below is over the number of trials shown, one start per trial unless stated.

### A script for reproducing without our tools

The appendix contains `oscquery_repro.py`: a single-file OSCQuery server, Python 3.8+, standard library only. It has three leaves (`/float`, `/toggle`, read-only `/ro`) and console commands to change values, arm crossings, stream, restart and change `ACCESS`. It logs everything it receives and sends. Every "with the script" result below was checked by driving the `ec6725c` VST3 against this script in the headless host, one fresh process per check.

Run it as `python3 oscquery_repro.py --port 5678`. In the DAW, bind a slot to `/float` on device `repro` at `127.0.0.1:5678`.

## 2. Defects, by impact on a user

### 2.1 Connecting overwrites the server's value with the host's value, read-only leaves included

Still present, and more frequent than on `0d8daab`

**What happens.** At connection, before the plugin has read the server's value, it sends the host's current value. The server value is lost, and the `?VALUE` that follows reads back the host's value. In a DAW, opening a project would push the project's saved values, or the defaults, onto the server. For a leaf with `ACCESS` 1, the write still goes out, even when the slot is then marked "ACCESS 1: read-only for this client". A server that does not protect its read-only leaves takes it.

**Reproduce with the script.**

1. Start the script. `/float` holds 0.5.
2. In the DAW, bind a slot to `/float`, direction `both`, with its host value at 0 (or anything other than 0.5).
3. Load the plugin, or reload the project.
4. The log shows `RECEIVED UDP ... /float [0.0]` right after `GET /` and before `LISTEN` and `GET /float?VALUE`. `show` prints `/float 0.0`.
5. For a read-only leaf, bind the slot to `/ro` with direction `both`. The script logs the write, then `(refused: ACCESS 1)`.

**Expected.** Nothing is sent before the server value has been read, and nothing is ever sent to a leaf whose `ACCESS` does not include 2.

**Observed.**

| Server, leaf | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| Test server, boolean, 10 starts | 0 | 5 |
| Test server, boolean and float in one device, 10 starts | 1 | 9 |
| Test server, read-only leaf bound `both`, 10 starts | 3 | 7 |
| Test server, read-only leaf bound `send`, 10 starts | 4 | 8 |
| Test server, read-only leaf bound `receive`, 10 starts | 0 | 0 |
| With the script, 10 starts | not run | 9 (the tenth sent nothing) |
| Max, 15 starts | 0 | 0 |
| TouchDesigner, 15 starts | 0 | 0 |

The `0d8daab` and `ec6725c` rows for the test server were measured in the same session, alternating builds, to rule out day-to-day variation. Max and TouchDesigner both answer HTTP more slowly than the Python servers, which fits a race.

**Proposal.** `ACCESS` 1: "the value may only be retrieved". A client that writes it, even once, breaks that contract.

**Code and likely cause.**

- `src/core/Instance.h:301`: `postEverything` starts `true`.
- `src/core/Instance.cpp:572`: `Instance::push()` sets it again after every bindings push.
- `src/core/Instance.cpp:885-904`: with `everything` set, `processBlock` posts every slot's host value. The received-value check at 899-902 is skipped (`! everything`).
- `src/core/registry/Registry.cpp:1911-1936` (`drain`, line 1933): those posts wait in the ring, then mark their group dirty once the plans exist, that is once the tree has arrived.
- `src/core/registry/Registry.cpp:1950-2040` (`flush`): it calls `ensureSender()` at 1968 and sends as soon as `senderReady` is set (2020), before the `?VALUE` reply.

Nothing on this path checks `ACCESS` or waits for the first value. **Likely**, not isolated. The increase since `0d8daab` fits more `push()` calls around connection in the new code.

### 2.2 A host change back to the last received value is never sent, so a boolean cannot be switched off

Still present, same numbers

**What happens.** After the server sends a value V, the plugin never sends V again from the host. Moving away from V is sent; moving back to V is dropped. The server keeps the intermediate value while the host shows V. For a boolean that the server last sent as `false`, every switch-on is sent and every switch-off is lost.

**Reproduce with the script.**

1. Bind a slot to `/float`, direction `both`. After connecting, type `set /float 0.5`; the host shows 0.5.
2. Move the host to 0.8: the log shows `RECEIVED ... [0.8]`.
3. Move the host back to exactly 0.5 (type the value): nothing is received. `show` prints `/float 0.8`; the host shows 0.5.
4. Boolean: bind a slot to `/toggle` (TYPE `T`, value `false`). Toggle on (logged `[True]`), off (nothing), on (logged `[True]`), off (nothing). `show` prints `/toggle True`; the host shows off.

**Expected.** Every host change that differs from what was last *sent* goes out.

**Observed.**

| Case | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| Float back to the received value, test server | not sent; server 0.8, host 0.5 | same |
| Boolean toggles right, out of 10, test server, Max and TouchDesigner | 5, 5, 5 | 5, 5, 5 |
| Boolean back to the state the server pushed, all three servers | not sent | not sent |
| With the script | not run | float not sent; 2 of 4 toggles right |

**Proposal.** `TYPE` `T` and `F` "are used interchangeably", so a client may send either. Nothing in the proposal lets a client assume the server still holds a value it once sent.

**Code and cause.** `src/core/Instance.cpp:899-902`. `fromDevice` is the last received value, and it is never cleared when the plugin sends something else, so it goes stale after the first send. Comparing with the last value *sent* for that slot, or clearing `received[i]` once a different value is posted, would fix both cases. The cause is clear from the code. Not tested with a patch.

### 2.3 Crossed writes stay diverged: the server's final value is dropped as an echo

Still present

**What happens.**

1. The host sends A while the server, at the same moment, changes to B.
2. The plugin applies B to the host.
3. The server applies A last, so the server holds A while the host shows B.
4. An arbitrating server (Max with ossia, TouchDesigner) then sends its final value A back. The loop guard drops it, because it equals what the plugin sent less than 50 ms earlier.
5. Nothing rereads `?VALUE`, so the two sides stay apart until the next change.

**Reproduce with the script.**

1. Bind `/float`, direction `both`.
2. Type `cross /float 0.2 answer`.
3. Move the host to 0.6. The script logs the received 0.6, sends 0.2 (its crossing change), then sends 0.6 (its final value) 5 ms later.
4. The host shows 0.2; `show` prints 0.6.

**Expected.** Both sides end on the same value. An echo is dropped, but a value from the server that differs from what the host now shows is applied.

**Observed** (diverged out of 20):

| Case | `0d8daab`: test server / Max / TouchDesigner | `ec6725c` |
| --- | --- | --- |
| Simple crossing | 20 / 19 / 20 | 20 / 20 / 20 |
| Crossing with the message thread stalled 0.7 s | 20 / 20 / 20 | 20 / 20 / 20 |
| Crossing with everything stalled 0.7 s | 20 / 19 / 20 | 20 / 19 / 20 |
| With the script | not run | 3 of 3 |

No `?VALUE` reread was seen in any of them.

**Proposal.** No rule on arbitration. `LISTEN` streams "all OSC messages sent to that method", and the server's answer is one of them.

**Code and likely cause.** `src/core/registry/Registry.cpp:1369-1376`, the loop guard. It compares the incoming value with `known[i]`, the last value sent, but not with what the host now shows. Once the crossing value B has been delivered, an incoming A is no longer an echo. **Likely.** Declaring the sender port (2.10) would also let servers tell an echo from an answer.

### 2.4 A server restarted on the same HTTP port with a new OSC port loses every write

Still present

**What happens.** When the server restarts on the same HTTP host and port but a new `OSC_PORT`, the plugin reconnects: `HOST_INFO`, `LISTEN` and `?VALUE` are all seen again. It keeps sending UDP to the old OSC port. Received values keep working (WebSocket); sent values are lost silently.

**Reproduce with the script.**

1. Bind `/float`, direction `both`; check that a move is logged.
2. Type `restart new-osc-port`. The log shows the plugin reconnect within about 0.5 s.
3. Move the host three times: nothing is received.
4. Control: `restart`, without `new-osc-port`. All three moves arrive.

**Expected.** After a reconnection, the OSC sender follows the new `HOST_INFO`.

**Observed.**

| Case | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| New `OSC_PORT`, writes received, test server | 0 of 5, then 0 of 7 | 0 of 7, twice |
| Same `OSC_PORT` (control) | 7 of 7 | 7 of 7 |
| Found again by name at a new HTTP and OSC port, writes lost | 3 of 5, then 0 of 7 | 0 of 7, then 3 of 7 (intermittent) |
| With the script, new port / same port | not run | 0 of 3 / 3 of 3 |

**Proposal.** `HOST_INFO` `OSC_PORT`: "the port at which this OSC address can be reached". It is the server's word at each connection, not a constant.

**Code and likely cause.**

- `src/core/registry/Registry.cpp:1810-1813`: `ensureSender()` returns at once while `senderReady` is true.
- `senderReady` is reset only in `openDevice()` (`src/core/registry/Registry.cpp:1697`), which runs when the address changes.
- A reconnection to the same address goes through the library's `Device` without `openDevice()`, so the UDP socket keeps the old port. The `Unreachable` branch at `src/core/registry/Registry.cpp:1627-1643` does not reset it either.

**Likely.** Resetting `senderReady`, `udp` and `tcp` whenever `HOST_INFO` is read again would cover it.

### 2.5 A device with the same name is adopted although its tree hash differs

Still present; now adopted faster

**What happens.** The plugin is bound to device "X" at address B. B disappears. Another device called "X" at address A is adopted, even though the `tree_hash` saved in the state chunk does not match A's tree. The slots then drive a device whose tree differs from the one they were bound to.

**Reproduce.**

1. Start two copies of the script under the same name: `--port 5678 --name X` and `--port 5679 --name X --extra-leaf`. The extra leaf makes the trees, and their hashes, differ.
2. Announce the second one over DNS-SD: `dns-sd -R X _oscjson._tcp local 5679` on macOS, or `avahi-publish -s X _oscjson._tcp 5679` on Linux.
3. Bind a slot to `/float` on X at port 5678 and save, so the chunk holds the first copy's hash.
4. Quit the first copy and move the host: the writes arrive at the second copy.

These steps were not run with the script. The numbers below come from the test server, which does the same.

**Expected.** With a saved `tree_hash`, a single candidate whose hash differs is listed for the user and not adopted, as `DEVICE_ID` candidates already are.

**Observed.**

| Case | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| Adopted despite the hash | yes | yes |
| Writes received by the twin | 0 of 5, then 3 of 5 from 2 s | 5 of 5 from 0.5 s, twice |

**Proposal.** No rule; device identity is a draft, `docs/oscquery-identity-clock.md`.

**Code and cause.** `src/core/registry/Registry.cpp:898-914`. The hash is only checked when `matching.size() > 1`, and a single candidate is adopted at 916-919. The cause is clear from the code.

### 2.6 A host change made in the same cycle as an incoming value is never sent

Still present

**What happens.** The host moves a parameter, and a value from the server arrives before the next `processBlock`. The message thread writes the received value into the same host parameter, so the host's move is overwritten before `processBlock` reads it, and it is never sent. Under automation, or with a user on a control surface while the server streams, some moves are lost.

**Reproduce.** The window is one audio block, so a fixed sequence is more reliable than timing by hand:

1. Set the slot's host parameter to 0.9 from the host.
2. Before the next block, have the server push 0.3, so `handleAsyncUpdate` runs.
3. Run a block.
4. Nothing is sent, and the host shows 0.3.

A unit test in `tests/` around `PluginProcessor` can do this deterministically. With the script, `stream /float 200 20` while moving the control shows the loss only now and then: 1 of 10 moves lost in our check.

**Expected.** The host's move is sent, or kept as the newer value.

**Observed** (moves sent, out of 20):

| Case | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| AU, move alone | 20 | 20 |
| AU, move with an incoming value in the same cycle | 0 | 0 |
| VST3, move alone | 20 | 20 |
| VST3, move with an incoming value in the same cycle | 0 | 0 |

The VST3 path also goes through JUCE's hosted-parameter cache, which was not isolated.

**Code and likely cause.**

- `src/shims/juce/PluginProcessor.cpp:228-243`: `handleAsyncUpdate` calls `setValueNotifyingHost` with the received value.
- `src/shims/juce/PluginProcessor.cpp:112-122`: `processBlock` reads the parameter afterwards.
- `src/core/Instance.cpp:899-902`: the value read equals `received[i]`, so it is skipped.

**Likely.**

### 2.7 An ACCESS change across a server restart is ignored

Still present

**What happens.** A leaf goes from `ACCESS` 3 to 1 while the server restarts. The plugin reconnects and reads the tree again, but the slot stays `ok` and keeps writing.

**Reproduce with the script.**

1. Bind `/float`, direction `both`. Move the host: it is received.
2. Type `access /float 1`, then `restart`.
3. After reconnection, move the host. The script logs the write and `(refused: ACCESS 1)`; the slot still shows `ok`.

**Expected.** The slot becomes "ACCESS 1: read-only for this client" and sends nothing, as when the leaf is read-only from the start.

**Observed.** On the test server, `0d8daab` and `ec6725c` both keep the state `ok` and keep writing. With the script on `ec6725c`: same, 1 of 1.

**Proposal.** `PATH_CHANGED` covers "adding OSC methods, removing OSC methods, and renaming OSC methods", not attribute changes. A client therefore only learns a new `ACCESS` by reading the tree again, which the plugin does.

**Code and likely cause.**

- `src/core/registry/Registry.cpp:1170-1185`: the check that sets `unauthorised`.
- It reruns when the plans are rebuilt after `treeArrived`. A reconnection to the same address seems not to rebuild them.
- `Tree::treeHash()` (`lib/oscquery/oscquery/tree/Tree.cpp:734-749`) covers path and `TYPE` only, so a hash comparison would not see the change either.

**Likely**, not isolated.

### 2.8 When the incoming queue is full, the newest value is lost

Still present

**What happens.** Values from the server are queued for the message thread in a ring of 4096 entries. When the message thread is busy (a modal dialog, a heavy editor redraw) and the server streams more than 4096 values, the ring drops the newest ones. The host then settles on an old value: the final value is wrong, and nothing rereads `?VALUE`.

**Reproduce.**

1. Block the message thread for about a second, with a breakpoint in `handleAsyncUpdate` or a debug-only sleep.
2. Type `burst /float 6000` in the script. It sends 6000 values in one go, the last being 0.9.
3. Release the thread. The host shows a value other than 0.9.

**Observed.** On the test server, the final value was wrong 3 times out of 3 on both commits: the host stopped at the value at index 4095 (0.646 instead of 0.9). The controls, 1000 values stalled and 6000 values not stalled, ended right 3 times out of 3.

**Code and cause.**

- `src/core/registry/Registry.cpp:62`: `incomingCapacity = 4096`.
- `src/core/registry/Registry.cpp:2808`: the result of `push` is ignored.
- `src/core/registry/ChangeRing.h:96-99`: a full ring refuses the new item.

The cause is clear from the code. For values, keeping only the latest value per slot would keep the newest, or the plugin could reread `?VALUE` after a drop.

### 2.9 A server that keeps its HTTP connection open prevents any connection

Still present

**What happens.** The plugin sends `Connection: close` and reads until the server closes the socket. It never uses `Content-Length`. A server that answers with `Content-Length` but keeps the connection open makes every request time out, so the plugin never connects.

**Reproduce with the script.** Start it with `--keep-alive` and bind a slot. The log shows only `GET /?HOST_INFO`, repeated every few seconds, and nothing else.

**Observed.**

| Case | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| Test server, 30 s | no connection; 6 attempts at 0, 3.5, 7.5, 12.5, 19.5, 30.5 s | same |
| With the script, 16 s | not run | no connection; 4 attempts |

**Proposal and HTTP.** The proposal says the server "should be an HTTP 1.1 server". In HTTP/1.1 (RFC 9112 §9.6), a server that receives `Connection: close` must close after its response, so such a server is at fault. The impact is low, but reading `Content-Length`, or the end of a chunked body, would make the client robust to it.

**Code.** `lib/oscquery/oscquery/net/HttpGet.cpp:128-170`: the read loop ends only at `read <= 0`.

### 2.10 No sender port is declared, so libossia-based servers echo every UDP write

Still present

**What happens.** A libossia server, here Max with ossia prism.12, cannot tell which client a UDP datagram came from unless the client declares its ports. Without that, it streams the plugin's own writes back to it. The loop guard absorbs the echoes, so nothing loops, but every write costs a round trip. The server also cannot answer a crossing to the right client (see 2.3).

**Reproduce.** With the script: `--echo` imitates that server. Each write is logged `SENT ... (echo, to 1 listener)` and the host keeps its value. With Max and ossia: bind a slot and move it 20 times; count the values streamed back over the WebSocket.

**Observed.**

- Max echoed 20 writes of 20 on `0d8daab` and 19 of 20 on `ec6725c`.
- The test server set to echo sent 19; the plugin sent nothing back.
- TouchDesigner (leaves marked `CRITICAL`, so the plugin writes over the WebSocket): 0 echoes on both commits.

**Rule.** Not in the proposal. It is libossia's own `OSC_STREAMING` extension, which libossia declares in `HOST_INFO`; our TouchDesigner component accepts the same command. The client sends `{"COMMAND": "START_OSC_STREAMING", "DATA": {"LOCAL_SERVER_PORT": <port it receives OSC on>, "LOCAL_SENDER_PORT": <port it sends from>}}` over the WebSocket. Both servers then treat datagrams from that port as that client's.

**Code.** `lib/oscquery/oscquery/Device.cpp:589` and `628` send `LISTEN`. Nothing in the repository sends `START_OSC_STREAMING`.

### 2.11 The plugin's own server is announced under the library's name, not the host's

New in this range (Phase 6b-2)

**What happens.** The name should end with the host application's name. It ends with the core library's name instead, `"OSCQuery Automation @ <computer name> (libOSCQuery-Automation-Core)"`. Every DAW on a machine would therefore announce the same name.

**Reproduce.** Load the plugin in any host and run `dns-sd -B _oscjson._tcp`, or open the Monitor.

**Observed.** In the headless host (a Python process), the name ends with `(libOSCQuery-Automation-Core)`. It was not checked in a DAW.

**Code and likely cause.**

- `src/core/server/PluginServer.cpp:151-154` uses `File::currentExecutableFile`.
- In JUCE on macOS and Linux, that resolves with `dladdr` on a JUCE symbol (`ThirdParty/JUCE/modules/juce_core/native/juce_SharedCode_posix.h:622-638`), which here lives in the core dylib.
- `File::hostApplicationPath` would name the process.

**Likely**; the mechanism is clear, but a DAW was not tried. The same name feeds `deviceIdFor` (`src/core/server/PluginServer.cpp:212`), so the announced `DEVICE_ID` may not tell hosts apart either. Not tested.

### 2.12 A boolean is resent at every host change, even when its state does not change

Still present, unchanged

**What happens.** A boolean slot sends `F` again for host values of 0.2, 0.4 and 0.49: three identical messages. The threshold is 0.5 inclusive.

**Reproduce with the script.** Bind `/toggle`. Move the host continuously below 0.5: the script logs one `[False]` per change.

**Code.** `src/core/Instance.cpp:892` compares the continuous host value with the last one posted. The conversion to `T`/`F` happens later (`src/core/Slot.cpp:232-233`). Comparing the converted state would send one message per state change.

### 2.13 Servers that list their clients show the plugin without a name

Still present Low impact.

Max with ossia prism.12 and our TouchDesigner 0.4.0 list connected clients, with the name each client gives. The plugin gives none, so it shows only as an address.

**Rule.** Not in the proposal, nor in `docs/oscquery-identity-clock.md`. It is an extension we have proposed for OSCQuery: a server declares `CLIENT_NAME` in `HOST_INFO` `EXTENSIONS`, and the client sends `{"COMMAND": "CLIENT_NAME", "DATA": "<name>"}` after connecting. The device name, or the DAW and track, would do.

**Observed.** No name on `0d8daab` or `ec6725c`.

## 3. What changed since the earlier reports, and what is new

The code review before the first reports read `d323535`. The first measurements were on `0d8daab`, 21 commits later (Phases 5b to 6a-3), and the comparisons below are with `0d8daab`. From `0d8daab` to `ec6725c` there are 28 commits:

- Phases 6b to 6f: your own server, the editor and the Monitor.
- Phase 6x: `DEVICE_ID` and `CLOCK`.
- Paced mDNS answers, with DNS-SD that can be turned off.

### Reconnection now goes through DNS-SD

Changed

**Before.** After the server went away and came back at the address saved in the chunk, the plugin reconnected to that address: 3 of 5 times through our relay, in 4.1 s with Max.

**Now.** After one refused connection at the saved address, the plugin joins the device of the same name at the address announced over DNS-SD, and does not try the saved address again:

| Case | `0d8daab` | `ec6725c` |
| --- | --- | --- |
| Max, reconnections through the saved address | 3 of 5 | 0 of 7 (all went to the announced address) |
| Max, direct reconnection | 4.1 s | 3.1 s |
| TouchDesigner, reconnection | 15.8 s, through the saved address | 13.3 s, through the announced address |

It is faster. However, the address the user entered (here a relay) is dropped after a single refusal, and the chunk saves the new one. A user who points the plugin at a proxy, a tunnel or a specific interface loses that choice silently.

The filter that excludes the saved address (`src/core/registry/Registry.cpp:794-796`) is the same as at `0d8daab`. **Likely**, the change comes from timing: discovery now answers before the saved address is retried. Not isolated.

### The plugin's own OSCQuery server, on by default

New

- Each process that loads the plugin serves its slot table over OSCQuery and announces it as `_oscjson._tcp`.
- It runs with the default settings (`server_enabled` and `discovery_enabled` both true).
- The name defect is in 2.11.
- Writes to this server were not tested.

### The WebSocket crash

Not reproduced

- On `0d8daab`: one SIGSEGV on the "Web socket client" thread (`asio::kqueue_reactor::run`, address 0x78), in 1 of 10 reconnections to Max.
- On `ec6725c`: none in 12 reconnections to Max and 2 to TouchDesigner, and no crash report.

That does not prove it is fixed.

### Server side

Max with ossia prism.12 now refuses a direct OSC write to a read-only leaf; prism.11 accepted it. TouchDesigner already refused it. On these two servers, the plugin's own refusal is no longer the only protection. It still matters for 2.1 on other servers.

### Still good

- `?VALUE` after each `LISTEN`.
- Chunked replies.
- One bundle per block with only the changed values (4 of 8).
- Timetags 99.7 ms ahead for a 100 ms offset.
- Bundles split at the MTU (1360 and 720 bytes).
- Reconnection to the same port in 1.8 s, and by name in 2.1 s.
- Writes served in 10 ms by Max and in 23 to 69 ms by TouchDesigner.
- Boolean reading: `true`, `false`, `1`, `0`, `"true"` and `TYPE` `F` are read; `null` or a missing value leaves the preset.
- Received values right 10 of 10 on all three servers.
- A read-only leaf bound as `receive` follows the server 5 of 5 and sends nothing.
- A read-only leaf bound `both` or `send` shows "ACCESS 1: read-only for this client" and sends nothing once connected (but see 2.1).

## 4. What was not tested

- A real DAW. All results come from the headless host.
- The editor and the Monitor in use; only their data was read.
- Writes to the plugin's own server, and two plugin processes seeing each other's servers.
- The CLAP with bound slots. clap-validator's parameter tests are skipped because unbound slots are hidden.
- `DEVICE_ID` and `CLOCK`: none of the three servers declares them.
- `ACCESS` 2 (write-only), `IGNORE`, OSC over TCP.
- Windows and Linux.
- Several instances in one process sharing the registry.
- With the script: steps 2.5 (same-name twin) and 2.8 (burst with a blocked message thread) were not run. Their numbers come from the test server.

## 5. Status on 2026-11-11: what was fixed, what is still to do

Added by Pierre-Olivier Boulant's Claude Code session, the same day. The sections above are the report as received and are unchanged.

Each fix has a test in `tests/` that fails without it, except where a row says otherwise. Nothing below was checked in a DAW, or against the script or the three servers of this report: that check is the retest.

### Fixed

| Item | Pull request | What changed |
| --- | --- | --- |
| 2.2 Host move back to the received value not sent | #6, merged | The last value received is forgotten once the host sends something else, so moving back to it is sent like any other move. |
| 2.4 New `OSC_PORT` after a restart | #7, merged | When the device goes away and comes back, the sender is reopened from the `HOST_INFO` just read. |
| 2.7 `ACCESS` change across a restart | #7, merged | The cause was narrower than guessed. The tree was re-read after a dropped connection, but not after the device had been down long enough for one reconnection to fail, which a server taking a second to restart is. It is now re-read in both cases. Only the library-level test fails without the fix; the plugin-level one is a guard. |
| 2.8 Newest value lost when the incoming queue is full | #7, merged | When the queue is full, the latest value per slot is held aside and delivered once the queue has room, so the last value delivered is the newest. |
| 2.11 Server named after the core library | #6, merged | `File::hostApplicationPath` instead of `currentExecutableFile`. The server's `DEVICE_ID` changes with it. The test cannot catch a regression (both calls agree inside the test program); a DAW or the headless host is the real check. |
| 2.12 Boolean resent at every host change | #7, merged | A toggle is sent when its state changes. What was last sent is forgotten when the device changes the value itself or comes back from a restart. |

### Fixed, merge pending (#8, branch `fix/retest-batch-3`)

These change what the plugin is specified to do; the PRD is amended in place, dated 2026-11-11.

| Item | What changed |
| --- | --- |
| 2.1 Values sent at connection; read-only leaves written | Connecting, binding a slot and loading a session send nothing; the first block after a session load only notes where the host stands. A host move is sent. When the transport starts, every sending slot is sent once. Nothing is sent to a leaf before the tree has been read and its `ACCESS` checked; a move made in that window is held and sent once the leaf is known writable. |
| 2.3 Crossed writes stay diverged | For 200 ms after the host moves a slot, values from the device for that slot are kept aside instead of applied. When the window ends, a device that ended on another value is sent the host's value again; one that agrees is left alone. In the report's crossing (0.2, then the server's final 0.6), both sides end on 0.6. The loop guard itself is unchanged. |
| 2.6 Host change lost in the same cycle as an incoming value | The same 200 ms hold. In addition, the JUCE shim (VST3, AU) passes each parameter's current value when received values are applied, so a host move the audio thread has not seen yet is not overwritten. The CLAP shim relies on the hold alone. JUCE's hosted-parameter cache on VST3 was not isolated. |
| 2.5 Same-name device with another tree adopted | The single candidate under the saved `NAME` is adopted only if it serves every path the session is bound to, with the same `TYPE`. If it does not, it is listed, checked again every few seconds and adopted by itself once it does; the user may point at it meanwhile. **Decided, and different from what the report expects:** the hash is not the test, because it changes whenever the tree does (a source added, a processor still starting). So the report's own twin, which differs by an extra leaf but serves the bound path, is still adopted, with the "tree changed since the bind" notice. |

**What the retest should expect from #8:**

- **2.1:** nothing on the wire at connection, then the moves. The send at transport start needs a host that reports a play head; a headless host without one never starts the transport.
- **2.3:** both sides converge, about 200 ms after the last host move.
- **2.5:** a twin missing the bound path is held; one with an extra leaf is adopted.

### Found and fixed during this work, not in the report

| Issue | Pull request | What changed |
| --- | --- | --- |
| A TCP peer that hangs up could kill the host process on macOS and Linux (`SIGPIPE`) | #9, merged | The client library's HTTP GET and TCP OSC sender wrote with `juce::StreamingSocket::write`, a plain `send()`. A device restarting between the connection and the write ended the process with no crash report. Every such write now goes through a write that cannot raise the signal. The headless host and the "WebSocket crash" of section 3 could have met this. |

### Still to do

| Item | Status |
| --- | --- |
| 2.9 A server keeping its HTTP connection open | Not fixed. Low priority: such a server breaks HTTP/1.1. Reading `Content-Length`, or the end of a chunked body, would make the client robust to it. |
| 2.10 No sender port declared (`START_OSC_STREAMING`) | Waiting for the libossia update from Jean-Michaël Celerier, so that the extension is adopted in the form libossia settles on. |
| 2.13 No client name (`CLIENT_NAME`) | Waiting, for the same reason. |
| Section 3, a saved address dropped after one refusal | Not addressed. The plugin moves to the device announced over DNS-SD and saves its address, so a relay, tunnel or chosen interface the user entered is lost. Needs a decision: try the saved address longer, or keep a typed address as the user's choice. |
| Section 1, `RtCheckTests` failing in Release | Not addressed. As the report says, probably an allocation optimised away. The CI runs them in Debug only. |
| The AU's "Current program is -1" pluginval warning | Not addressed; unchanged since before this report. |
| The test suite on macOS | Two intermittent test failures on the Mac mini, not plugin defects as far as is known. About one full run in thirty, a device on loopback took more than ten seconds to be ready, unexplained; the tests now print the device's status when that happens. And a message-count check found 49 messages instead of 50 once in thirty runs. |

## Appendix: `oscquery_repro.py`

```
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