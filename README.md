**English** | [简体中文](README.zh-CN.md)

[![Release](https://img.shields.io/github/v/release/synchain-oss/rinne?include_prereleases&style=flat-square)](https://github.com/synchain-oss/rinne/releases)
[![License](https://img.shields.io/github/license/synchain-oss/rinne?style=flat-square)](LICENSE)

<!-- BEGIN gen:platform-badge:en -->
Windows x64 VST3 · macOS arm64 VST3/AU
<!-- END gen:platform-badge:en -->

# Synchain Rinne (回环)

> An open-source plugin pair that feeds a channel back into itself, with a loop delay you set.

*Rinne* is pronounced **RIN-neh** (/ˈrɪn.neɪ/), two syllables. It comes from Japanese 輪廻 (rinne, "cycle"), written with the same characters as Chinese 轮回. Chinese name: 回环.

> **Status: pre-release.** Single maintainer, best-effort support. Builds are published on the [Releases page](https://github.com/synchain-oss/rinne/releases).

<!-- BEGIN gen:safety-warning:en -->
> [!WARNING]
> Feedback can get loud — fast. Rinne deliberately feeds a channel back into itself. Start with Feedback at 0, keep monitors and headphones low while you experiment, and leave Rinne’s safety protection on. Rinne limits and sanitises the signal it feeds back and has a Panic button, but plugins you place inside the loop can still raise the level that reaches your speakers, and no software can guarantee a loop will never howl. Rinne comes with no warranty (GPL-3.0, sections 15–16).
<!-- END gen:safety-warning:en -->

## What it does

**Rinne** is a pair of audio plugins that turns the stretch of one mixer channel's insert chain between them into a feedback loop. **Rinne Input** goes in the first insert slot (the return end) and holds every control. **Rinne Output** goes after the last plugin you want inside the loop (the send end) and only handles pairing. Whatever the plugins between them output is sent back into Rinne Input after a loop delay *D* that you set, so a delay, a distortion, an EQ or a reverb inside the loop is heard again on every trip.

The loop delay is a fixed number of samples that is designed not to depend on buffer size or export mode, and both plugins report no plugin latency to the host. Feedback starts at zero. A safety chain on the return path limits and cleans the signal, and Panic cuts the return at any time.

Rinne is open source under the [GPL-3.0-or-later](LICENSE). It is made by [Synchain](https://synchain.ca). If you know Synchain's SCVB, note that its Input and Output point the other way round.

## Where the plugins go

<!-- BEGIN gen:placement:en -->
```
One mixer channel - inserts (Cubase shown)
+----------------------------------------------+
| 1   Rinne Input   <--- fed back after D --+  |   return end: mixes the loop back in
| 2   any plugin                            |  |   inside the loop
| 3   another plugin                        |  |   inside the loop
| ..  ...                                   |  |
| n   Rinne Output  ------------------------+  |   send end: last pre-fader slot
|----------- pre/post-fader divider -----------|
|     post-fader inserts                       |   outside the loop
+----------------------------------------------+
    fader / pan / sends                            outside the loop
```
<!-- END gen:placement:en -->

Not the same as SCVB: in Rinne, Input goes FIRST and Output goes LAST.

Rinne Output goes after the last plugin you want in the loop. Keep it above the fader divider in the insert chain (simplest: the last pre-fader slot). Every plugin between the two is in the loop, including the host's own insert effects; its latency adds to *D*. The fader, pan and sends are not.

## Requirements

- Windows 10 or 11 (x64) with the Microsoft Edge WebView2 Runtime.
- macOS 11 or later on Apple Silicon, with a host that runs natively (not under Rosetta). The interface uses the system WebKit (15.4 or later).
- A VST® 3 host; on macOS also an Audio Unit host. GarageBand is not supported.

## Supported hosts

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

Tiers go up only with a written test record in [docs/validation/](docs/validation/TEMPLATE.md). Today no record of a real-host test is in the repository: the two hosts being tested first, Cubase Pro 15 on Windows and LUNA on macOS, stand at Tier 3 (CI evidence only), every other host is still waiting for its first test, and the macOS Audio Unit builds are beta. Details, definitions and per-host notes are in [DAW_COMPATIBILITY](docs/DAW_COMPATIBILITY.md).

## Install

1. Download the zip for your system from the [Releases page](https://github.com/synchain-oss/rinne/releases), not "Source code". The authoritative SHA-256 is in the release notes; compare it before you install.
2. Windows: unblock the zip (the command below), extract it and copy both `.vst3` folders to `%COMMONPROGRAMFILES%\VST3\`.
3. macOS: the builds are not signed with an Apple Developer ID and not notarized, so unlock them first (the command below), run `install-macos.sh` from the zip, and only then open your host.
4. Rescan plugins in your host. Install both plugins, always the same version.

```powershell
Unblock-File .\Rinne-v<X.Y.Z>-win64.zip
```

```sh
xattr -dr com.apple.quarantine ~/Downloads/Rinne
```

If macOS says it cannot verify the plugin, do not click "Move to Trash". Step by step, with upgrade and uninstall: [INSTALL](docs/INSTALL.md).

## Quick start

1. Put Rinne Input in the first insert slot of a channel and Rinne Output after the last plugin you want in the loop.
2. Check that the lamp says CONNECTED on both. They pair when their groups (A–H) match; a new plugin picks a group for itself.
3. Turn your monitors down. Feedback starts at 0.
4. Raise Feedback slowly and listen. Set the loop delay *D* and the Mix.
5. If anything goes wrong, press Panic first, or bypass Rinne Input in the host.

Read these before you start:

<!-- BEGIN gen:hard-rules:en -->
1. **Placement**: Put Rinne Input on the first insert slot of the channel. Put Rinne Output after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot; if your host has no pre-fader section, the last insert slot). Every plug-in between them — including the host’s own insert effects — is inside the loop, and its own latency adds to the loop delay D; plug-ins after Output, the fader, pan and sends are not.
2. **Start from 0**: A new instance starts at 0% feedback, and the return fades in from 0 when a project or preset loads. Turn your monitors down, then raise feedback slowly from 0: the more return, the easier the loop self-oscillates. With Mix at 50% or more and nothing attenuating in the loop, feedback above 100% always self-oscillates; gain-adding plug-ins in the loop (EQ boosts, compressor make-up, saturation) can make it happen lower. The limiter and runaway protection are on by default, but they can’t protect you from sustained loud levels.
3. **Cut it first**: If you hear howling or anything odd, press Panic first: it cuts only the return, and the dry signal keeps playing; restore it with Restore return in the notice bar. With the window closed, the fastest way is to bypass Rinne Input in the host. The fader isn’t in the loop — pulling it down only hides the sound while the loop keeps growing, and stopping playback or muting won’t stop self-oscillation either.
4. **One pair per group**: A group (A–H) holds one Rinne Input and one Rinne Output, and they connect only when their groups match. Duplicating a track copies its group too: the copy shows CONFLICT — click Move to free group on the copy’s Rinne Input, then Re-pair on the copy’s Rinne Output.
<!-- END gen:hard-rules:en -->

## Safety

- Feedback starts at 0, the limiter and the runaway protection are on by default, and the return fades in over 500 ms whenever it (re)starts.
- Everything fed back is cleaned (non-finite values become 0), blocked of DC, limited and soft-clipped; a failure silences the return and never makes it louder.
- Panic cuts only the return; the dry signal keeps playing.
- Rinne Output passes the host's audio through untouched and writes silence to the loop when bypassed.
- Rinne cannot know what the plugins you put in the loop do. They can raise the level that reaches your speakers, and sustained high levels are not stopped (see [KNOWN_ISSUES](docs/KNOWN_ISSUES.md)).

## Privacy

Rinne itself makes no network connections, needs no account and collects no data. Its interface is drawn by the Microsoft Edge WebView2 Runtime on Windows and by the system WebKit on macOS; these are operating-system components updated and configured by Microsoft and Apple (for example, Microsoft Defender SmartScreen settings apply to WebView2). Rinne does not change their behaviour. The files Rinne writes are listed in the [user guide](docs/USER_GUIDE.md#privacy-and-files-on-disk).

## Build from source

```powershell
git clone https://github.com/synchain-oss/rinne.git
cd rinne
pwsh scripts/build.ps1 -JucePath <JUCE checkout>
```

JUCE is pinned by commit and passed in, not downloaded. The full steps for Windows and macOS are in [BUILDING](docs/BUILDING.md).

## Known limitations

- The loop time is *D* plus the latency of every plugin in the loop (KI-1).
- Hosts that suspend plugins during silence can break the loop; set Tail mode to Infinite or turn that host option off (KI-3).
- An export that starts while the return is still fading in (about the first 0.5 s after load or pairing) begins with the fade-in (KI-5).
- Steady self-oscillation at moderate loop gain is held at the ceiling, not stopped; use Panic (KI-6).
- The macOS builds are Apple Silicon only, unsigned and unnotarized; Intel Macs, Rosetta hosts and GarageBand are not supported, and Logic Pro is not tested (KI-10, KI-11).

## Documentation

- [User guide](docs/USER_GUIDE.md): placement, controls, safety, export, troubleshooting.
- [Install](docs/INSTALL.md) and [Building](docs/BUILDING.md).
- [Host compatibility](docs/DAW_COMPATIBILITY.md) and [known issues](docs/KNOWN_ISSUES.md).
- [Architecture](docs/ARCHITECTURE.md), the [contracts](docs/contracts/PARAMETERS.md) and the [decision records](docs/adr/README.md).
- [Changelog](CHANGELOG.md).

## Contributing

See [CONTRIBUTING](CONTRIBUTING.md). Every commit must be signed off (`git commit -s`). Security reports go through [SECURITY](SECURITY.md), not public issues.

## License

The source code is [GPL-3.0-or-later](LICENSE). The binary as a whole is distributed under GPLv3; its JUCE portion is covered by AGPLv3 (GPLv3 §13). There is no warranty. The third-party components and their licences are listed in [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES.md).

## Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH. Cubase, LUNA, Logic Pro, GarageBand, macOS, Audio Units, Windows, WebView2 and other trademarks belong to their respective owners.

The GPL does not grant trademark rights (see GPLv3 §7(e)). The Synchain and Rinne names and logos are not licensed under the GPL; modified versions must not use them in a way that suggests they are official.

## Related projects

- [SCVB](https://github.com/synchain-oss/scvb): Synchain's vocal balancer plugin pair.
- [synchain.ca](https://synchain.ca): the project website.
