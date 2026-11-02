# Rinne User Guide

## Read this first

<!-- BEGIN gen:safety-warning:en -->
> [!WARNING]
> Feedback can get loud — fast. Rinne deliberately feeds a channel back into itself. Start with Feedback at 0, keep monitors and headphones low while you experiment, and leave Rinne’s safety protection on. Rinne limits and sanitises the signal it feeds back and has a Panic button, but plugins you place inside the loop can still raise the level that reaches your speakers, and no software can guarantee a loop will never howl. Rinne comes with no warranty (GPL-3.0, sections 15–16).
<!-- END gen:safety-warning:en -->

<!-- BEGIN gen:hard-rules:en -->
1. **Placement**: Put Rinne Input on the first insert slot of the channel. Put Rinne Output after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot; if your host has no pre-fader section, the last insert slot). Every plug-in between them — including the host’s own insert effects — is inside the loop, and its own latency adds to the loop delay D; plug-ins after Output, the fader, pan and sends are not.
2. **Start from 0**: A new instance starts at 0% feedback, and the return fades in from 0 when a project or preset loads. Turn your monitors down, then raise feedback slowly from 0: the more return, the easier the loop self-oscillates. With Mix at 50% or more and nothing attenuating in the loop, feedback above 100% always self-oscillates; gain-adding plug-ins in the loop (EQ boosts, compressor make-up, saturation) can make it happen lower. The limiter and runaway protection are on by default, but they can’t protect you from sustained loud levels.
3. **Cut it first**: If you hear howling or anything odd, press Panic first: it cuts only the return, and the dry signal keeps playing; restore it with Restore return in the notice bar. With the window closed, the fastest way is to bypass Rinne Input in the host. The fader isn’t in the loop — pulling it down only hides the sound while the loop keeps growing, and stopping playback or muting won’t stop self-oscillation either.
4. **One pair per group**: A group (A–H) holds one Rinne Input and one Rinne Output, and they connect only when their groups match. Duplicating a track copies its group too: the copy shows CONFLICT — click Move to free group on the copy’s Rinne Input, then Re-pair on the copy’s Rinne Output.
<!-- END gen:hard-rules:en -->

## Install

Download and installation steps, including the extra unlock step on macOS, are in [INSTALL](INSTALL.md). Install both plugins, always the same version, then rescan plugins in your host. Which hosts have been looked at, and how closely, is in [host compatibility](DAW_COMPATIBILITY.md).

## Five-minute start

1. Pick a channel with some sound on it. Put Rinne Input in the first insert slot.
2. Put the plugins you want in the loop after it (a delay, a filter, a distortion), then Rinne Output after the last of them, above the fader divider.
3. Check the lamp on both plugins: it should say CONNECTED. If not, see "Pairing" below.
4. Turn your monitors down. Start playback and give it a moment: the loop is cleared when playback starts, and the return needs at least one loop delay D to come around.
5. Raise Feedback slowly from 0 while you listen. Set LOOP DELAY D and MIX to taste.
6. If anything sounds wrong, press PANIC (it cuts only the return), or bypass Rinne Input in the host.

## Placement and the edge of the loop

Rinne Input is the return end: it reads back what arrives from the loop and mixes it into the channel. Rinne Output is the send end: it passes the host's audio through untouched and copies it into the loop. What lies between them is the loop.

| Position | What goes there | In the loop? |
|---|---|---|
| First insert slot | Rinne Input | Start of the loop |
| Slots after it | The plugins you want in the loop, including the host's own insert effects | Yes |
| After the last of those | Rinne Output | End of the loop |
| Slots after Rinne Output | Anything else | No |
| Fader, pan, sends | Mixer controls | No |

Keep Rinne Output above the fader divider in the insert chain (simplest: the last pre-fader slot). If your host has no pre-fader section, use the last insert slot. The latency of every plugin in the loop adds to the loop time, so the loop takes D plus that latency to come around.

Rinne Output also has to be after Rinne Input. If the order is reversed, the notice bar says "Plug-in order reversed: move Rinne Output later."

## Pairing

