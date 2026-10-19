# Changelog

All notable user-facing changes to Rinne are recorded here. The format follows
[Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/). The version number comes from `project(Rinne VERSION …)` in
`CMakeLists.txt`.

## [Unreleased]

### Compatibility

- The huge-value trip now counts a returned peak only when it exceeds the sent peak by more than 0.5 dB, so
  phase-only chains such as a first-order all-pass no longer trip it on hot dry signals (about +12 dBFS and above).
