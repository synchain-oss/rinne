# ADR-0007：许可与合规口径

## Summary (EN)

- **Decision**: own code GPL-3.0-or-later; binaries distributed as a whole under GPLv3, the JUCE parts under AGPLv3 (GPLv3 §13). JUCE is `AGPL-3.0-only`; VST3 SDK 3.7.12 is used under GPLv3 (parts BSD-3-Clause); AudioUnitSDK Apache-2.0; the WebView2 loader is a Microsoft BSD-3-Clause binary; fonts OFL-1.1, IBM Plex renamed.
- **Consequences**: corresponding source is the Rinne tag plus the JUCE tag and commit; no claim of complete source.
- **Compatibility**: changing the JUCE version is a licensing change.

## 状态

已采纳（2026-10-12）。

## 决定

- **自有代码**：GPL-3.0-or-later，版权行 `Copyright 2026 Synchain`，源文件带 SPDX 头。
- **JUCE**：AGPLv3 与商业许可双授权，取 AGPLv3。头注写的是「the terms of the AGPLv3」，没有「or any later version」，SPDX 标 `AGPL-3.0-only`。插件不经网络提供服务，AGPLv3 第 13 条实际不触发，但仍适用于组合作品。
- **VST3 SDK**（随 JUCE 8.0.8，3.7.12）：顶层与 `pluginterfaces/` 是 Steinberg 专有许可与 GPLv3 二选一，取 GPLv3，不签 Steinberg 的协议；`base/` 与 `public.sdk/` 是 BSD-3-Clause，也编进 `.vst3`。
- **二进制**整体按 GPLv3 分发，其中 JUCE 部分适用 AGPLv3（GPLv3 §13）；含 GPLv3-only 与 AGPLv3-only 的部分，不能再授权为「或更高版本」。
- **AudioUnitSDK**：Apache-2.0，只进 `.component`。
- **WebView2 loader**：微软 BSD-3-Clause 的二进制，静态链接进 Windows 包。微软只发布二进制，按 GPLv3 对「系统库」的定义视为 WebView2 系统组件的一部分，不附任何链接例外；因此不说「完整对应源码都在本仓」。macOS 用系统 WKWebView。
- **字体**：OFL-1.1。「Plex」是保留字体名，子集化算修改，按 OFL-1.1 §3 改名为 Rinne Sans / Rinne Mono；上游 OFL.txt 随包分发，保留字体名声明只在那里。
- **对外口径**：「源码 GPL-3.0-or-later；二进制整体按 GPLv3 分发，其中 JUCE 部分适用 AGPLv3（GPLv3 §13）」。关于面板有「查看许可证」，LICENSE 编进插件、离线可看。面板是按 GPLv3 §5(d) 的稳妥做法，不是义务：自有代码的版权人是 Synchain，JUCE 部分适用该条的但书。
- **对应源码**（GPLv3 §6(d)）：INSTALL.txt、Release 正文与打包摘要写 Rinne 的 tag 与 commit、JUCE 的 tag 与 40 位 commit、WebView2 SDK 版本、构建步骤。JUCE 源码在上游服务器上，持续可得的义务仍在分发者。
- **机检**：NOTICES、`LICENSES/`、`third_party/notices/`、`REUSE.toml` 与 `THIRD_PARTY_BY_MODULE` 由 licenses-closure、modules-closure 与 reuse lint 保持闭合。

## 后果

- 升级 JUCE 时同一个 PR 改 `.juce-version`、`.juce-commit`、本 ADR、NOTICES、`LICENSES/` 与 `THIRD_PARTY_BY_MODULE`；JUCE 8.0.11 起 VST3 SDK 改为 MIT。
- 要改 JUCE 时补丁放 `third_party/juce-patches/`，标 `AGPL-3.0-only`，补丁头写修改说明与日期，NOTICES 标明已修改。

## 被否决的备选

- **二进制按「GPLv3 或更高版本」**：含只授权 GPLv3 与 AGPLv3 的部分，做不到。
- **给 WebView2 附链接例外**：会改变本仓许可；按系统库解释并如实说明即可。
- **照搬 SCVB 的 JUCE 标识**：SCVB 写成「或更高版本」，与 JUCE 头注不符。