The two plugins find each other through a group, A to H. They connect only when both are in the same group, and a group holds one Rinne Input and one Rinne Output. A new plugin picks a group for itself (a new Rinne Output follows the most recent unpaired Rinne Input, preferring one on a track with the same name) and marks it AUTO; click any group to choose by hand.

Below the group row Rinne shows the track names it can see: "Track names match", "Different track names" or "Can’t verify". Matching names are a good sign that both ends are on one track. A host that gives no names is fine; it only means Rinne cannot check.

The lamp tells you where pairing stands:

| Lamp | Meaning | What to do |
|---|---|---|
| CONNECTED | Loop closed, both ends found | Nothing |
| CONNECTED (UNVERIFIED) | Connected, but the host gives no track names | Nothing; use is not affected |
| WAITING FOR AUDIO | The host has not sent audio yet | Start playback |
| NO OUTPUT (on Input), NO INPUT (on Output) | The partner is not in this group | Insert it and choose the same group |
| CONFLICT | The group already has another Input (or Output) | Click Move to free group on the Input, then Re-pair on the Output |
| NO GROUP | All 8 groups are taken | Remove or regroup a pair |
| LOST | No reply from the other end for 2 s; the return faded out | Check that the other plugin is not bypassed or removed |
| OTHER PROCESS | The two ends run in different processes; the delay may vary | Put both on one track if you can |
| OTHER TRACK? | Track names differ; the loop delay may vary | Check that both are where you meant |
| VERSION MISMATCH | The two plugins are different versions | Install the same version of both |
| SAMPLE RATE MISMATCH | The ends run at different sample rates; the return stopped | Make both ends run at the same sample rate |
| CANNOT CONNECT | Shared memory is unavailable (for example in a sandboxed host); loop off, dry only | Hosts that sandbox plugins are not supported yet; otherwise report it with Copy diagnostics |
| VALIDATION MODE | Running inside a plugin validator; loop off | Nothing; this is expected there |
| BYPASSED (on Output) | The host bypassed Rinne Output; it sends silence | Switch bypass off |

Duplicating a track copies the group too. The copy shows CONFLICT: click Move to free group on the copy’s Rinne Input, then Re-pair on the copy’s Rinne Output. If the notice bar offers Take over, the group is held by an idle instance; taking over makes that instance lose the group.

## Rinne Input controls

All controls live in Rinne Input. From top to bottom:

- **GROUP**: the group A–H described above.
- **Feedback** (FEEDBACK): the gain of the return, 0 to 150 %, starting at 0. Drag to adjust, Shift for fine, double-click to reset, Ctrl-click (Cmd on Mac) to type a value. Above 100 % the dial turns amber and the chip reads SELF-OSC. FADING IN shows while the return fades in, CUT while it is cut. With the limiter off, feedback is capped at 100 % ("Set 120%, effective 100%").
- **LOOP**: switches the loop on and off. Off fades the return out without clearing the loop. It is a host parameter, so it can be automated or mapped to a controller.
- **MIX** (DRY to RETURN): the balance between the dry signal and the return. Towards DRY the return is turned down, towards RETURN the dry signal is turned down; at 50 % both are at full level. The default is 25 %.
- **LOOP DELAY D**: the time for one trip around the loop, 1 to 4000 ms. Choose MS, SAMPLES or SYNC (a note value at the host tempo, with triplet and dotted values). The MIN mark shows the lowest usable D.
- **RETURN TONE**: LOW CUT (OFF at the lowest setting), HIGH CUT (OFF at the highest) and Ø INVERT POLARITY. They act on the return only; the dry signal is untouched.
- **SAFETY**: PANIC, LIMITER with its ceiling (−24 to 0 dBFS, default −6), the GAIN REDUCTION meter, the DC, NaN and RUNAWAY lamps, and RUNAWAY PROTECTION.
- **LEVELS**: IN, RETURN and OUT meters. RETURN is measured before the feedback gain, so it moves even at 0 % feedback; use it to check that the loop is closed.
- **PRESET**: factory presets and your own; see "Presets".

