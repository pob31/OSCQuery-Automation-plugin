# Bench, 2026-11-10: the validators flooded the network with multicast DNS

Found by the author: twice, on two evenings of farm checks, the phone hotspot his machines reach
the internet through became unusable. The second time it was looked for and measured. The cause
was in this repository, and it is fixed on the branch `fix/mdns-flood`.

## What was measured

The farm check's phases run one at a time on the NUC (Ubuntu 24.04; Ethernet on the home network,
Wi-Fi on the phone's hotspot), on the Phase 6x build. For each phase: the multicast datagrams the
machine sent (`nstat`, `IpExtOutMcastPkts`) and a ping to the phone every half second.

| Phase | Multicast datagrams sent | Ping to the phone during the phase |
| --- | --- | --- |
| Idle, 30 s | 0 | 0% loss, 7 ms |
| The library's tests | 0 | 0% loss |
| The core's tests | 62 | 0% loss |
| The CLAP tests | 18 | 0% loss |
| pluginval, the plugin and the Monitor | 10 each | 0% loss |
| **clap-validator** | **11,600 in about a second** | **50% loss; latency up to 2.9 s over the next 30 s** |

The author's Windows machine, on both networks, saw the burst arrive: about 4,600 multicast
datagrams on Wi-Fi and 5,800 on Ethernet within a few seconds, and its one lost ping to the phone
fell in that second. The Mac mini's clap-validator, a minute later, put about 13,000 on the home
network (it has two interfaces there).

## Why

- **pluginval and clap-validator load the real plugin**, which reads the per-user settings, not
  the tests' own. With the defaults its server is on and announced over DNS-SD, on every interface.
- **clap-validator runs its 229 tests in separate processes, within a second or two.** Each one
  started a browser and an announcer.
- **The responder answered every question it heard, at once, to the group**
  (`Discovery::answer`, as copied from go.dot): no delay, no suppression of an answer somebody
  else just gave, no limit on how often a record goes out. So every process answered every other
  process's question, on every interface.
- **A hotspot is the worst place for it**: Wi-Fi sends multicast at its slowest rate, and several
  thousand datagrams hold the air for seconds.

The unit tests were never part of it: they run with the server off and the browser asking a
responder of their own on loopback.

## What changed

| Change | Where |
| --- | --- |
| A multicast answer waits 20 to 120 ms, is dropped if another responder says the same meanwhile, and no record is multicast twice within a second (RFC 6762 §6, §7.4) | `lib/oscquery/oscquery/mdns/AnswerPacer.h`, used by `Discovery` |
| A budget on what one browser sends to the group: ten datagrams a second, a burst of twenty; the rest is dropped | the same |
| A setting, `discovery_enabled` (default on): off, the Finder neither browses nor announces; typed devices are still probed and the server is still served. In the Monitor's settings form as "Find devices over DNS-SD" | `RegistrySettings`, the registry's resolver |
| pluginval and clap-validator run with `tests/validators.registry.json` (the server on, discovery off), a copy named in `OQA_SETTINGS_FILE` | `.github/workflows/ci.yml`, `scripts/farm-check.sh` |

## After

| Check | How | Result |
| --- | --- | --- |
| clap-validator with the validators' settings, on the NUC, on the hotspot | three runs of the fix branch's Release CLAP, counted and pinged as above | **0 multicast datagrams each run (11,600 before); ping to the phone 0% loss, 5 to 6 ms; 229 run, 0 failed; the settings file unchanged** |
| Two hundred responders asked at once | `AnswerPacerTests.cpp`, times of the test's choosing | **one datagram, not two hundred; silence for the rest of the second** |
| The budget | a thousand sends in an instant | **twenty pass; five more half a second later** |
| Discovery off in a registry | a settings file with it off and the server on; a mock typed by hand | **the server runs and is not announced; the Finder is not started for it; the typed device is probed and found** |
| The suites | `ctest --preset vs-debug`, Windows, Debug | **8 of 8 entries, C and fr_FR** |

**Not measured live:** clap-validator with discovery *on* and the pacing alone. It would put a
burst on the author's hotspot on purpose. By the rules above a process may still send its own
announcements, its goodbye and its questions, a handful of datagrams each; with two hundred
processes that is hundreds, not thousands. The validators no longer do it at all.

## For whoever runs a validator by hand

Set `OQA_SETTINGS_FILE` to a copy of `tests/validators.registry.json` first, as the farm script
and CI do. Without it the plugin announces itself from every process the validator starts.
