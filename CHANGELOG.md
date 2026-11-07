# Changelog

All notable user-facing changes to Rinne are recorded here. The format follows
[Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). The version number comes from `project(Rinne VERSION …)` in
`CMakeLists.txt`.

## [Unreleased]

### Added

- The permanent placement reminder on Rinne Output can now be dismissed. A "Got it" button on the red reminder hides it;
  the choice is a machine-wide setting, so every Rinne Output window, now and in later sessions, follows it (an open
  window within about a second), and it is not part of the project: it does not mark the project as changed. When the
  reminder is hidden, the rows below it move up evenly inside the same window size, and notices such as "Plug-in order
  reversed" still appear. A new "Show placement hint" switch under ADVANCED in About (Rinne Output only) brings the
  reminder back. The French reminder text is shorter to make room for the button.
- Export mismatch counting. In an offline render, each block that can make the export differ from live playback counts
  one event: the host calls with a block longer than declared, the main read head has to be zero-filled, the peer
  reports an over-long block, or the loop resynchronises (play start, first alignment and binding changes do not count).
  The peer's flag is read once per peer block; the first read after the slot was lost, the group changed or the binding
  changed only records the block number. A clean export, with blocks no longer than the maximum, counts nothing. The
  audio thread only increments a counter and the acknowledged value lives on the message thread, so a hint can be
  acknowledged and comes back on the next new event. The count is in the status snapshot; the editor is not wired to it
  yet.

### Changed

- New Latin interface fonts in both plug-ins. Rinne Sans is now Figtree at weight 500, with weight 600 for emphasis,
  and Rinne Mono is DM Mono at weight 500, replacing IBM Plex Sans and IBM Plex Mono at weight 400, which read too thin
  and stiff. Text widths are nearly unchanged and the window layout does not move. Chinese text still uses Noto Sans SC,
  and titles and large numbers still use Space Grotesk. The About panel and the third-party notices name the new
  sources; both fonts are under the SIL Open Font License 1.1.
- Rinne Output has a new "signal chain" layout. The pairing card now reads left to right as the Rinne Input track, a
  connecting line, and this track, and the whole window is symmetric about its centre axis. The line shows the pairing
  state at a glance: solid green when connected, solid amber when connected but worth a look (stale, crossed process or
  crossed track), dashed red when broken or unusable, dashed gray when still searching or bypassed. A track name wraps
  to two lines before it is shortened, with the full name kept in its tooltip. The group buttons no longer shift
  sideways when the "AUTO" badge appears or disappears. The send line shows "NOT IN THE LOOP" (and its Chinese and
  French equivalents) instead of a lone dash while this track is not connected to a loop. Labels use Rinne Sans, and
  Rinne Mono is kept for the send row, the version, the zoom level, the channel badge, the insert chain and small header
  text. The placement hint is Rinne Sans at weight 600 and its "Got it" button is shorter and sized to its label; the
  notice slot is 44 px high (42 px while the hint is shown) with its buttons side by side. Step 3 of the tour now
  highlights only the status row. The window size and every setting are unchanged.
- The zoom menu in the footer of both plug-ins is now drawn by the plug-in instead of the system. Clicking the zoom level
  opens a light panel above it, in the same style as the preset panel, instead of the white native list; the current
  level carries a check mark, and levels that do not fit the current screen are greyed out, labelled "Exceeds current
  screen" (and its Chinese and French equivalents), and ignore clicks. The arrow keys on the zoom button only open the
  panel and never change the zoom; inside the panel Up, Down, Home, End, Enter and Esc work, Esc returns focus to the
  button, and clicking outside or tabbing away closes it. Choosing the current level only closes the panel. Picking a
  new level still applies it at once and shows the 10-second keep-or-revert prompt. The panel never leaves the window at
  any zoom level and scrolls inside itself when it cannot fit.

### Compatibility

- Changing the loop delay D now crossfades two read heads linearly over 40 ms, replacing the old head's 5 ms fade-out and
  the new head's 20 ms onset; further changes within 100 ms are merged. Beat-sync D is computed from the unrounded tempo
  and rounded to the nearest sample, and an invalid host tempo keeps the last valid value of the session (120 BPM only
  if none was ever seen).
- The out-of-order check (Output processed before Input) now judges once per block, and clearing it takes 3 consecutive
  Input-first blocks instead of the first correct one. A block that cannot be judged (no host timestamp, no new block
  from the peer, or a host timestamp that goes backwards, stands still or jumps ahead by more than 0.25 s) no longer
  drops the verdict to unknown at once: it resets the streak and keeps the verdict for at most 1 s, counted in samples.
  Host timestamps that run backwards, as seen around an ASIO-Guard switch in Cubase, never vote. The first read of the
  peer's slot, and the first after the peer reappears or the group changes, only records its block number, so the
  first vote comes one block later. A block longer than 32768 frames neither votes nor breaks the streak, but still
  advances the timestamp baseline and the 1 s expiry.
- Rinne Output now reaches its own out-of-order verdict from the same evidence, which agrees with the Input side;
  previously only Rinne Input judged.
- The return is now 0 while the two ends run at different sample rates. When the sample rate that the writer publishes
  in the ring header differs from Rinne Input's own, the loop layer treats the ring as having no writer: the read head
  fades out over 10 ms through the existing no-peer path (the ring is not pulled on that first block, because the
  writer is still writing at the new rate), the dry signal is unchanged, and no new safety transition is added. Both
  ends round the sample rate to the nearest integer with one shared function before it is published or compared, so
  44055.944 Hz and 44056 Hz count as the same rate. When the rates match again it counts as a new pairing: the read
  head starts from the current write head, the return stays 0 for one loop delay and then fades in over 500 ms; frames
  written while the rates differed are never read. Previously the ring was read as if the rates matched, so the
  return played back pitch-shifted. The status snapshot reports `rateMismatch` and the writer's rate; the editor is not
  wired to them yet.
- A brief host report of "not playing" no longer counts as a play start. The host has to report a stop for at least
  max(100 ms, 4 times the largest block), counted in samples, before the next playing block is a play start (5 ms
  fade-out of the return and a reset of the local delay line). Cubase 15 occasionally reports three stopped blocks
  (about 16 ms) in the middle of playback, which used to cut the echo tail. The first playing block after both ends
  were suspended for more than 250 ms, or after a re-prepare while a stop report is in progress, is still a play
  start, even if the host never reported a stop.
- The dry gain of Rinne Input now moves smoothly instead of in steps. When Mix changes, and when the host bypasses or
  un-bypasses Rinne Input, the dry gain ramps linearly over 30 ms to its new value and lands on it exactly; it used to
  jump at the next block boundary, which clicked, at a sample that depended on how the host split its blocks. When the
  host bypasses Rinne Input, the return fades out over 10 ms and the dry gain returns to 1 over 30 ms, after which the
  dry signal passes through bit for bit; previously, with Mix above 50 %, the dry signal stayed attenuated after the
  bypass (silent at 100 %). Un-bypassing ramps the dry gain back to the Mix value over 30 ms. A Mix value that is NaN is
  now treated as 0: the dry signal is unchanged and the return is 0 (it used to give the return its maximum gain).

### Fixed

- The return no longer drops out and fades back in when nothing about the loop's segment changed: when a smart default
  claim is confirmed (for example by the first project save), when one end is re-prepared at the same sample rate, or
  when both ends resume after being stopped. The stream carries on sample for sample; after a long stop only the
  existing suspend flush applies. A real change of segment (another group, or claiming again after losing the slot)
  still rebuilds the read heads and fades in. The Output's writer no longer restarts its stall count on these events.
