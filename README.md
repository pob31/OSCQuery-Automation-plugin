# OSCQuery Automation Plugin

A DAW plugin that records and plays back automation curves to and from any OSC-controlled
device, with the device's address space discovered over OSCQuery instead of typed into a
mapping file. CLAP, VST3 and AU from one codebase. The reference client for an OSCQuery
discovery profile, with a conformance tool that prints a device's interoperability row.

**Status:** Phase 0 of the plan. The repository builds a plugin that does nothing yet (VST3 on
Windows, macOS and Linux, AU on macOS), validated by pluginval in CI, and a mock OSCQuery device
the tests drive. The product requirements, the phase plan and the profile draft are in
[`docs/`](docs/).

- [PRD](docs/PRD%20—%20OSCQuery%20Automation%20Plugin%20(CLAP%20VST3%20AU).md)
- [Development plan, draft 0.1](docs/Devplan%20—%20OSCQuery%20Automation%20Plugin%20(draft%200.1).md)
- [OSCQuery discovery profile, draft 0.1](docs/spec/core-profile-draft-0.1.md): the Core and Live
  profiles the plugin is the reference client for, out for review.
- [`CLAUDE.md`](CLAUDE.md): the review criterion for every change, and where to look.

## Building

CMake 3.22 or later, a C++20 compiler, and the submodules:

```
git clone --recurse-submodules https://github.com/pob31/OSCQuery-Automation-plugin.git
```

| Where | Configure, build, test |
| --- | --- |
| Windows, any shell | `cmake --preset vs`, `cmake --build --preset vs-debug`, `ctest --preset vs-debug` |
| macOS, Linux (Ninja) | `cmake --preset dev`, `cmake --build --preset dev-debug`, `ctest --preset dev-debug` |

The suite runs twice, under `C` and under the French locale, and fails if the French locale is
missing (`sudo locale-gen fr_FR.UTF-8` on Debian and Ubuntu). The plugin lands in
`build/<preset>/src/shims/juce/oqa_plugin_32_artefacts/<config>/`. On Linux, `.github/workflows/ci.yml`
lists the packages JUCE needs.

`tests/mock-device/` has a mock OSCQuery device you can run by hand:
`oqa-mock-device --fixture tests/fixtures/trees/generic-device.json`.

Before pushing: `python3 scripts/check-claude-md.py`, `check-pins.py` and `check-headers.py`, as
CI's first job runs them.

## Release builds and installers

One script per platform builds every shipped binary (`oqa_plugins`, Release), stages it under
`build/stage/<os>/` and makes the installer in `build/installer/`. `.github/workflows/release.yml`
runs the same scripts.

| Where | Command | Installer |
| --- | --- | --- |
| Windows | `pwsh scripts/build-release.ps1` | Inno Setup 6 `.exe`, unsigned |
| macOS | `scripts/build-release.sh` | universal `.pkg`; signed and notarised when `CODESIGN_IDENTITY_APP`, `CODESIGN_IDENTITY_INSTALL` and notary credentials are set (`installer/macos/build-pkg.sh`) |
| Linux | `scripts/build-release.sh` | `.tar.gz` with `install.sh --user` or `--system` |

`--install` (`-Install` on Windows) also copies the plugins into this user's plugin folders,
with no administrator needed, for a host on the same machine. `--skip-installer` and `--skip-build`
do what they say.

The installers put the shared core library at the fixed path the plugins look for, so every
format in one host shares one registry: `%CommonProgramFiles%\Pix et Bel\OSCQuery Automation\`,
`/Library/Application Support/Pix et Bel/OSCQuery Automation/`, or
`/usr/local/lib/oscquery-automation/` (a `--user` install puts a copy beside each plugin folder
instead).

The release workflow runs when a release is published (the tag must be `v` plus the version in
`project()`), when started by hand, and on pull requests that touch the installers. The signed
macOS job needs approval in the `OSCQuery-Automation` environment; on a pull request it runs
only if the PR carries the `sign` label.

## Licence

GPLv3. Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle. See [LICENSE](LICENSE).

The profile text in [`docs/spec/`](docs/spec/) is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) instead, so that manufacturers can quote it.
