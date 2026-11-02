# Host compatibility

## Summary

Rinne builds for Windows (x64, VST3) and macOS (Apple Silicon, VST3 and Audio Unit). This page says what has been checked in which host, and how far. It is deliberately conservative: a host only moves up a tier when a written test record in [docs/validation/](validation/TEMPLATE.md) backs it, and a host nobody has opened is listed as waiting for its first test, not as working.

Today there is no record of a real-host acceptance test in the repository. The two hosts being tested first, Cubase Pro 15 on Windows and LUNA on macOS, therefore stand at Tier 3. The macOS Audio Unit builds are beta.

## Tier definitions

| Tier | Meaning | What it needs |
|---|---|---|
| Tier 1 | Fully verified | The release build passed the whole acceptance matrix below on a real host, and the measured loop period equals D |
| Tier 2 | Partly verified | A written record on a real host, covering only the items it lists |
| Tier 3 | Not verified on a real host | CI evidence only: build, tests, and the plugin validators (pluginval on both systems, auval for the Audio Units) |
| Out of scope | Not provided | The format or platform is not built, or the host is known to be unsuitable |
| Awaiting test | No tier yet | Nothing has been looked at in this host |

The acceptance matrix has seven items: realtime playback, offline export, Freeze, save and reopen, buffer sizes (64, 256, 1024 and 2048), ASIO-Guard on and off, and the loop period measured against D. Each record in `docs/validation/` states which items passed.

## Current matrix

<!-- BEGIN gen:daw-tiers:en -->
| Host | Version | Platform | Formats | Tier | Status | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Cubase Pro | 15 | Windows | VST3 | Tier 3 | Partial | CI evidence only (build, tests, pluginval). No acceptance record from a real host yet. |
| LUNA | 3.x | macOS | VST3 / AU | Tier 3 | Partial | CI evidence only (build, tests, auval, pluginval). No acceptance record from a real host yet. The Audio Unit build is beta. |
| Cubase Pro | 14 | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| Cubase Artist | 15 | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| Cubase Elements | 15 | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| Nuendo | any | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| Cubase Pro | 15 | macOS | VST3 / AU | — | Awaiting test | Not tested on a real host yet. |
| REAPER | 7 | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| REAPER | 7 | macOS | VST3 / AU | — | Awaiting test | Not tested on a real host yet. |
| Ableton Live | 12 | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| Ableton Live | 12 | macOS | VST3 / AU | — | Awaiting test | Not tested on a real host yet. |
| Studio One | any | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| Studio One | any | macOS | VST3 / AU | — | Awaiting test | Not tested on a real host yet. |
| Bitwig | any | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. Its plug-in hosting mode may load the two plug-ins into different processes. |
| Bitwig | any | macOS | VST3 | — | Awaiting test | Not tested on a real host yet. Its plug-in hosting mode may load the two plug-ins into different processes. |
| FL Studio | any | Windows | VST3 | — | Awaiting test | Not tested on a real host yet. |
| FL Studio | any | macOS | VST3 / AU | — | Awaiting test | Not tested on a real host yet. |
| Logic Pro | any | macOS | AU | — | Awaiting test | Audio Units only. Not tested on a real host yet. |
| GarageBand | any | macOS | AU | Out of scope | Out of scope | Not supported: sandboxed and out-of-process Audio Unit hosting is not tested. |
| Pro Tools | any | Any | AAX | Out of scope | Out of scope | Not supported: this build ships VST3 and Audio Units only. |
| CLAP | any | Any | CLAP | Out of scope | Out of scope | Not supported: this build ships VST3 and Audio Units only. |
| VST2 hosts | any | Any | VST2 | Out of scope | Out of scope | Not supported: this build ships VST3 and Audio Units only. |
| Linux hosts | any | Linux | VST3 | Out of scope | Out of scope | Not supported: this build ships Windows and macOS plug-ins only. |
| Intel Macs | any | macOS | VST3 / AU | Out of scope | Out of scope | Not supported: the macOS build is Apple Silicon only. |
| Rosetta hosts | any | macOS | VST3 / AU | Out of scope | Out of scope | Not supported: the host has to run natively. |
<!-- END gen:daw-tiers:en -->

The table is generated from `docs/daw-tiers.json`; do not edit it by hand.

## What the evidence is

- **CI evidence** means the continuous-integration jobs passed on the build: the Windows job builds, runs the tests and pluginval at strictness 5 without a GUI; the macOS job also runs auval on both Audio Units, checks the bundle identity and instantiates the Audio Units in an out-of-process host. None of this loads Rinne into a DAW, so none of it says anything about how a DAW treats the pair.
- **A validation record** is a dated file in `docs/validation/` made from the template, with the host, system, build commit and the result of each acceptance item. Only these records can lift a host to Tier 1 or Tier 2.
- **Development testing** in a host (probe runs, local smoke runs) informs the design but is not an acceptance record and does not move a tier.

## Per-host notes

### Cubase Pro 15 (Windows, VST3)

Target: Tier 1. Current: Tier 3.

- Put Rinne Input in the first insert slot and Rinne Output above the fader divider of the same channel (the last pre-fader slot is simplest).
- ASIO-Guard can raise the minimum loop delay D; the MIN mark in Rinne Input shows the value in force.
- Not yet checked in the acceptance matrix: all seven items.

### LUNA 3.x (macOS, VST3 and Audio Unit)

Target: Tier 2. Current: Tier 3.

- VST3 scanning is off by default in LUNA; switch it on to use the VST3 build, or use the Audio Unit build.
- The Audio Unit build is beta: it passes auval and pluginval in CI and instantiates in an out-of-process host, but it has not been run inside LUNA.
- Not yet checked in the acceptance matrix: all seven items.

### REAPER 7

Waiting for its first test. A local smoke tool exists for development (see [the REAPER notes](testing/REAPER.md)); it renders a project unattended and counts as development testing only. It does not feed conclusions about other hosts.

### Bitwig

Waiting for its first test. Bitwig’s plugin hosting mode may load the two plugins into different processes. Rinne then shows OTHER PROCESS and warns that the delay may vary.

### Other hosts

Every other host in the table above is waiting for its first test. The placement rules and the notices in the interface apply everywhere; see the [user guide](USER_GUIDE.md#placement-and-the-edge-of-the-loop).

## Not supported

- **GarageBand**: sandboxed and out-of-process Audio Unit hosting is not tested.
- **Pro Tools (AAX), CLAP, VST2 hosts**: these formats are not built.
- **Linux hosts**: the build ships Windows and macOS plugins only.
- **Intel Macs and hosts running under Rosetta**: the macOS build is Apple Silicon only, and the host has to run natively.

## Report your host

If you tried Rinne in a host, tell us, whether it worked or not: open a Host report issue (it is in the issue chooser). A written test record helps most: copy [the template](validation/TEMPLATE.md), fill in the acceptance items you ran, and open a pull request that adds the record and updates `docs/daw-tiers.json`. If you would rather not use GitHub, mail the diagnostics from About (Copy diagnostics) and a few lines on what you did to contact@synchain.ca. For problems, see the known issues first: [KNOWN_ISSUES](KNOWN_ISSUES.md).
