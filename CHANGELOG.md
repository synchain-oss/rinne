# Changelog

All notable user-facing changes to Rinne are recorded here. The format follows
[Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). The version number comes from `project(Rinne VERSION …)` in
`CMakeLists.txt`.

## [Unreleased]

### Added

- Export mismatch counting. In an offline render, each block that can make the export differ from live playback counts
  one event: the host calls with a block longer than declared, the main read head has to be zero-filled, the peer
  reports an over-long block, or the loop resynchronises (play start, first alignment and binding changes do not count).
  The peer's flag is read once per peer block; the first read after the slot was lost, the group changed or the binding
  changed only records the block number. A clean export, with blocks no longer than the maximum, counts nothing. The
  audio thread only increments a counter and the acknowledged value lives on the message thread, so a hint can be
  acknowledged and comes back on the next new event. The count is in the status snapshot; the editor is not wired to it
  yet.

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
