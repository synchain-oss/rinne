# Changelog

All notable user-facing changes to Rinne are recorded here. The format follows
[Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). The version number comes from `project(Rinne VERSION …)` in
`CMakeLists.txt`.

## [Unreleased]

### Compatibility

- Changing the loop delay D now crossfades two read heads linearly over 40 ms, replacing the old head's 5 ms fade-out and
  the new head's 20 ms onset; further changes within 100 ms are merged. Beat-sync D is computed from the unrounded tempo
  and rounded to the nearest sample, and an invalid host tempo keeps the last valid value of the session (120 BPM only
  if none was ever seen).
