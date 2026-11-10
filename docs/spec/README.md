# docs/spec

The OSCQuery discovery profile (Core and Live) that the plugin is the reference client for.

- [`core-profile-draft-0.1.md`](core-profile-draft-0.1.md): draft 0.1, 2026-11-04, out for review.
- [`extensions-draft-0.1.md`](extensions-draft-0.1.md): draft 0.1, 2026-11-10, by Julien Bayle. Two
  optional extensions, `DEVICE_ID` and `CLOCK`, outside the profile: a device passes Core and Live
  without them, and the plugin uses them only when a device declares them.

**Licence.** Everything in this folder is licensed under
[Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)
(decided 2026-11-04), so manufacturers can quote clauses in their own documentation. The rest of the
repository is GPLv3. Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle.

Each clause maps to one check of the PRD's interoperability matrix (C1–C7 Core, L1–L5 Live), and the
conformance tool in `tools/conform/` runs one check per clause. Clauses marked *(proposed)* go beyond
the PRD and wait for a reviewer's yes before the plugin or the tool implements them.