The host lists these controls as parameters; the table below gives their IDs, display names, ranges and defaults. Panic, Restore return, the group and Re-pair are actions in the interface, not host parameters.

<!-- BEGIN gen:params-table:en -->
| ID | Display name | Range | Default | Unit | Automatable |
| --- | --- | --- | --- | --- | --- |
| feedback | Feedback | 0…150 | 0 | % | Yes |
| loop_on | Loop | 0…1 | 1 |  | Yes |
| mix | Mix | 0…100 | 25 | % | Yes |
| delay_mode | Delay Mode | 0…1 | 0 |  | No |
| delay_ms | Loop Delay | 1…4000 | 100 | ms | No |
| delay_note | Sync Note | 0…15 | 10 |  | No |
| low_cut | Low Cut | 20…2000 | 20 | Hz | Yes |
| high_cut | High Cut | 1000…20000 | 20000 | Hz | Yes |
| polarity | Invert Polarity | 0…1 | 0 |  | Yes |
| limiter | Return Limiter | 0…1 | 1 |  | No |
| ceiling | Ceiling | -24…0 | -6 | dB | No |
<!-- END gen:params-table:en -->

## Rinne Output

Rinne Output has no knobs. It shows the group, the pairing lamp, the track names, and a sending line such as "SENDING · ST · 48 kHz". While the host has it bypassed it reads "BYPASSED: WRITING SILENCE" and the return is silent. If the host is not processing audio it reads "HOST NOT PROCESSING: PAUSED".

It passes the host's audio through untouched, so putting it in a chain does not change the sound of the channel. Plugins in the loop that output NaN or Inf are cleaned on the way into the loop: the invalid values become 0 and are counted on the NaN lamp of Rinne Input.

## Loop delay D

D is a fixed number of samples. The loop is designed not to depend on buffer size or export mode, and both plugins report no latency to the host, so the host does not compensate for the loop.

- **Minimum.** D cannot be lower than the larger of the host’s declared maximum block and the largest block seen in this session. The MIN mark shows it; it changes with your buffer settings, and Cubase’s ASIO-Guard can raise it. If you ask for less, Rinne runs at the minimum and shows "Actual".
- **Total.** The loop time is D plus the latency of every plugin in the loop (the interface says "Total = D + in-loop plug-in latency").
- **SYNC.** Note values follow the host tempo; with no tempo available Rinne uses 120 BPM and says so.
- **Changing D.** While audio plays, Rinne crossfades between the old and the new delay over 40 ms.
- **Cap.** D is capped at 4000 ms.

## Safety mechanisms

### What the return passes through

Every sample that comes back goes through the same chain, in this order: cleaning of non-finite values, DC blocking, the low cut, the high cut, polarity, the feedback gain, a level check, the limiter, a soft clipper and a last finiteness check. If anything in the chain fails, the return becomes 0 and the dry signal keeps playing.

### Panic and Restore return

PANIC fades the return to 0 at once and keeps it cut; the dry signal keeps playing. Clicking it again does not restore. To restore, use Restore return in the notice bar, which then fades the return in from 0 over 500 ms. After a cut there is a short wait of about 1.5 s before Restore return works. The chip reads CUT, with the cause: manual, invalid values, level too high, or runaway.

Panic is a button in the interface, not a host parameter, so it cannot be mapped to a controller. With the window closed, the fastest stop is to bypass Rinne Input in the host.

### Fade-in

The return fades in over 500 ms, starting from the first valid sample, after each of these: the plugin first becoming ready, a new pairing, a new writer taking over, loading a preset or the host restoring a saved state (a project, A/B or undo), and Restore return. Feedback starts at 0 in a new instance.

Turning Loop back on, or taking Rinne Input out of bypass, is not one of these: there is no 500 ms fade-in, and the return comes back to its previous level within about 10 ms, with whatever the loop still holds. If Feedback is set high, lower it before you turn Loop back on.

### Runaway protection

When the return keeps overloading, RUNAWAY PROTECTION lowers feedback automatically (6 dB per second) and lets it come back after 3 s of calm. It stays out of the way at 90 % feedback or less with no gain in the loop. If it cannot stop the runaway at −24 dB, it cuts the return. A return level that stays above +12 dBFS for 100 ms cuts it and clears the loop. A NaN or Inf produced inside Rinne Input cuts it at once and clears the loop.

