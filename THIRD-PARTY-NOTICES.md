# Third-Party Notices

Rinne (the plugin pair Rinne Input and Rinne Output) is free software: its own source code is licensed under
GPL-3.0-or-later (see [`LICENSE`](LICENSE)). This file lists the third-party components that the release binaries
contain or that the build uses, with their licences, copyright lines and notice texts.

Rinne has not been released yet. Until the first release this file describes the planned composition of v1; it is
checked against the actual build logs before the first tag.

- Full licence texts are in [`LICENSES/`](LICENSES/), one SPDX standard text per identifier.
- Upstream licence and copyright notices, copied verbatim, are in [`third_party/notices/`](third_party/notices/);
  where each one comes from is listed in [`third_party/notices/README.md`](third_party/notices/README.md).
- Release zips carry this file, `LICENSE`, `LICENSES/` and `third_party/notices/` at the same relative paths.
- Version numbers come from the pin files named in the tables. Where a literal is given it is the current value of
  that pin file, read from the pinned source.

## Licensing at a glance

- **Source code**: GPL-3.0-or-later, `Copyright 2026 Synchain`.
- **Binaries**: distributed as a whole under GPLv3, with the JUCE parts remaining under AGPLv3 (GPLv3 §13). The
  binaries combine JUCE (AGPL-3.0-only) with the VST3 SDK (used under GPL-3.0-only), so they cannot be relicensed as
  "GPLv3 or later" as a whole.
