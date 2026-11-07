# Host compatibility

## Summary

Rinne builds for Windows (x64, VST3) and macOS (Apple Silicon, VST3 and Audio Unit). This page says what has been checked in which host, and how far. It is deliberately conservative: a host only moves up a tier when a written test record in [docs/validation/](validation/TEMPLATE.md) backs it, and a host nobody has opened is listed as waiting for its first test, not as working.

Two hosts have a written real-host record so far. Cubase Pro 15 on Windows stands at Tier 2: all seven acceptance items passed on a preview build, and Tier 1 waits for the same matrix on a release build. LUNA on macOS has a first record for the Audio Unit builds that leaves one finding open, so it stays at Tier 3 until that is fixed and retested. Every other host is still waiting for its first test. The macOS Audio Unit builds are beta.

## Tier definitions

| Tier | Meaning | What it needs |
|---|---|---|
| Tier 1 | Fully verified | The release build passed the whole acceptance matrix below on a real host, and the measured loop period equals D |
| Tier 2 | Partly verified | A written record on a real host, covering only the items it lists |
| Tier 3 | Not verified on a real host | CI evidence only: build, tests, and the plugin validators (pluginval on both systems, auval for the Audio Units) |
| Out of scope | Not provided | The format or platform is not built, or the host is known to be unsuitable |
| Awaiting test | No tier yet | Nothing has been looked at in this host |

The acceptance matrix has seven items: realtime playback, offline export, Freeze, save and reopen, buffer sizes (64, 256, 1024 and 2048), ASIO-Guard on and off, and the loop period measured against D. Each record in `docs/validation/` states which items passed. A record that leaves a finding open on an item it covers does not lift the host: the row stays where it is until the finding is fixed and the item is checked again.

## Current matrix

<!-- BEGIN gen:daw-tiers:en -->
| Host | Version | Platform | Formats | Tier | Status | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Cubase Pro | 15 | Windows | VST3 | Tier 2 | Partial | Real-host records on two preview builds. All seven acceptance items passed (2026-11-03), and a Cubase insert effect inside the loop adds its delay to the loop delay (2026-11-07). Tier 1 needs the same matrix on a release build. |
| LUNA | 3.x | macOS | VST3 / AU | Tier 3 | Partial | First real-host record, on a preview build and the Audio Unit builds only. Loop period, buffer sizes 64, 512 and 2048, real-time against offline export, and save and reopen passed. One finding is open: the first echo after playback starts is 13 to 21 dB too quiet. Not run: the VST3 builds (LUNA lists only system-wide VST3 plug-ins), Bounce, uninstall and reinstall, Freeze. Stays at Tier 3 until the finding is fixed and retested. The Audio Unit build is beta. |
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

Target: Tier 1. Current: Tier 2.

- Put Rinne Input in the first insert slot and Rinne Output above the fader divider of the same channel (the last pre-fader slot is simplest).
- ASIO-Guard can raise the minimum loop delay D; the MIN mark in Rinne Input shows the value in force.
- [Record of 2026-11-03](validation/2026-11-03-cubase-pro-15-win.md), preview build 0.9.0-preview.1 (commit 968685ef): all seven acceptance items passed. Every echo interval equalled D at buffer sizes 64 to 2048 and with ASIO-Guard Off, Normal and High, and the realtime and the offline export agreed (residual peak −162.56 dBFS).
- [Record of 2026-11-07](validation/2026-11-07-cubase-pro-15-win.md), preview build 0.9.0-preview.2 (commit 51bec023): a Cubase insert effect between the two ends (MonoDelay, 10 ms) is inside the loop, and its delay adds to D.
- Not covered: 44.1 and 96 kHz, mono tracks, several projects open at once, and in-loop plug-ins other than that one effect. Tier 1 needs the matrix repeated on a release build.

### LUNA 3.x (macOS, VST3 and Audio Unit)

Target: Tier 2. Current: Tier 3.

- LUNA lists VST3 plug-ins only from the system folder `/Library/Audio/Plug-Ins/VST3`, not from the one in your user Library, and VST3 scanning is off by default (switch it on). The installer's default puts the plug-ins in your user Library, so for the VST3 build run `uninstall-macos.sh` first (so the same plug-in is not registered in two places), then `bash install-macos.sh --system`, which asks for an administrator password. The Audio Unit build is found in either place and does not need `--system`. Steps: [INSTALL](INSTALL.md#macos).
- The Audio Unit build is beta. [First record](validation/2026-11-07-luna-3-macos.md), 2026-11-07, preview build 0.9.0-preview.2 (commit 51bec023), LUNA 3.1.0.4581 on macOS 15.6.1: loop period, buffer sizes 64, 512 and 2048, realtime against offline export, and save and reopen passed.
- One finding is open: the first echo after playback starts is 13 to 21 dB too quiet (it was not seen in Cubase). The host stays at Tier 3 until it is fixed and the check repeated.
- Not run: the VST3 builds, Bounce, uninstall and reinstall, Freeze, and buffer sizes 256 and 1024.

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