### Limits you should know

Steady self-oscillation at a moderate loop gain is held at the limiter ceiling; the runaway protection does not step in, and nothing stops sustained high levels. A hot, harmonically rich dry signal can trip the +12 dBFS check. These and other limits are listed in [KNOWN_ISSUES](KNOWN_ISSUES.md).

## Export, render and Freeze

- **Same loop time.** D is a fixed number of samples, so offline export and realtime playback use the same loop time. Rinne’s sound does not depend on whether the host is rendering offline; it only uses that fact to watch for export problems.
- **Clean start.** By default the loop is cleared when playback starts ("Clear the loop when playback starts" under ADVANCED in About), so every playback or export begins from an empty loop.
- **Fade-in at the start.** An export that starts within about 0.5 s after the project loads or the pair connects has a faded-in return at its start, unlike later realtime playback. This is the price of the fade-in rule above. Wait a second after loading or re-pairing before you export.
- **Export notices.** If the host breaks its own limits, the notice bar says "Last export may differ" with the cause: oversized blocks, D below the block size, the Output host broke its limit, or the loop resynced. Raise D and export again.
- **Freeze and Render in Place.** How a host’s Freeze or Render in Place treats the paired plugins has not been tested. Listen to the result, and compare it with a normal export.

## Host setup

### Cubase

Put Rinne Input in the first insert slot. Rinne Output goes above the fader divider of the same channel (the last pre-fader slot is simplest). ASIO-Guard can raise the minimum D; the MIN mark shows by how much. Cubase has not been through the written acceptance test yet; see [host compatibility](DAW_COMPATIBILITY.md) for the current status.

### LUNA

VST3 scanning is off by default in LUNA, so switch it on if you want the VST3 build, or use the Audio Unit build, which is beta. LUNA has not been tested yet.

### Other hosts

Other hosts are untested. The placement rules and the notices in the interface are the same everywhere. If something looks wrong, copy the diagnostics (see "Interface") and report it.

## Interface

- **First run.** A card asks for the language, then the rules page appears. Choose Got it, start the tour to see a short tour, or Skip the tour.
- **Language.** Use the 中, EN and FR buttons.
- **Rules and tour again.** Click the "?" button at the bottom right.
- **Zoom.** Pick a zoom level; Rinne applies it and asks you to confirm within 10 s with Save, otherwise it reverts; Cancel reverts too. Under WebKit older than 26.4 the zoom is fixed at 100 %.
- **About.** Click the version number for licences, ADVANCED settings (Clear the loop when playback starts, Tail mode: Auto, Infinite or 30 s, Record a diagnostics log), Copy diagnostics and View the rules.

Tail mode tells the host how long the tail is, so that it does not suspend the plugin during silence and break the loop. Auto means infinite for VST3 and 30 s for Audio Unit.

## Presets

PRESET holds FACTORY presets and MY PRESETS. Save as new preset… stores the current settings; a preset can be overwritten or deleted from the same menu. Names cannot contain \ / : * ? " < > |. Presets do not include Loop, the group, or the limiter switch and ceiling. Loading a preset fades the return in from 0.

## Privacy and files on disk

Rinne itself makes no network connections, needs no account and collects no data. It writes only these files:

