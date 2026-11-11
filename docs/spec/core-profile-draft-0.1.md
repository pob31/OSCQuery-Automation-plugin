# OSCQuery discovery profile: Core and Live

**Draft 0.1**, 2026-11-04. Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle. This text is
licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/): quote it, copy clauses into
your own documentation, with attribution.

A profile of the [OSCQuery proposal](https://github.com/Vidvox/OSCQueryProposal) (README at commit
`e3d8e3b9`, 2018-08-31, "the third revision of this proposal"): which of its parts a device has to
serve, and how, for a client to bind to the device's parameters without anyone typing an address, and
to find the same device again after it moves to another IP. It is written for device manufacturers and
for authors of other clients. The OSCQuery Automation Plugin
([github.com/pob31/OSCQuery-Automation-plugin](https://github.com/pob31/OSCQuery-Automation-plugin)) is
its reference client, and that repository's conformance tool runs one check per clause below.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are to be read as in RFC 2119 and RFC 8174,
when, and only when, they appear in capitals. Clauses that go beyond what the plugin's requirements
already state are marked *(proposed)*; they are the ones this draft asks reviewers to rule on.

---

## 1. Why a profile, and why it adds nothing

OSCQuery is itself a proposal that is still looking for support, and the devices that implement it
today implement different subsets of it. Another revision with new fields would ask every one of them
to move twice: once to the proposal, once to the amendment. A profile asks them to move once, toward
text that already exists. So this profile **adds no field** to `HOST_INFO` and no attribute to a node:
no UUID, no `TIMETAGS`, no reply port. What it does add are constraints on fields the proposal already
has (that `NAME` stays put, that `RANGE` is there), and a reading of the places where the proposal can
be read two ways (§4). What a client needs beyond that, it keeps on its own side: which read-only leaf
of the device's tree identifies the unit, whether the device honours bundle timetags, which UDP port to
send feedback to. The user sets those once in the client; the device is not asked.

*(Added 2026-11-10.)* Two optional extensions, a device identifier and a clock declaration, are drafted
apart in [`extensions-draft-0.1.md`](extensions-draft-0.1.md). They are outside this profile: no clause
below depends on them, and a device passes Core and Live without them.

## 2. Terms

- **Device**: an OSCQuery server, with the OSC server it describes.
- **Client**: anything that reads the device's tree and sends or receives its OSC.
- **Leaf**: a node with a `TYPE` attribute. **Container**: a node with `CONTENTS` and no `TYPE`.
- **Bindable leaf**: a leaf whose `ACCESS` is not 0, and whose `TYPE` the client supports. A container
  is never bindable.
- **Allow-list**: whatever mechanism the device uses to decide which clients may change its values (a
  client table, a remote-control switch). This profile does not define one; it only asks that what
  the device answers be consistent with it (C6, L3).

## 3. Core profile

A device that passes C1 to C7 is a **Core** device. A Core device can be browsed, bound and found
again; it does not have to stream anything.

**C1 — `HOST_INFO`.** The device MUST answer `GET /?HOST_INFO` with a JSON object holding at least
`NAME`, `OSC_PORT` and `OSC_TRANSPORT`, as the proposal defines them. `OSC_IP`, `WS_IP` and `WS_PORT`
MAY be absent, with the proposal's meaning: absent `OSC_IP` means the OSC server is at the HTTP
server's address, absent `WS_PORT` means the websocket is on the HTTP port. `EXTENSIONS` SHOULD be
present and SHOULD list every optional attribute and websocket feature the device serves, because a
client MUST NOT assume one that is not listed.

**C2 — Stable `NAME`.** `NAME` MUST be the same across reboots and changes of IP address, and
*(proposed)* across firmware updates that keep the configuration. Where the device advertises over DNS-SD (C3), `NAME` MUST
equal the DNS-SD instance name. `NAME` does not have to be unique: two units of one model may share
it. A client tells them apart by a read-only leaf the user designates (§5), never by a field the
device is asked to add. A device that lets the user rename it SHOULD use that name as `NAME`.

**C3 — DNS-SD.** The device MUST advertise the service type `_oscjson._tcp` on every interface it
serves on, with the instance name equal to `NAME` and the port equal to the HTTP port. (The proposal
names the service type and says nothing of the instance name; C2 and C3 together close that.)

**C4 — Typed leaves.** Every leaf whose `ACCESS` includes write (2 or 3) MUST have `TYPE`. Every
argument of type `f` or `i` MUST have a `RANGE` entry with `MIN` and `MAX`, or `VALS`. For a node of
several arguments, `RANGE` is an array with one entry per argument, in type-tag order, as the proposal
says; `null` holds the place of an argument that has no range, and such an argument fails C4 if it is
`f` or `i`. A leaf SHOULD have `ACCESS`: without it the proposal tells clients to assume the value is
writable, which turns every read-only value into a send target.

**C5 — Path stability.** The set of leaves, by `FULL_PATH` and `TYPE`, MUST be the same after
a reboot, and after the device's own session or show file is saved and reloaded, as before. Paths are
the binding; a device that renumbers its channels on reload breaks every session bound to it. Devices
whose address space genuinely changes with configuration (channels added by the user) pass as long as
the paths that existed before still exist with the same type. The check compares tree hashes:

> *(proposed)* **Tree hash** = SHA-256, as lowercase hex, of the UTF-8 text made of one line
> `FULL_PATH` TAB `TYPE` LF per leaf (every node with a `TYPE`, whatever its `ACCESS`), the lines
> sorted by byte value. `RANGE`, `VALUE`,
> `DESCRIPTION` and `UNIT` are left out on purpose: a range can follow configuration (a room size, a
> speaker count) without breaking a binding, and a client re-reads it on every connection. `ACCESS` is
> left out because it depends on which client asks (C6), and for the same reason every leaf enters
> the hash, not only the bindable ones: two clients the device treats differently compute the same
> hash. *(Amended 2026-11-10: this read "per bindable leaf", and whether a leaf is bindable depends on
> its `ACCESS`.)*

**C6 — `ACCESS` as reported to this client.** `ACCESS` MUST describe what the requesting client may
do. A client that the device's allow-list does not let write MUST see `ACCESS: 1` (or 0) on leaves it
cannot change, not `2` or `3`. A device with no allow-list passes trivially. The point is that the
client can say *why* a slot will not send, before the user draws a curve that goes nowhere.

**C7 — Subtree GET.** `GET /<path>` for any container or leaf MUST return the node at that path with
its descendants and nothing else, in the same shape as that node takes inside `GET /`. A path that does
not exist MUST return 404. This is what lets a client browse a 50,000-node tree one level at a time.

## 4. Live profile

A Core device that also passes L1 to L5 is a **Live** device: it can be recorded from, and a client
learns of changes to its tree while connected. Every item below is optional in the proposal, which
says websockets are "strongly encouraged" but not required; a Core device without them is still fully
usable for playback, and for record through a UDP port the user configures on the device.

**L1 — `LISTEN`.** After a client sends `{"COMMAND": "LISTEN", "DATA": "<FULL_PATH>"}` as a text frame,
the device MUST send that leaf's value changes to that client as binary frames, each an OSC message
(or a bundle of them) at the leaf's `FULL_PATH`, until `IGNORE` or disconnection. Changes MAY be
coalesced to the device's own update rate, but the latest value MUST be sent. *(proposed)* A device MAY
skip sending a change back to the websocket connection that caused it; it MUST NOT skip by IP address,
because several clients (one per plugin process, or a DAW and a conformance tool) share one host.

**L2 — Change notifications.** When a bindable leaf disappears the device MUST send
`{"COMMAND": "PATH_REMOVED", "DATA": "<old FULL_PATH>"}`; when one moves, it MUST send
`{"COMMAND": "PATH_RENAMED", "DATA": {"OLD": "<old FULL_PATH>", "NEW": "<new FULL_PATH>"}}`, to every
connected client. `PATH_ADDED` SHOULD be sent when a node appears. `PATH_CHANGED` with the node's path
remains the catch-all for anything else (a `TYPE` or `RANGE` that changed); a client re-reads that path.
*(proposed)* A device that sends any of these SHOULD list each one it sends in `EXTENSIONS`, as the
proposal's example does for `PATH_CHANGED`.

**L3 — Websocket OSC under the same rules.** OSC the device accepts as binary frames on the websocket
MUST be subject to the same allow-list as OSC on `OSC_PORT`. The query port must never be a way round
the device's write protection.

**L4 — `CRITICAL` honoured.** A leaf marked `"CRITICAL": true` MUST accept its OSC over a TCP
transport: on `OSC_PORT` when `OSC_TRANSPORT` is `"TCP"`, and otherwise as a binary frame on the
websocket. *(proposed)* OSC over a TCP `OSC_PORT` is SLIP-framed, as OSC 1.1 specifies.

**L5 — Large trees.** `GET /` for a tree of up to 10,000 leaves MUST arrive complete within 2 s on a
local network. *(proposed)* Above 10,000 leaves a device MAY instead rely on C7, and then each subtree
GET of one level MUST arrive within 2 s. The check reports the time and the leaf count, not just a verdict.

## 5. What the profile does not ask for

- **No device identity field.** *(An optional one is drafted outside the profile, see §1.)* A client stores `NAME`, the last host and port, the tree hash, and,
  when the user designates one, the path and value of a read-only leaf the device already publishes
  (a serial number, a host name). On reopen it tries the last address, then DNS-SD by `NAME`, then
  tells candidates apart by that leaf's value, then by tree hash; what is still ambiguous, the user
  points at once.
- **No timetag flag.** Whether a device honours bundle timetags is a per-device setting in the client.
  *(An optional clock declaration that can pre-fill it is drafted outside the profile, see §1.)*
- **No reply port.** Without `LISTEN`, a client shows its own UDP port and the user enters it in the
  device's configuration, as with any OSC controller today.
- **No authentication.** Write access is the device's business; C6 and L3 only ask that what the
  device reports and enforces agree.

## 6. Where the proposal reads two ways, and how a profile client reads it

| Point | The proposal | A profile client |
| --- | --- | --- |
| Unsupported attribute query | Says 204 in one place, defines 400 for it in another | Accepts both. A device SHOULD answer 204 for a known attribute this node lacks, 400 for an unknown attribute |
| Change notifications | `PATH_CHANGED` "would" cover adding, removing and renaming; `PATH_ADDED`, `PATH_REMOVED`, `PATH_RENAMED` are also defined, without examples | Handles all four; L2 asks for the specific ones |
| Which notifications are in `EXTENSIONS` | Only `PATH_CHANGED` appears in the example | Uses whatever is listed; never infers an unlisted one |
| DNS-SD instance name | Not specified | Must equal `NAME` (C2, C3) |
| `CRITICAL` transport | "a TCP connection of some sort" | TCP `OSC_PORT` if declared, otherwise the websocket (L4) |
| OSC framing over TCP | Not specified | SLIP (L4, proposed) |
| `HOST_INFO` path | "the path in the URL is functionally irrelevant" | Always asks `GET /?HOST_INFO` |
| `RANGE` wording | Refers to a `TYPES` attribute that does not exist | Reads it as `TYPE` |

## 7. Clauses and checks

Each clause is one row of a device's interoperability matrix. The conformance tool (`conform`, in the
plugin repository) runs them against `host:port`, or finds the device by `--browse`, and prints the row
as Markdown and JSON.

| Check | Profile | Passes when | How it is run |
| --- | --- | --- | --- |
| C1 | Core | `HOST_INFO` has `NAME`, `OSC_PORT`, `OSC_TRANSPORT` | One GET |
| C2 | Core | `NAME` unchanged across reboot; equals the DNS-SD instance name | Two snapshots, `--before` and `--after` a reboot; DNS-SD browse |
| C3 | Core | `_oscjson._tcp` advertised with that name and the HTTP port | DNS-SD browse on every interface |
| C4 | Core | Every writable leaf typed; every `f`/`i` argument ranged | Walk of `GET /` |
| C5 | Core | Tree hash unchanged across reboot and session reload | Snapshots before and after; the hash above |
| C6 | Core | A client off the allow-list sees `ACCESS: 1` on writable leaves | The device set up to refuse the tool's address, then a walk |
| C7 | Core | `GET /<path>` returns that subtree only; unknown path 404 | Every container compared with its place in `GET /` |
| L1 | Live | `LISTEN` streams changes | A value written on one connection, observed on another |
| L2 | Live | `PATH_REMOVED` and `PATH_RENAMED` sent | The operator removes and renames a parameter on the device |
| L3 | Live | Websocket OSC under the UDP allow-list | A write over the websocket from a refused address must not land |
| L4 | Live | `CRITICAL` leaves accept TCP OSC | A write over the TCP path, read back |
| L5 | Live | 10,000 leaves within 2 s | Timed `GET /`; time and count reported |

Rows for each device, with dates and firmware versions, are published with the plugin's releases.
