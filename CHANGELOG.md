# Changelog

All notable user-facing changes to Rinne are recorded here. The format follows
[Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). The version number comes from `project(Rinne VERSION …)` in
`CMakeLists.txt`.

## [Unreleased]

### Added

- Export mismatch counting. In an offline render, each block that can make the export differ from live playback counts
  one event: the host calls with a block longer than declared, the main read head has to be zero-filled, the peer
  reports an over-long block, or the loop resynchronises (play start, first alignment and binding changes do not
  count). A clean export, with blocks no longer than the maximum, counts nothing. The audio thread only increments a
  counter and the acknowledged value lives on the message thread, so a hint can be acknowledged and comes back on the
  next new event. The count is in the status snapshot; the editor is not wired to it yet.

### Compatibility

- Changing the loop delay D now crossfades two read heads linearly over 40 ms, replacing the old head's 5 ms fade-out and
  the new head's 20 ms onset; further changes within 100 ms are merged. Beat-sync D is computed from the unrounded tempo
  and rounded to the nearest sample, and an invalid host tempo keeps the last valid value of the session (120 BPM only
  if none was ever seen).
- The out-of-order check (Output processed before Input) now judges once per block, and clearing it takes 3 consecutive
  Input-first blocks instead of the first correct one. A block that cannot be judged (no host timestamp, no new block
  from the peer, or a host timestamp that goes backwards, stands still or jumps ahead by more than 0.25 s) no longer
  drops the verdict to unknown at once: it resets the streak and keeps the verdict for at most 1 s, counted in samples.
  Host timestamps that run backwards, as seen around an ASIO-Guard switch in Cubase, never vote.
- Rinne Output now reaches its own out-of-order verdict from the same evidence, which agrees with the Input side;
  previously only Rinne Input judged.