- **Corresponding Source** (GPLv3 §6(d)): the Rinne tag a binary was built from; JUCE at the upstream tag in
  `.juce-version` and the 40-character commit in `.juce-commit` (currently 8.0.8,
  [`d6181bde38d858c283c3b7bf699ce6340c050b5d`](https://github.com/juce-framework/JUCE/tree/d6181bde38d858c283c3b7bf699ce6340c050b5d)),
  which also contains the VST3 SDK and the AudioUnitSDK; on Windows, the version of the WebView2 SDK NuGet package.
  Microsoft publishes the WebView2 loader as a binary only (see [WebView2 loader](#webview2-loader)), so this
  repository does not claim to hold the complete corresponding source of the Windows binaries.

## 1. Shipped in binaries

<!-- section:shipped-table -->

| Component | Version | SPDX | Ships in | Notice texts |
| --- | --- | --- | --- | --- |
| JUCE | `.juce-version` at `.juce-commit` (8.0.8) | `AGPL-3.0-only` | both plugins, all formats | `third_party/notices/juce.LICENSE.md` |
| JUCE JavaScript helper | with JUCE, verbatim copy in `web/js/juce/` | `AGPL-3.0-only` | both plugins (embedded web UI) | `third_party/notices/juce.LICENSE.md` |
| VST3 SDK | with JUCE (3.7.12 in JUCE 8.0.8) | `GPL-3.0-only`, `BSD-3-Clause` | `.vst3` | `third_party/notices/vst3sdk.LICENSE.txt`, `third_party/notices/vst3sdk-base.LICENSE.txt` |
| AudioUnitSDK | with JUCE | `Apache-2.0` | `.component` (macOS) | `third_party/notices/audiounitsdk.LICENSE.txt` |
| zlib | with JUCE `juce_core` (1.3.1) | `Zlib` | both plugins, all formats | `third_party/notices/zlib.LICENSE` |
| libpng | with JUCE `juce_graphics` (1.6.37) | `libpng-2.0` | both plugins, all formats | `third_party/notices/libpng.LICENSE` |
| libjpeg | with JUCE `juce_graphics` (6b) | `IJG` | both plugins, all formats | `third_party/notices/libjpeg.README` |
| HarfBuzz | with JUCE `juce_graphics` (10.1.0) | `MIT-Modern-Variant` | both plugins, all formats | `third_party/notices/harfbuzz.COPYING` |
| SheenBidi | with JUCE `juce_graphics` | `Apache-2.0` | both plugins, all formats | `third_party/notices/sheenbidi.LICENSE` |
| Unicode data tables | generated tables in HarfBuzz, SheenBidi and JUCE | `Unicode-3.0` | both plugins, all formats | `third_party/notices/unicode.license.txt` |
| WebView2 loader | `WEBVIEW2_VERSION` in CMake (1.0.2957.106) | `BSD-3-Clause` | Windows only | `third_party/notices/webview2.LICENSE.txt`, `third_party/notices/webview2.NOTICE.txt` |
| Space Grotesk | 2.000, subset | `OFL-1.1` | both plugins (embedded web UI) | `third_party/notices/space-grotesk.OFL.txt` |
| Rinne Sans | IBM Plex Sans 3.201, subset and renamed | `OFL-1.1` | both plugins (embedded web UI) | `third_party/notices/ibm-plex-sans.OFL.txt` |
| Rinne Mono | IBM Plex Mono 2.3, subset and renamed | `OFL-1.1` | both plugins (embedded web UI) | `third_party/notices/ibm-plex-mono.OFL.txt` |
| Noto Sans SC | 2.004, subset | `OFL-1.1` | both plugins (embedded web UI) | `third_party/notices/noto-sans-sc.OFL.txt` |

The rows are derived from the JUCE modules and packages that the build links. `scripts/lib/rules.mjs`
(`THIRD_PARTY_BY_MODULE`) maps each of them to the components above, and the `modules-closure` and
`licenses-closure` checks keep that map, this table, `LICENSES/` and `third_party/notices/` in step.

<!-- /section:shipped-table -->

## 2. Build, test and CI only

Not linked into the plugins and not distributed with them.

| Component | Version | SPDX | Used for | Notice texts |
| --- | --- | --- | --- | --- |
| Catch2 | FetchContent tag in `tests/CMakeLists.txt` (v3.5.4) | `BSL-1.0` | unit-test executables | `third_party/notices/catch2.LICENSE.txt` |
| github-slugger slug rules | vendored in `third_party/github-slugger/` | `ISC` | the documentation link check | — |
| pluginval | `.pluginval-version` | `GPL-3.0-only` | plugin validation in CI and local gates; downloaded, not linked | — |

## 3. Component notes

<!-- section:juce -->

### JUCE

- Copyright line: `Copyright (c) Raw Material Software Limited`.
- JUCE is dual-licensed under the AGPLv3 and the commercial JUCE licence (see `juce.LICENSE.md`). Rinne uses it under
  the AGPLv3. The module headers grant "the terms of the AGPLv3" with no "or any later version", so the SPDX
  identifier is `AGPL-3.0-only`.
- Modules linked into the plugins: `juce_audio_basics`, `juce_audio_processors`, `juce_audio_plugin_client`,
  `juce_core`, `juce_data_structures`, `juce_events`, `juce_graphics`, `juce_gui_basics` and `juce_gui_extra`. The
  third-party code they bring in has its own row in the table above.
- Dependencies listed in JUCE's `LICENSE.md` that are **not** in Rinne's binaries: Oboe, FLAC, Ogg Vorbis, GLEW
  (with Mesa and Khronos), CHOC (with QuickJS), Box2D, LV2 and AAX. They live in modules Rinne does not link, or
  belong to plugin formats and platforms Rinne does not build. The `pslextensions` headers that come with the VST3
  wrapper are in the public domain.
- The JavaScript helper in `web/js/juce/` is a verbatim copy of JUCE's own helper. It keeps JUCE's copyright and
  licence, carries no Synchain header, and `REUSE.toml` records it as `AGPL-3.0-only`.
- Rinne does not modify JUCE. If a patch is ever needed it goes into `third_party/juce-patches/`, keeps JUCE's licence,
  and this note marks JUCE as modified.
- The AGPLv3 network clause (section 13) is not triggered in practice: the plugins do not offer a service over a
  network. It still applies to the combined work, and the corresponding source above covers it.

<!-- /section:juce -->

<!-- section:vst3-sdk -->

### VST3 SDK

- JUCE 8.0.8 bundles VST3 SDK 3.7.12 in `modules/juce_audio_processors/format_types/VST3_SDK/`. It has two licence
  texts, both from Steinberg Media Technologies GmbH (`(c) 2024, Steinberg Media Technologies GmbH, All Rights Reserved`):
  - the top-level and `pluginterfaces/` `LICENSE.txt` (`vst3sdk.LICENSE.txt`): the proprietary Steinberg VST3 License
    **or** the GPL version 3. Rinne uses the GPLv3 option (`GPL-3.0-only`) and has not signed Steinberg's licence
    agreement;
  - the `base/` and `public.sdk/` `LICENSE.txt` (`vst3sdk-base.LICENSE.txt`): a BSD 3-clause licence. JUCE's VST3
    wrapper compiles sources from both directories into every `.vst3`.
- Because the GPLv3 option is "version 3" only, the binaries as a whole are distributed under GPLv3 and not "GPLv3 or
  later".
- JUCE 8.0.11 and later bundle VST3 SDK 3.8.0 under the MIT licence. Changing the JUCE version is a licensing change:
  the same pull request updates `.juce-version`, `.juce-commit`, this file, `LICENSES/`, `third_party/notices/`,
  `THIRD_PARTY_BY_MODULE` and ADR-0007.
- Rinne does not use the VST logo. VST is used only as the name of the plugin format.

<!-- /section:vst3-sdk -->

<!-- section:au-sdk -->

### AudioUnitSDK

- Bundled with JUCE in `modules/juce_audio_plugin_client/AU/AudioUnitSDK/`, compiled only into the Audio Unit
  (`.component`) on macOS.
- Licence: Apache License 2.0 (`audiounitsdk.LICENSE.txt`). Copyright line, from the source headers:
  `© 2000-2021 Apple Inc. All rights reserved.`
- The upstream directory has no `NOTICE` file. JUCE records its changes to the SDK sources in `JUCE CHANGES.txt` in
  the same directory (Apache-2.0 section 4(b)); that file is part of the JUCE corresponding source above.

<!-- /section:au-sdk -->

<!-- section:webview2 -->

### WebView2 loader

- The Windows plugins host their user interface in Microsoft Edge WebView2. They statically link the WebView2 loader
  from the `Microsoft.Web.WebView2` NuGet package (version `WEBVIEW2_VERSION` in CMake, currently 1.0.2957.106).
- Licence: BSD 3-clause, `Copyright (C) Microsoft Corporation. All rights reserved.` (`webview2.LICENSE.txt`, from
  the package). The package also contains `NOTICE.txt`, reproduced as `webview2.NOTICE.txt`.
- Microsoft publishes the loader as a binary only; there is no source code for it to provide. Rinne treats it as
  part of the WebView2 platform component under the GPLv3 definition of System Libraries, and attaches no additional
  linking exception. For the same reason this repository does not claim to hold the complete corresponding source of
  the Windows binaries.
- The WebView2 Runtime itself is not distributed with Rinne; it is installed on the user's system by Microsoft. On
  macOS the plugins use the system WKWebView and ship nothing for it.

<!-- /section:webview2 -->

<!-- section:fonts -->

### Fonts

- The embedded web UI uses four font families under the SIL Open Font License 1.1. They are subset from the upstream
  fonts and compiled into both plugins.
- **Rinne Sans** and **Rinne Mono** are IBM Plex Sans and IBM Plex Mono. "Plex" is a Reserved Font Name and
  subsetting counts as a modification, so under OFL-1.1 §3 the subset files carry a different name: the font names in
  the `name` table (name IDs 1, 3, 4, 6, 16 and 17), the file names and the CSS family names say Rinne Sans / Rinne
  Mono. The copyright and licence entries (name IDs 0, 13 and 14) are kept unchanged.
- **Noto Sans SC** has the Reserved Font Name "Source", which the subset name does not use. **Space Grotesk** has no
  Reserved Font Name.
- Copyright lines, from the first line of each upstream `OFL.txt` (the Reserved Font Name statement is only there,
  not in the subset files, which is why the upstream texts ship with the binaries):

  | Family | Copyright line |
  | --- | --- |
  | Space Grotesk | `Copyright 2020 The Space Grotesk Project Authors (https://github.com/floriankarsten/space-grotesk)` |
  | IBM Plex Sans (Rinne Sans) | `Copyright © 2017 IBM Corp. with Reserved Font Name "Plex"` |
  | IBM Plex Mono (Rinne Mono) | `Copyright © 2017 IBM Corp. with Reserved Font Name "Plex"` |
  | Noto Sans SC | `Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'` |

- The font files are built by `scripts/fetch_fonts.py` from the upstream files in google/fonts at commit
  `bd8f81ddb5c74d5c8897b36ad88b440266245103` (the script checks each source file's SHA-256). Changes made to the
  upstream fonts: subsetting to the characters the interface uses plus common Chinese characters and the Latin-1 and
  Latin Extended-A blocks; fixing the variable axes of Space Grotesk (weight 600) and IBM Plex Sans (weight 400, width
  100) and narrowing the weight axis of Noto Sans SC to 300–700; renaming IBM Plex Sans and IBM Plex Mono as above;
  mapping U+202F (narrow no-break space) to the thin-space or no-break-space glyph in the fonts that lack it.
- The copyright line in each subset file's `name` table (name ID 0) is recorded in `REUSE.toml`, one block per file
  (`check-font-names.py` compares them):

  | File | Name ID 0 |
  | --- | --- |
  | `web/fonts/SpaceGrotesk.woff2` | `Copyright 2020 The Space Grotesk Project Authors (https://github.com/floriankarsten/space-grotesk)` |
  | `web/fonts/RinneSans.woff2` | `Copyright 2019 IBM Corp. All rights reserved.` |
  | `web/fonts/RinneMono.woff2` | `Copyright 2017 IBM Corp. All rights reserved.` |
  | `web/fonts/NotoSansSC.woff2` | `(c) 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.` |

<!-- /section:fonts -->

<!-- section:catch2 -->

### Catch2

- Used only by the unit-test executables, fetched at configure time at the pinned tag. Not distributed.
- Licence: Boost Software License 1.0 (`catch2.LICENSE.txt`). Copyright line, from the source headers:
  `Copyright Catch2 Authors`.

<!-- /section:catch2 -->

<!-- section:github-slugger -->

### github-slugger

- The documentation link check computes GitHub heading anchors with the slug rules of github-slugger (ISC licence).
  The rules are vendored into `third_party/github-slugger/` together with `LICENSES/ISC.txt`; they are not part of the
  plugins. Not vendored yet.

<!-- /section:github-slugger -->

## 4. Code derived from other Synchain projects

<!-- section:scvb-derived -->

Some files are adapted from [synchain-oss/scvb](https://github.com/synchain-oss/scvb), another Synchain project
under GPL-3.0-or-later with the same copyright holder. Each such file carries the header line
`Derived from synchain-oss/scvb; see upstream/<area>.json`. The registry in `upstream/*.json` records the upstream
commit and file blob of every derived file; release zips render it into the per-file table of their copy of this
file. Registered so far: `upstream/ci.json` (CI configuration and check scripts).

<!-- /section:scvb-derived -->

## 5. Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH. Cubase, LUNA, Logic Pro, GarageBand, macOS,
Audio Units, Windows, WebView2 and other trademarks belong to their respective owners.
