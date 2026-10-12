# Upstream notice texts

Verbatim copies of the licence and copyright notices of the components listed in
[`THIRD-PARTY-NOTICES.md`](../../THIRD-PARTY-NOTICES.md). The only change is the line endings, which `.gitattributes`
normalises to LF. Do not edit these files: when a component changes version, replace its notice from the same source
and update the table below in the same pull request. `REUSE.toml` (section `third-party-notices`) records the
copyright and licence of each file; the `licenses-closure` check requires every file here to be referenced by a row
of `THIRD-PARTY-NOTICES.md`.

Files taken from JUCE can be checked offline with `git -C <JUCE checkout> rev-parse <commit>:<path>`, which must
print the blob id below. The other sources are pinned to a commit or a package version.

| File | Source | Blob or pin |
| --- | --- | --- |
| `juce.LICENSE.md` | JUCE 8.0.8 `LICENSE.md` | `91beefb3ad445dc9bcb16da7ee99687f7c68ebd9` |
| `vst3sdk.LICENSE.txt` | JUCE 8.0.8 `modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt` (same blob as `pluginterfaces/LICENSE.txt`) | `f5df1de438030fc6ccf39ff62c8c1a07bddec218` |
| `vst3sdk-base.LICENSE.txt` | JUCE 8.0.8 `modules/juce_audio_processors/format_types/VST3_SDK/base/LICENSE.txt` (same blob as `public.sdk/LICENSE.txt`) | `ec797c8bd323dba3dccbb41d71c184e42f12f60a` |
| `audiounitsdk.LICENSE.txt` | JUCE 8.0.8 `modules/juce_audio_plugin_client/AU/AudioUnitSDK/LICENSE.txt` | `d645695673349e3947e8e5ae42332d0ac3164cd7` |
| `zlib.LICENSE` | JUCE 8.0.8 `modules/juce_core/zip/zlib/LICENSE` | `3a09a35d890271eb2d230ea73bb5ce20aab98d57` |
| `libpng.LICENSE` | JUCE 8.0.8 `modules/juce_graphics/image_formats/pnglib/LICENSE` | `e0c5b531cf548d608e911c43c577cd4ba9a1526e` |
| `libjpeg.README` | JUCE 8.0.8 `modules/juce_graphics/image_formats/jpglib/README` | `86cc20669d6123ee2999fd0c8f6c21641825fda6` |
| `harfbuzz.COPYING` | JUCE 8.0.8 `modules/juce_graphics/fonts/harfbuzz/COPYING` | `1dd917e9f2e72167de69e3b5dadbc4e7db417da1` |
| `sheenbidi.LICENSE` | JUCE 8.0.8 `modules/juce_graphics/unicode/sheenbidi/LICENSE` | `d645695673349e3947e8e5ae42332d0ac3164cd7` |
| `unicode.license.txt` | <https://www.unicode.org/license.txt>, retrieved 2026-10-12 | Unicode License v3 |
| `webview2.LICENSE.txt` | `LICENSE.txt` in the NuGet package `Microsoft.Web.WebView2` 1.0.2957.106 | package sha256 `4c35a54835b63954159eac1d5b7a60ae617a41dbb5b73bfdb11c4870a891080a` |
| `webview2.NOTICE.txt` | `NOTICE.txt` in the same package | as above |
| `catch2.LICENSE.txt` | catchorg/Catch2 tag v3.5.4, `LICENSE.txt` | commit `b5373dadca40b7edc8570cf9470b9b1cb1934d40` |
| `ibm-plex-sans.OFL.txt` | IBM/plex `LICENSE.txt` (byte-identical to google/fonts `ofl/ibmplexsans/OFL.txt`) | commit `763c36ef9117782905ae010056dfbe8fd2653a25` |
| `ibm-plex-mono.OFL.txt` | IBM/plex `LICENSE.txt` (byte-identical to google/fonts `ofl/ibmplexmono/OFL.txt`) | commit `763c36ef9117782905ae010056dfbe8fd2653a25` |
| `space-grotesk.OFL.txt` | floriankarsten/space-grotesk `OFL.txt` (byte-identical to google/fonts `ofl/spacegrotesk/OFL.txt`) | commit `03507d024a01282884232081fc6011c09ff4e849` |
| `noto-sans-sc.OFL.txt` | google/fonts `ofl/notosanssc/OFL.txt` | commit `bd8f81ddb5c74d5c8897b36ad88b440266245103` |

JUCE 8.0.8 is the commit `d6181bde38d858c283c3b7bf699ce6340c050b5d`. The Noto Sans SC text comes from google/fonts
rather than from notofonts/noto-cjk because the font is subset from the google/fonts release, and only that copy of
the licence carries the copyright line with the Reserved Font Name.