- Interface defaults (language, rules acknowledged, tour dismissed, zoom): `ui-defaults.settings` under `%APPDATA%\Synchain\Rinne\` on Windows and `~/Library/Application Support/Synchain/Rinne/` on macOS.
- Your presets: the `Presets` folder next to it.
- Windows only: the WebView2 user data under `%LOCALAPPDATA%\Synchain\Rinne\WebView2`.
- macOS only: a small lock folder, `~/Library/Application Support/Synchain/Rinne/ipc`, used for pairing.
- A diagnostics log, only if you switch on Record a diagnostics log (off by default): `%LOCALAPPDATA%\Synchain\Rinne\logs\` on Windows and `~/Library/Logs/Synchain/Rinne/` on macOS, one file per plugin, rotated at 1 MiB.
- View licence writes `Rinne-LICENSE.txt` to the temporary folder and opens it.

Copy diagnostics puts text on the clipboard; nothing is sent. It contains versions, system, host, audio settings and loop state, no file paths or user names; track names appear only as whether they match, plus an 8-character hash. If you have no GitHub account, you can send it to contact@synchain.ca.

## Troubleshooting

### No return heard: five steps

1. **Look at the lamp.** Both plugins should say CONNECTED. If not, follow the table under "Pairing".
2. **Read the notice bar.** It says why the return is silent: Panic on, Loop off, bypassed by the host, Rinne Output bypassed, host paused processing, or a cut. If the chip reads CUT, click Restore return.
3. **Check Feedback and Mix.** Feedback starts at 0, and at 0 the return adds nothing. Raise it a little. Make sure MIX is not all the way at DRY. The RETURN meter moves even at 0 % feedback, so if it moves, the loop is closed.
4. **Check the order.** Rinne Input first, the plugins you want in the loop, then Rinne Output, above the fader divider. Look for "Plug-in order reversed" and the track-name notices.
5. **Wait one loop.** Playback clears the loop, so the return needs at least D to come around. Then check the notices about sample rate, block size and sandboxing. If it is still silent, use Copy diagnostics and report it.

### Map LOOP to a Quick Control

LOOP is a host parameter named "Loop", so most hosts let you assign it to a controller button; Cubase calls this Quick Controls, other hosts call it remote control or MIDI learn. Set the button so that it turns Loop off. Then one press fades the return out. Pressing it again brings the return back within about 10 ms, with no 500 ms fade-in, so lower Feedback first if it is high.

Know what it is and is not. Loop off fades the return out; it is not latched and it does not clear the loop, and turning Loop on again lets the return come back. For real trouble use PANIC: it cuts and clears, and it needs Restore return. Panic itself cannot be mapped.

### Other symptoms

- **Howling or a fast build-up.** Press PANIC, lower Feedback, and check which plugin in the loop adds gain.
- **The loop is longer than D.** Plugins in the loop add their own latency to D.
- **A silent or odd start in an export.** See "Export, render and Freeze".
- **Only a simple panel.** If the Microsoft Edge WebView2 Runtime is missing or too old, or the interface fails to start, Rinne shows a simple panel. Its Panic, Resume and Loop buttons work without the interface, but only on Rinne Input; on Rinne Output they are disabled. Use Install WebView2 Runtime or Retry.

## FAQ

- **Why does Feedback start at 0?** Because a loop can grow very fast. A new instance, a new pairing and a loaded preset all start from 0.
- **Can Output be on a different track?** It can, but the delay may then vary and the interface says so.
- **Does Rinne add latency in the host?** No: both plugins report no latency, so the host does not shift anything. The loop itself takes D plus the latency of the plugins inside it.
- **Where do I report a problem?** On the project’s issue tracker, or by mail to contact@synchain.ca if you have no GitHub account.
- **Is the macOS build signed?** No. It is not signed with an Apple Developer ID and not notarized; see [INSTALL](INSTALL.md).
- **Why can’t I map Panic?** It is an interface action by design; map Loop for a hardware stop.

## Glossary

- **Dry**: the signal that comes into Rinne Input from the host.
- **Return**: the signal that comes back around the loop, shaped by the return tone and the feedback gain.
- **Loop delay D**: the fixed time for one trip around the loop.
- **Group**: one of A–H, which pairs a Rinne Input with a Rinne Output.
- **Self-oscillation**: the loop sustaining and growing by itself because the return is as loud as or louder than what feeds it.
- **Tail**: how long the plugin tells the host it keeps making sound after the input stops.
- **Pre-fader**: the insert slots above the fader divider of a mixer channel.

VST® is a registered trademark of Steinberg Media Technologies GmbH. Cubase, LUNA, Logic Pro, GarageBand, macOS, Audio Units, Windows, WebView2 and other trademarks belong to their respective owners.
